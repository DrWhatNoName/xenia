/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <xenia/cpu/exec_module.h>

#include <llvm/Linker.h>
#include <llvm/PassManager.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/system_error.h>
#include <llvm/Support/Threading.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include <xenia/cpu/cpu-private.h>
#include <xenia/cpu/llvm_exports.h>
#include <xenia/cpu/sdb.h>
#include <xenia/cpu/codegen/module_generator.h>
#include <xenia/cpu/ppc/instr.h>
#include <xenia/cpu/ppc/state.h>
#include <xenia/cpu/xethunk/xethunk.h>


using namespace llvm;
using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::codegen;
using namespace xe::cpu::sdb;
using namespace xe::kernel;


ExecModule::ExecModule(
    xe_memory_ref memory, shared_ptr<ExportResolver> export_resolver,
    const char* module_name, const char* module_path,
    shared_ptr<llvm::ExecutionEngine>& engine) {
  memory_ = xe_memory_retain(memory);
  export_resolver_ = export_resolver;
  module_name_ = xestrdupa(module_name);
  module_path_ = xestrdupa(module_path);
  engine_ = engine;

  context_ = shared_ptr<LLVMContext>(new LLVMContext());
}

ExecModule::~ExecModule() {
  if (gen_module_) {
    Uninit();
    engine_->removeModule(gen_module_.get());
  }

  xe_free(module_path_);
  xe_free(module_name_);
  xe_memory_release(memory_);
}

int ExecModule::PrepareXex(xe_xex2_ref xex) {
  sdb_ = shared_ptr<sdb::SymbolDatabase>(
      new sdb::XexSymbolDatabase(memory_, export_resolver_.get(), xex));

  code_addr_low_ = 0;
  code_addr_high_ = 0;
  const xe_xex2_header_t* header = xe_xex2_get_header(xex);
  for (size_t n = 0, i = 0; n < header->section_count; n++) {
    const xe_xex2_section_t* section = &header->sections[n];
    const size_t start_address =
        header->exe_address + (i * xe_xex2_section_length);
    const size_t end_address =
        start_address + (section->info.page_count * xe_xex2_section_length);
    if (section->info.type == XEX_SECTION_CODE) {
      code_addr_low_ = MIN(code_addr_low_, start_address);
      code_addr_high_ = MAX(code_addr_high_, end_address);
    }
    i += section->info.page_count;
  }

  int result_code = Prepare();
  if (result_code) {
    return result_code;
  }

  // Import variables.
  // TODO??

  return 0;
}

int ExecModule::PrepareRawBinary(uint32_t start_address, uint32_t end_address) {
  sdb_ = shared_ptr<sdb::SymbolDatabase>(
      new sdb::RawSymbolDatabase(memory_, export_resolver_.get(),
                                 start_address, end_address));

  code_addr_low_ = start_address;
  code_addr_high_ = end_address;

  return Prepare();
}

