/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <xenia/cpu/processor.h>

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/ExecutionEngine/Interpreter.h>
#include <llvm/ExecutionEngine/JIT.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>

#include <xenia/cpu/codegen/emit.h>


using namespace llvm;
using namespace xe;
using namespace xe::cpu;
using namespace xe::kernel;


namespace {
  void InitializeIfNeeded();
  void CleanupOnShutdown();

  void InitializeIfNeeded() {
    static bool has_initialized = false;
    if (has_initialized) {
      return;
    }
    has_initialized = true;

    // TODO(benvanik): only do this once
    LLVMLinkInInterpreter();
    LLVMLinkInJIT();
    InitializeNativeTarget();

    llvm_start_multithreaded();

    // TODO(benvanik): only do this once
    codegen::RegisterEmitCategoryALU();
    codegen::RegisterEmitCategoryControl();
    codegen::RegisterEmitCategoryFPU();
    codegen::RegisterEmitCategoryMemory();

    atexit(CleanupOnShutdown);
  }

  void CleanupOnShutdown() {
    llvm_shutdown();
  }
}


Processor::Processor(xe_pal_ref pal, xe_memory_ref memory) {
  pal_ = xe_pal_retain(pal);
  memory_ = xe_memory_retain(memory);

  InitializeIfNeeded();
}

Processor::~Processor() {
  // Cleanup all modules.
  for (std::vector<ExecModule*>::iterator it = modules_.begin();
       it != modules_.end(); ++it) {
    delete *it;
  }

  engine_.reset();

  xe_memory_release(memory_);
  xe_pal_release(pal_);
}

xe_pal_ref Processor::pal() {
  return xe_pal_retain(pal_);
}

xe_memory_ref Processor::memory() {
  return xe_memory_retain(memory_);
}

int Processor::Setup() {
  XEASSERTNULL(engine_);

  dummy_context_ = auto_ptr<LLVMContext>(new LLVMContext());
  Module* dummy_module = new Module("dummy", *dummy_context_.get());

  std::string error_message;

  EngineBuilder builder(dummy_module);
  builder.setEngineKind(EngineKind::JIT);
  builder.setErrorStr(&error_message);
  builder.setOptLevel(CodeGenOpt::None);
  //builder.setOptLevel(CodeGenOpt::Aggressive);
  //builder.setTargetOptions();
  builder.setAllocateGVsWithCode(false);
  //builder.setUseMCJIT(true);

  engine_ = shared_ptr<ExecutionEngine>(builder.create());
  if (!engine_) {
    return 1;
  }

  return 0;
}

int Processor::LoadBinary(const xechar_t* path, uint32_t start_address,
                          shared_ptr<ExportResolver> export_resolver) {
  ExecModule* exec_module = NULL;
  const xechar_t* name = xestrrchr(path, '/') + 1;

  // TODO(benvanik): map file from filesystem
  xe_mmap_ref mmap = xe_mmap_open(pal_, kXEFileModeRead, path, 0, 0);
  if (!mmap) {
    return NULL;
  }
  void* addr = xe_mmap_get_addr(mmap);
  size_t length = xe_mmap_get_length(mmap);

  int result_code = 1;

  XEEXPECTZERO(xe_copy_memory(xe_memory_addr(memory_, start_address),
                              xe_memory_get_length(memory_),
                              addr, length));

  // Prepare the module.
  char name_a[XE_MAX_PATH];
  XEEXPECTTRUE(xestrnarrow(name_a, XECOUNT(name_a), name));
  char path_a[XE_MAX_PATH];
  XEEXPECTTRUE(xestrnarrow(path_a, XECOUNT(path_a), path));

  exec_module = new ExecModule(
      memory_, export_resolver, name_a, path_a, engine_);

  if (exec_module->PrepareRawBinary(start_address,
                                    start_address + (uint32_t)length)) {
    delete exec_module;
    return 1;
  }

  modules_.push_back(exec_module);

  exec_module->Dump();

  result_code = 0;
XECLEANUP:
  if (result_code) {
    delete exec_module;
  }
  xe_mmap_release(mmap);
  return result_code;
}

int Processor::PrepareModule(const char* name, const char* path,
                             xe_xex2_ref xex,
                             shared_ptr<ExportResolver> export_resolver) {
  ExecModule* exec_module = new ExecModule(
      memory_, export_resolver, name, path,
      engine_);

  if (exec_module->PrepareXex(xex)) {
    delete exec_module;
    return 1;
  }

  modules_.push_back(exec_module);

  return 0;
}

uint32_t Processor::CreateCallback(void (*callback)(void* data), void* data) {
  // TODO(benvanik): implement callback creation.
  return 0;
}

ThreadState* Processor::AllocThread(uint32_t stack_size,
                                    uint32_t thread_state_address) {
  ThreadState* thread_state = new ThreadState(
      this, stack_size, thread_state_address);
  return thread_state;
}

void Processor::DeallocThread(ThreadState* thread_state) {
  delete thread_state;
}

int Processor::Execute(ThreadState* thread_state, uint32_t address) {
  xe_ppc_state_t* ppc_state = thread_state->ppc_state();

  // TODO(benvanik): faster search of containing module.
  for (std::vector<ExecModule*>::iterator it = modules_.begin();
       it != modules_.end(); ++it) {
    if (!it->Execute(address, ppc_state)) {
      return 0;
    }
  }

  return 1;
}

uint64_t Processor::Execute(ThreadState* thread_state, uint32_t address,
                            uint64_t arg0) {
  xe_ppc_state_t* ppc_state = thread_state->ppc_state();
  ppc_state->r[3] = arg0;
  if (Execute(thread_state, address)) {
    return 0xDEADBABE;
  }
  return ppc_state->r[3];
}