int ExecModule::Prepare() {
  int result_code = 1;
  std::string error_message;

  char file_name[XE_MAX_PATH];

  OwningPtr<MemoryBuffer> shared_module_buffer;
  auto_ptr<Module> shared_module;
  auto_ptr<raw_ostream> outs;

  PassManager pm;
  PassManagerBuilder pmb;

  // TODO(benvanik): embed the bc file into the emulator.
  const char *thunk_path = "src/xenia/cpu/xethunk/xethunk.bc";

  // Calculate a cache path based on the module, the CPU version, and other
  // bits.
  // TODO(benvanik): cache path calculation.
  //const char *cache_path = "build/generated.bc";

  // Check the cache to see if the bitcode exists.
  // If it does, load that module directly. In the future we could also cache
  // on linked binaries but that requires more safety around versioning.
  // TODO(benvanik): check cache for module bitcode and load.
  // if (path_exists(cache_key)) {
  //   exec_module = load_bitcode(cache_key);
  //   sdb = load_symbol_table(cache_key);
  // }

  // If not found in cache, generate a new module.
  if (!gen_module_.get()) {
    // Load shared bitcode files.
    // These contain globals and common thunk code that are used by the
    // generated code.
    XEEXPECTZERO(MemoryBuffer::getFile(thunk_path, shared_module_buffer));
    shared_module = auto_ptr<Module>(ParseBitcodeFile(
        &*shared_module_buffer, *context_, &error_message));
    XEEXPECTNOTNULL(shared_module.get());

    // Analyze the module and add its symbols to the symbol database.
    XEEXPECTZERO(sdb_->Analyze());

    // Load a specified module map and diff.
    if (FLAGS_load_module_map.size()) {
      sdb_->ReadMap(FLAGS_load_module_map.c_str());
    }

    // Dump the symbol database.
    if (FLAGS_dump_module_map) {
      xesnprintfa(file_name, XECOUNT(file_name),
                  "%s%s.map", FLAGS_dump_path.c_str(), module_name_);
      sdb_->WriteMap(file_name);
    }

    // Initialize the module.
    gen_module_ = shared_ptr<Module>(
        new Module(module_name_, *context_.get()));
    // TODO(benavnik): addModuleFlag?

    // Inject globals.
    // This should be done ASAP to ensure that JITed functions can use the
    // constant addresses.
    XEEXPECTZERO(InjectGlobals());

    // Link shared module into generated module.
    // This gives us a single module that we can optimize and prevents the need
    // for foreward declarations.
    Linker::LinkModules(gen_module_.get(), shared_module.get(), 0,
                        &error_message);

    // Build the module from the source code.
    codegen_ = auto_ptr<ModuleGenerator>(new ModuleGenerator(
        memory_, export_resolver_.get(), module_name_, module_path_,
        sdb_.get(), context_.get(), gen_module_.get(),
        engine_.get()));
    XEEXPECTZERO(codegen_->Generate());

    // Write to cache.
    // TODO(benvanik): cache stuff

    // Dump pre-optimized module to disk.
    if (FLAGS_dump_module_bitcode) {
      xesnprintfa(file_name, XECOUNT(file_name),
                  "%s%s-preopt.bc", FLAGS_dump_path.c_str(), module_name_);
      outs = auto_ptr<raw_ostream>(new raw_fd_ostream(
          file_name, error_message, raw_fd_ostream::F_Binary));
      XEEXPECTTRUE(error_message.empty());
      WriteBitcodeToFile(gen_module_.get(), *outs);
    }
  }

  // Link optimizations.
  XEEXPECTZERO(gen_module_->MaterializeAllPermanently(&error_message));

  // Reset target triple (ignore what's in xethunk).
  gen_module_->setTargetTriple(llvm::sys::getDefaultTargetTriple());

  // Run full module optimizations.
  pm.add(new DataLayout(gen_module_.get()));
  if (FLAGS_optimize_ir_modules) {
    pm.add(createVerifierPass());
    pmb.OptLevel      = 3;
    pmb.SizeLevel     = 0;
    pmb.Inliner       = createFunctionInliningPass();
    pmb.Vectorize     = true;
    pmb.LoopVectorize = true;
    pmb.populateModulePassManager(pm);
    pmb.populateLTOPassManager(pm, false, true);
  }
  pm.add(createVerifierPass());
  pm.run(*gen_module_);

  // Dump post-optimized module to disk.
  if (FLAGS_optimize_ir_modules && FLAGS_dump_module_bitcode) {
    xesnprintfa(file_name, XECOUNT(file_name),
                "%s%s.bc", FLAGS_dump_path.c_str(), module_name_);
    outs = auto_ptr<raw_ostream>(new raw_fd_ostream(
        file_name, error_message, raw_fd_ostream::F_Binary));
    XEEXPECTTRUE(error_message.empty());
    WriteBitcodeToFile(gen_module_.get(), *outs);
  }

  // TODO(benvanik): experiment with LLD to see if we can write out a dll.

  // Initialize the module.
  XEEXPECTZERO(Init());

  // Force JIT of all functions.
  // for (Module::iterator it = gen_module_->begin(); it != gen_module_->end();
  //      ++it) {
  //   Function* fn = it;
  //   if (!fn->isDeclaration()) {
  //     engine_->getPointerToFunction(fn);
  //   }
  // }

  result_code = 0;
XECLEANUP:
  return result_code;
}

int ExecModule::InjectGlobals() {
  LLVMContext& context = *context_.get();
  const DataLayout* dl = engine_->getDataLayout();
  Type* intPtrTy = dl->getIntPtrType(context);
  Type* int8PtrTy = PointerType::getUnqual(Type::getInt8Ty(context));
  GlobalVariable* gv;

  // xe_memory_base
  // This is the base void* pointer to the memory space.
  gv = new GlobalVariable(
      *gen_module_,
      int8PtrTy,
      true,
      GlobalValue::ExternalLinkage,
      0,
      "xe_memory_base");
  // Align to 64b - this makes SSE faster.
  gv->setAlignment(64);
  gv->setInitializer(ConstantExpr::getIntToPtr(
      ConstantInt::get(intPtrTy, (uintptr_t)xe_memory_addr(memory_, 0)),
      int8PtrTy));

  // xe_function_table
  std::vector<Type*> exec_ty_args;
  exec_ty_args.push_back(PointerType::get(IntegerType::get(context, 8), 0));
  exec_ty_args.push_back(IntegerType::get(context, 32));
  // void fn(i8* ppc_state, int32 lr)
  FunctionType* exec_ty = FunctionType::get(
      Type::getVoidTy(context), exec_ty_args, false);
  // exec_ty**
  Type* function_table_ty = PointerType::get(PointerType::get(exec_ty, 0), 0);
  gv = new GlobalVariable(
      *gen_module_,
      function_table_ty,
      true,
      GlobalValue::ExternalLinkage,
      0,
      "xe_function_table");
  gv->setAlignment(8);

  size_t slot_count = (code_addr_high_ - code_addr_low_) / 4;
  void** function_table = xe_calloc(slot_count * sizeof(void*));
  gv->setInitializer(ConstantExpr::getIntToPtr(
      ConstantInt::get(intPtrTy, (uintptr_t)xe_memory_addr(memory_, 0)),
      int8PtrTy));

  SetupLlvmExports(gen_module_.get(), dl, engine_.get());

  return 0;
}

int ExecModule::Init() {
  // Setup all kernel variables.
  std::vector<VariableSymbol*> variables;
  if (sdb_->GetAllVariables(variables)) {
    return 1;
  }
  uint8_t* mem = xe_memory_addr(memory_, 0);
  for (std::vector<VariableSymbol*>::iterator it = variables.begin();
       it != variables.end(); ++it) {
    VariableSymbol* var = *it;
    if (!var->kernel_export) {
      continue;
    }
    KernelExport* kernel_export = var->kernel_export;

    // Grab, if available.
    uint32_t* slot = (uint32_t*)(mem + var->address);
    if (kernel_export->type == KernelExport::Function) {
      // Not exactly sure what this should be...
      // TODO(benvanik): find out what import variables are.
    } else {
      if (kernel_export->is_implemented) {
        // Implemented - replace with pointer.
        *slot = XESWAP32BE(kernel_export->variable_ptr);
      } else {
        // Not implemented - write with a dummy value.
        *slot = XESWAP32BE(0xDEADBEEF);
        XELOGCPU("WARNING: imported a variable with no value: %s",
                 kernel_export->name);
      }
    }
  }

  // Run static initializers. I'm not sure we'll have any, but who knows.
  engine_->runStaticConstructorsDestructors(gen_module_.get(), false);

  // Grab the init function and call it.
  Function* xe_module_init = gen_module_->getFunction("xe_module_init");
  std::vector<GenericValue> args;
  GenericValue ret = engine_->runFunction(xe_module_init, args);

  return static_cast<int>(ret.IntVal.getSExtValue());
}

int ExecModule::Uninit() {
  // Grab function and call it.
  Function* xe_module_uninit = gen_module_->getFunction("xe_module_uninit");
  std::vector<GenericValue> args;
  engine_->runFunction(xe_module_uninit, args);

  // Run static destructors.
  engine_->runStaticConstructorsDestructors(gen_module_.get(), true);

  return 0;
}

int ExecModule::Execute(uint32_t address, xe_ppc_state* ppc_state) {
  if (address < code_addr_low_ ||
      address > code_addr_high_) {
    return 1;
  }

  // This could be set to anything to give us a unique identifier to track
  // re-entrancy/etc.
  uint32_t lr = 0xBEBEBEBE;

  // Setup registers.
  ppc_state->lr = lr;

  // Args:
  // - i8* state
  // - i64 lr

  std::vector<GenericValue> args;
  args.push_back(PTOGV(ppc_state));
  GenericValue lr_arg;
  lr_arg.IntVal = APInt(64, lr);
  args.push_back(lr_arg);
  GenericValue ret = engine_->runFunction(f, args);
  return (uint32_t)ret.IntVal.getSExtValue();

  // Faster, somewhat.
  // Messes with the stack in such a way as to cause Xcode to behave oddly.
  // typedef void (*fnptr)(xe_ppc_state_t*, uint64_t);
  // fnptr ptr = (fnptr)engine_->getPointerToFunction(f);
  // ptr(ppc_state, lr);

  return 0;
}

void ExecModule::Dump() {
  sdb_->Dump(stdout);
}
