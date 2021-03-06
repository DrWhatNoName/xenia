/*
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <xenia/cpu/codegen/emit.h>

#include <xenia/cpu/codegen/function_generator.h>
#include <xenia/cpu/ppc/state.h>


using namespace llvm;
using namespace xe::cpu::codegen;
using namespace xe::cpu::ppc;
using namespace xe::cpu::sdb;


namespace xe {
namespace cpu {
namespace codegen {


int XeEmitIndirectBranchTo(
    FunctionGenerator& g, IRBuilder<>& b, const char* src, uint32_t cia,
    bool lk, uint32_t reg) {
  // TODO(benvanik): run a DFA pass to see if we can detect whether this is
  //     a normal function return that is pulling the LR from the stack that
  //     it set in the prolog. If so, we can omit the dynamic check!

  // NOTE: we avoid spilling registers until we know that the target is not
  // a basic block within this function.

  Value* target;
  switch (reg) {
    case kXEPPCRegLR:
      target = g.lr_value();
      break;
    case kXEPPCRegCTR:
      target = g.ctr_value();
      break;
    default:
      XEASSERTALWAYS();
      return 1;
  }

  // Dynamic test when branching to LR, which is usually used for the return.
  // We only do this if LK=0 as returns wouldn't set LR.
  // Ideally it's a return and we can just do a simple ret and be done.
  // If it's not, we fall through to the full indirection logic.
  if (!lk && reg == kXEPPCRegLR) {
    BasicBlock* next_block = g.GetNextBasicBlock();
    BasicBlock* mismatch_bb = BasicBlock::Create(*g.context(), "lr_mismatch",
                                                 g.gen_fn(), next_block);
    Value* lr_cmp = b.CreateICmpEQ(target, ++(g.gen_fn()->arg_begin()));
    // The return block will spill registers for us.
    b.CreateCondBr(lr_cmp, g.GetReturnBasicBlock(), mismatch_bb);
    b.SetInsertPoint(mismatch_bb);
  }

  // Defer to the generator, which will do fancy things.
  bool likely_local = !lk && reg == kXEPPCRegCTR;
  return g.GenerateIndirectionBranch(cia, target, lk, likely_local);
}

int XeEmitBranchTo(
    FunctionGenerator& g, IRBuilder<>& b, const char* src, uint32_t cia,
    bool lk) {
  // Get the basic block and switch behavior based on outgoing type.
  FunctionBlock* fn_block = g.fn_block();
  switch (fn_block->outgoing_type) {
    case FunctionBlock::kTargetBlock:
    {
      BasicBlock* target_bb = g.GetBasicBlock(fn_block->outgoing_address);
      XEASSERTNOTNULL(target_bb);
      b.CreateBr(target_bb);
      break;
    }
    case FunctionBlock::kTargetFunction:
    {
      // Spill all registers to memory.
      // TODO(benvanik): only spill ones used by the target function? Use
      //     calling convention flags on the function to not spill temp
      //     registers?
      g.SpillRegisters();

      XEASSERTNOTNULL(fn_block->outgoing_function);
      Function* target_fn = g.GetFunction(fn_block->outgoing_function);
      Function::arg_iterator args = g.gen_fn()->arg_begin();
      Value* state_ptr = args;
      BasicBlock* next_bb = g.GetNextBasicBlock();
      if (!lk || !next_bb) {
        // Tail. No need to refill the local register values, just return.
        // We optimize this by passing in the LR from our parent instead of the
        // next instruction. This allows the return from our callee to pop
        // all the way up.
        b.CreateCall2(target_fn, state_ptr, ++args);
        b.CreateRetVoid();
      } else {
        // Will return here eventually.
        // Refill registers from state.
        b.CreateCall2(target_fn, state_ptr, b.getInt64(cia + 4));
        g.FillRegisters();
        b.CreateBr(next_bb);
      }
      break;
    }
    case FunctionBlock::kTargetLR:
    {
      // An indirect jump.
      printf("INDIRECT JUMP VIA LR: %.8X\n", cia);
      return XeEmitIndirectBranchTo(g, b, src, cia, lk, kXEPPCRegLR);
    }
    case FunctionBlock::kTargetCTR:
    {
      // An indirect jump.
      printf("INDIRECT JUMP VIA CTR: %.8X\n", cia);
      return XeEmitIndirectBranchTo(g, b, src, cia, lk, kXEPPCRegCTR);
    }
    default:
    case FunctionBlock::kTargetNone:
      XEASSERTALWAYS();
      return 1;
  }
  return 0;
}


XEDISASMR(bx,           0x48000000, I  )(InstrData& i, InstrDisasm& d) {
  d.Init("b", "Branch", i.I.LK ? InstrDisasm::kLR : 0);
  uint32_t nia;
  if (i.I.AA) {
    nia = XEEXTS26(i.I.LI << 2);
  } else {
    nia = i.address + XEEXTS26(i.I.LI << 2);
  }
  d.AddUImmOperand(nia, 4);
  return d.Finish();
}
XEEMITTER(bx,           0x48000000, I  )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  // if AA then
  //   NIA <- EXTS(LI || 0b00)
  // else
  //   NIA <- CIA + EXTS(LI || 0b00)
  // if LK then
  //   LR <- CIA + 4

  uint32_t nia;
  if (i.I.AA) {
    nia = XEEXTS26(i.I.LI << 2);
  } else {
    nia = i.address + XEEXTS26(i.I.LI << 2);
  }
  if (i.I.LK) {
    g.update_lr_value(b.getInt32(i.address + 4));
  }

  return XeEmitBranchTo(g, b, "bx", i.address, i.I.LK);
}

XEDISASMR(bcx,          0x40000000, B  )(InstrData& i, InstrDisasm& d) {
  // TODO(benvanik): mnemonics
  d.Init("bc", "Branch Conditional", i.B.LK ? InstrDisasm::kLR : 0);
  if (!XESELECTBITS(i.B.BO, 2, 2)) {
    d.AddCTR(InstrRegister::kReadWrite);
  }
  if (!XESELECTBITS(i.B.BO, 4, 4)) {
    d.AddCR(i.B.BI >> 2, InstrRegister::kRead);
  }
  d.AddUImmOperand(i.B.BO, 1);
  d.AddUImmOperand(i.B.BI, 1);
  return d.Finish();
}
XEEMITTER(bcx,          0x40000000, B  )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  // if ¬BO[2] then
  //   CTR <- CTR - 1
  // ctr_ok <- BO[2] | ((CTR[0:63] != 0) XOR BO[3])
  // cond_ok <- BO[0] | (CR[BI+32] ≡ BO[1])
  // if ctr_ok & cond_ok then
  //   if AA then
  //     NIA <- EXTS(BD || 0b00)
  //   else
  //     NIA <- CIA + EXTS(BD || 0b00)
  // if LK then
  //   LR <- CIA + 4

  // NOTE: the condition bits are reversed!
  // 01234 (docs)
  // 43210 (real)

  // TODO(benvanik): this may be wrong and overwrite LRs when not desired!
  // The docs say always, though...
  if (i.B.LK) {
    g.update_lr_value(b.getInt32(i.address + 4));
  }

  Value* ctr_ok = NULL;
  if (XESELECTBITS(i.B.BO, 2, 2)) {
    // Ignore ctr.
  } else {
    // Decrement counter.
    Value* ctr = g.ctr_value();
    ctr = b.CreateSub(ctr, b.getInt64(1));
    g.update_ctr_value(ctr);

    // Ctr check.
    if (XESELECTBITS(i.B.BO, 1, 1)) {
      ctr_ok = b.CreateICmpEQ(ctr, b.getInt64(0));
    } else {
      ctr_ok = b.CreateICmpNE(ctr, b.getInt64(0));
    }
  }

  Value* cond_ok = NULL;
  if (XESELECTBITS(i.B.BO, 4, 4)) {
    // Ignore cond.
  } else {
    Value* cr = g.cr_value(i.B.BI >> 2);
    cr = b.CreateAnd(cr, 1 << (i.B.BI & 3));
    if (XESELECTBITS(i.B.BO, 3, 3)) {
      cond_ok = b.CreateICmpNE(cr, b.getInt64(0));
    } else {
      cond_ok = b.CreateICmpEQ(cr, b.getInt64(0));
    }
  }

  // We do a bit of optimization here to make the llvm assembly easier to read.
  Value* ok = NULL;
  if (ctr_ok && cond_ok) {
    ok = b.CreateAnd(ctr_ok, cond_ok);
  } else if (ctr_ok) {
    ok = ctr_ok;
  } else if (cond_ok) {
    ok = cond_ok;
  }

  // Handle unconditional branches without extra fluff.
  BasicBlock* original_bb = b.GetInsertBlock();
  if (ok) {
    char name[32];
    xesnprintfa(name, XECOUNT(name), "loc_%.8X_bcx", i.address);
    BasicBlock* next_block = g.GetNextBasicBlock();
    BasicBlock* branch_bb = BasicBlock::Create(*g.context(), name, g.gen_fn(),
                                               next_block);

    b.CreateCondBr(ok, branch_bb, next_block);
    b.SetInsertPoint(branch_bb);
  }

  // Note that this occurs entirely within the branch true block.
  uint32_t nia;
  if (i.B.AA) {
    nia = XEEXTS26(i.B.BD << 2);
  } else {
    nia = i.address + XEEXTS26(i.B.BD << 2);
  }
  if (XeEmitBranchTo(g, b, "bcx", i.address, i.B.LK)) {
    return 1;
  }

  b.SetInsertPoint(original_bb);

  return 0;
}


XEDISASMR(bcctrx,       0x4C000420, XL )(InstrData& i, InstrDisasm& d) {
  // TODO(benvanik): mnemonics
  d.Init("bcctr", "Branch Conditional to Count Register",
      i.XL.LK ? InstrDisasm::kLR : 0);
  if (!XESELECTBITS(i.XL.BO, 4, 4)) {
    d.AddCR(i.XL.BI >> 2, InstrRegister::kRead);
  }
  d.AddUImmOperand(i.XL.BO, 1);
  d.AddUImmOperand(i.XL.BI, 1);
  d.AddCTR(InstrRegister::kRead);
  return d.Finish();
}
XEEMITTER(bcctrx,       0x4C000420, XL )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  // cond_ok <- BO[0] | (CR[BI+32] ≡ BO[1])
  // if cond_ok then
  //   NIA <- CTR[0:61] || 0b00
  // if LK then
  //   LR <- CIA + 4

  // NOTE: the condition bits are reversed!
  // 01234 (docs)
  // 43210 (real)

  // TODO(benvanik): this may be wrong and overwrite LRs when not desired!
  // The docs say always, though...
  if (i.XL.LK) {
    g.update_lr_value(b.getInt32(i.address + 4));
  }

  Value* cond_ok = NULL;
  if (XESELECTBITS(i.XL.BO, 4, 4)) {
    // Ignore cond.
  } else {
    Value* cr = g.cr_value(i.XL.BI >> 2);
    cr = b.CreateAnd(cr, 1 << (i.XL.BI & 3));
    if (XESELECTBITS(i.XL.BO, 3, 3)) {
      cond_ok = b.CreateICmpNE(cr, b.getInt64(0));
    } else {
      cond_ok = b.CreateICmpEQ(cr, b.getInt64(0));
    }
  }

  // We do a bit of optimization here to make the llvm assembly easier to read.
  Value* ok = NULL;
  if (cond_ok) {
    ok = cond_ok;
  }

  // Handle unconditional branches without extra fluff.
  BasicBlock* original_bb = b.GetInsertBlock();
  if (ok) {
    char name[32];
    xesnprintfa(name, XECOUNT(name), "loc_%.8X_bcctrx", i.address);
    BasicBlock* next_block = g.GetNextBasicBlock();
    XEASSERTNOTNULL(next_block);
    BasicBlock* branch_bb = BasicBlock::Create(*g.context(), name, g.gen_fn(),
                                               next_block);

    b.CreateCondBr(ok, branch_bb, next_block);
    b.SetInsertPoint(branch_bb);
  }

  // Note that this occurs entirely within the branch true block.
  if (XeEmitBranchTo(g, b, "bcctrx", i.address, i.XL.LK)) {
    return 1;
  }

  b.SetInsertPoint(original_bb);

  return 0;
}

XEDISASMR(bclrx,        0x4C000020, XL )(InstrData& i, InstrDisasm& d) {
  std::string name = "bclr";
  if (i.code == 0x4E800020) {
    name = "blr";
  }
  d.Init(name, "Branch Conditional to Link Register",
      i.XL.LK ? InstrDisasm::kLR : 0);
  if (!XESELECTBITS(i.B.BO, 2, 2)) {
    d.AddCTR(InstrRegister::kReadWrite);
  }
  if (!XESELECTBITS(i.B.BO, 4, 4)) {
    d.AddCR(i.B.BI >> 2, InstrRegister::kRead);
  }
  d.AddUImmOperand(i.XL.BO, 1);
  d.AddUImmOperand(i.XL.BI, 1);
  d.AddLR(InstrRegister::kRead);
  return d.Finish();
}
XEEMITTER(bclrx,        0x4C000020, XL )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  // if ¬BO[2] then
  //   CTR <- CTR - 1
  // ctr_ok <- BO[2] | ((CTR[0:63] != 0) XOR BO[3]
  // cond_ok <- BO[0] | (CR[BI+32] ≡ BO[1])
  // if ctr_ok & cond_ok then
  //   NIA <- LR[0:61] || 0b00
  // if LK then
  //   LR <- CIA + 4

  // NOTE: the condition bits are reversed!
  // 01234 (docs)
  // 43210 (real)

  // TODO(benvanik): this may be wrong and overwrite LRs when not desired!
  // The docs say always, though...
  if (i.XL.LK) {
    g.update_lr_value(b.getInt32(i.address + 4));
  }

  Value* ctr_ok = NULL;
  if (XESELECTBITS(i.XL.BO, 2, 2)) {
    // Ignore ctr.
  } else {
    // Decrement counter.
    Value* ctr = g.ctr_value();
    ctr = b.CreateSub(ctr, b.getInt64(1));

    // Ctr check.
    if (XESELECTBITS(i.XL.BO, 1, 1)) {
      ctr_ok = b.CreateICmpEQ(ctr, b.getInt64(0));
    } else {
      ctr_ok = b.CreateICmpNE(ctr, b.getInt64(0));
    }
  }

  Value* cond_ok = NULL;
  if (XESELECTBITS(i.XL.BO, 4, 4)) {
    // Ignore cond.
  } else {
    Value* cr = g.cr_value(i.XL.BI >> 2);
    cr = b.CreateAnd(cr, 1 << (i.XL.BI & 3));
    if (XESELECTBITS(i.XL.BO, 3, 3)) {
      cond_ok = b.CreateICmpNE(cr, b.getInt64(0));
    } else {
      cond_ok = b.CreateICmpEQ(cr, b.getInt64(0));
    }
  }

  // We do a bit of optimization here to make the llvm assembly easier to read.
  Value* ok = NULL;
  if (ctr_ok && cond_ok) {
    ok = b.CreateAnd(ctr_ok, cond_ok);
  } else if (ctr_ok) {
    ok = ctr_ok;
  } else if (cond_ok) {
    ok = cond_ok;
  }

  // Handle unconditional branches without extra fluff.
  BasicBlock* original_bb = b.GetInsertBlock();
  if (ok) {
    char name[32];
    xesnprintfa(name, XECOUNT(name), "loc_%.8X_bclrx", i.address);
    BasicBlock* next_block = g.GetNextBasicBlock();
    XEASSERTNOTNULL(next_block);
    BasicBlock* branch_bb = BasicBlock::Create(*g.context(), name, g.gen_fn(),
                                               next_block);

    b.CreateCondBr(ok, branch_bb, next_block);
    b.SetInsertPoint(branch_bb);
  }

  // Note that this occurs entirely within the branch true block.
  if (XeEmitBranchTo(g, b, "bclrx", i.address, i.XL.LK)) {
    return 1;
  }

  b.SetInsertPoint(original_bb);

  return 0;
}


// Condition register logical (A-23)

XEEMITTER(crand,        0x4C000202, XL )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

XEEMITTER(crandc,       0x4C000102, XL )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

XEEMITTER(creqv,        0x4C000242, XL )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

XEEMITTER(crnand,       0x4C0001C2, XL )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

XEEMITTER(crnor,        0x4C000042, XL )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

XEEMITTER(cror,         0x4C000382, XL )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

XEEMITTER(crorc,        0x4C000342, XL )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

XEEMITTER(crxor,        0x4C000182, XL )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

XEEMITTER(mcrf,         0x4C000000, XL )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}


// System linkage (A-24)

XEEMITTER(sc,           0x44000002, SC )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}


// Trap (A-25)

int XeEmitTrap(FunctionGenerator& g, IRBuilder<>& b, InstrData& i,
                Value* va, Value* vb, uint32_t TO) {
  // if (a < b) & TO[0] then TRAP
  // if (a > b) & TO[1] then TRAP
  // if (a = b) & TO[2] then TRAP
  // if (a <u b) & TO[3] then TRAP
  // if (a >u b) & TO[4] then TRAP
  // Bits swapped:
  // 01234
  // 43210

  if (!TO) {
    return 0;
  }

  BasicBlock* after_bb = BasicBlock::Create(*g.context(), "", g.gen_fn(),
                                            g.GetNextBasicBlock());
  BasicBlock* trap_bb = BasicBlock::Create(*g.context(), "", g.gen_fn(),
                                           after_bb);

  // Create the basic blocks (so we can chain).
  std::vector<BasicBlock*> bbs;
  if (TO & (1 << 4)) {
    bbs.push_back(BasicBlock::Create(*g.context(), "", g.gen_fn(), trap_bb));
  }
  if (TO & (1 << 3)) {
    bbs.push_back(BasicBlock::Create(*g.context(), "", g.gen_fn(), trap_bb));
  }
  if (TO & (1 << 2)) {
    bbs.push_back(BasicBlock::Create(*g.context(), "", g.gen_fn(), trap_bb));
  }
  if (TO & (1 << 1)) {
    bbs.push_back(BasicBlock::Create(*g.context(), "", g.gen_fn(), trap_bb));
  }
  if (TO & (1 << 0)) {
    bbs.push_back(BasicBlock::Create(*g.context(), "", g.gen_fn(), trap_bb));
  }
  bbs.push_back(after_bb);

  // Jump to the first bb.
  b.CreateBr(bbs.front());

  // Setup each basic block.
  std::vector<BasicBlock*>::iterator it = bbs.begin();
  if (TO & (1 << 4)) {
    // a < b
    BasicBlock* bb = *(it++);
    b.SetInsertPoint(bb);
    Value* cmp = b.CreateICmpSLT(va, vb);
    b.CreateCondBr(cmp, trap_bb, *it);
  }
  if (TO & (1 << 3)) {
    // a > b
    BasicBlock* bb = *(it++);
    b.SetInsertPoint(bb);
    Value* cmp = b.CreateICmpSGT(va, vb);
    b.CreateCondBr(cmp, trap_bb, *it);
  }
  if (TO & (1 << 2)) {
    // a = b
    BasicBlock* bb = *(it++);
    b.SetInsertPoint(bb);
    Value* cmp = b.CreateICmpEQ(va, vb);
    b.CreateCondBr(cmp, trap_bb, *it);
  }
  if (TO & (1 << 1)) {
    // a <u b
    BasicBlock* bb = *(it++);
    b.SetInsertPoint(bb);
    Value* cmp = b.CreateICmpULT(va, vb);
    b.CreateCondBr(cmp, trap_bb, *it);
  }
  if (TO & (1 << 0)) {
    // a >u b
    BasicBlock* bb = *(it++);
    b.SetInsertPoint(bb);
    Value* cmp = b.CreateICmpUGT(va, vb);
    b.CreateCondBr(cmp, trap_bb, *it);
  }

  // Create trap BB.
  b.SetInsertPoint(trap_bb);
  g.SpillRegisters();
  // TODO(benvanik): use @llvm.debugtrap? could make debugging better
  b.CreateCall2(g.gen_module()->getFunction("XeTrap"),
                g.gen_fn()->arg_begin(),
                b.getInt32(i.address));
  b.CreateBr(after_bb);

  // Resume.
  b.SetInsertPoint(after_bb);

  return 0;
}

XEDISASMR(td,           0x7C000088, X  )(InstrData& i, InstrDisasm& d) {
  d.Init("td", "Trap Doubleword", 0);
  d.AddRegOperand(InstrRegister::kGPR, i.X.RA, InstrRegister::kRead);
  d.AddRegOperand(InstrRegister::kGPR, i.X.RB, InstrRegister::kRead);
  return d.Finish();
}
XEEMITTER(td,           0x7C000088, X  )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  // a <- (RA)
  // b <- (RB)
  // if (a < b) & TO[0] then TRAP
  // if (a > b) & TO[1] then TRAP
  // if (a = b) & TO[2] then TRAP
  // if (a <u b) & TO[3] then TRAP
  // if (a >u b) & TO[4] then TRAP
  return XeEmitTrap(g, b, i,
                    g.gpr_value(i.X.RA),
                    g.gpr_value(i.X.RB),
                    i.X.RT);
}

XEDISASMR(tdi,          0x08000000, D  )(InstrData& i, InstrDisasm& d) {
  d.Init("tdi", "Trap Doubleword Immediate", 0);
  d.AddRegOperand(InstrRegister::kGPR, i.D.RA, InstrRegister::kRead);
  return d.Finish();
}
XEEMITTER(tdi,          0x08000000, D  )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  // a <- (RA)
  // if (a < EXTS(SI)) & TO[0] then TRAP
  // if (a > EXTS(SI)) & TO[1] then TRAP
  // if (a = EXTS(SI)) & TO[2] then TRAP
  // if (a <u EXTS(SI)) & TO[3] then TRAP
  // if (a >u EXTS(SI)) & TO[4] then TRAP
  return XeEmitTrap(g, b, i,
                    g.gpr_value(i.D.RA),
                    b.getInt64(XEEXTS16(i.D.DS)),
                    i.D.RT);
}

XEDISASMR(tw,           0x7C000008, X  )(InstrData& i, InstrDisasm& d) {
  d.Init("tw", "Trap Word", 0);
  d.AddRegOperand(InstrRegister::kGPR, i.X.RA, InstrRegister::kRead);
  d.AddRegOperand(InstrRegister::kGPR, i.X.RB, InstrRegister::kRead);
  return d.Finish();
}
XEEMITTER(tw,           0x7C000008, X  )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  // a <- EXTS((RA)[32:63])
  // b <- EXTS((RB)[32:63])
  // if (a < b) & TO[0] then TRAP
  // if (a > b) & TO[1] then TRAP
  // if (a = b) & TO[2] then TRAP
  // if (a <u b) & TO[3] then TRAP
  // if (a >u b) & TO[4] then TRAP
  return XeEmitTrap(g, b, i,
                    b.CreateSExt(b.CreateTrunc(g.gpr_value(i.X.RA),
                                               b.getInt32Ty()),
                                 b.getInt64Ty()),
                    b.CreateSExt(b.CreateTrunc(g.gpr_value(i.X.RB),
                                               b.getInt32Ty()),
                                 b.getInt64Ty()),
                    i.X.RT);
}

XEDISASMR(twi,          0x0C000000, D  )(InstrData& i, InstrDisasm& d) {
  d.Init("twi", "Trap Word Immediate", 0);
  d.AddRegOperand(InstrRegister::kGPR, i.D.RA, InstrRegister::kRead);
  return d.Finish();
}
XEEMITTER(twi,          0x0C000000, D  )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  // a <- EXTS((RA)[32:63])
  // if (a < EXTS(SI)) & TO[0] then TRAP
  // if (a > EXTS(SI)) & TO[1] then TRAP
  // if (a = EXTS(SI)) & TO[2] then TRAP
  // if (a <u EXTS(SI)) & TO[3] then TRAP
  // if (a >u EXTS(SI)) & TO[4] then TRAP
  return XeEmitTrap(g, b, i,
                    b.CreateSExt(b.CreateTrunc(g.gpr_value(i.D.RA),
                                               b.getInt32Ty()),
                                 b.getInt64Ty()),
                    b.getInt64(XEEXTS16(i.D.DS)),
                    i.D.RT);
}


// Processor control (A-26)

XEEMITTER(mfcr,         0x7C000026, X  )(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

XEDISASMR(mfspr,        0x7C0002A6, XFX)(InstrData& i, InstrDisasm& d) {
  d.Init("mfspr", "Move From Special Purpose Register", 0);
  d.AddRegOperand(InstrRegister::kGPR, i.XFX.RT, InstrRegister::kWrite);
  const uint32_t n = ((i.XFX.spr & 0x1F) << 5) | ((i.XFX.spr >> 5) & 0x1F);
  switch (n) {
  case 1:
    d.AddRegOperand(InstrRegister::kXER, 0, InstrRegister::kRead);
    break;
  case 8:
    d.AddRegOperand(InstrRegister::kLR, 0, InstrRegister::kRead);
    break;
  case 9:
    d.AddRegOperand(InstrRegister::kCTR, 0, InstrRegister::kRead);
    break;
  }
  return d.Finish();
}
XEEMITTER(mfspr,        0x7C0002A6, XFX)(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  // n <- spr[5:9] || spr[0:4]
  // if length(SPR(n)) = 64 then
  //   RT <- SPR(n)
  // else
  //   RT <- i32.0 || SPR(n)

  const uint32_t n = ((i.XFX.spr & 0x1F) << 5) | ((i.XFX.spr >> 5) & 0x1F);
  Value* v = NULL;
  switch (n) {
  case 1:
    // XER
    v = g.xer_value();
    break;
  case 8:
    // LR
    v = g.lr_value();
    break;
  case 9:
    // CTR
    v = g.ctr_value();
    break;
  default:
    XEINSTRNOTIMPLEMENTED();
    return 1;
  }

  g.update_gpr_value(i.XFX.RT, v);

  return 0;
}

XEEMITTER(mftb,         0x7C0002E6, XFX)(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

XEEMITTER(mtcrf,        0x7C000120, XFX)(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

XEDISASMR(mtspr,        0x7C0003A6, XFX)(InstrData& i, InstrDisasm& d) {
  d.Init("mtspr", "Move To Special Purpose Register", 0);
  const uint32_t n = ((i.XFX.spr & 0x1F) << 5) | ((i.XFX.spr >> 5) & 0x1F);
  switch (n) {
  case 1:
    d.AddRegOperand(InstrRegister::kXER, 0, InstrRegister::kWrite);
    break;
  case 8:
    d.AddRegOperand(InstrRegister::kLR, 0, InstrRegister::kWrite);
    break;
  case 9:
    d.AddRegOperand(InstrRegister::kCTR, 0, InstrRegister::kWrite);
    break;
  }
  d.AddRegOperand(InstrRegister::kGPR, i.XFX.RT, InstrRegister::kRead);
  return d.Finish();
}
XEEMITTER(mtspr,        0x7C0003A6, XFX)(FunctionGenerator& g, IRBuilder<>& b, InstrData& i) {
  // n <- spr[5:9] || spr[0:4]
  // if length(SPR(n)) = 64 then
  //   SPR(n) <- (RS)
  // else
  //   SPR(n) <- (RS)[32:63]

  Value* v = g.gpr_value(i.XFX.RT);

  const uint32_t n = ((i.XFX.spr & 0x1F) << 5) | ((i.XFX.spr >> 5) & 0x1F);
  switch (n) {
  case 1:
    // XER
    g.update_xer_value(v);
    break;
  case 8:
    // LR
    g.update_lr_value(v);
    break;
  case 9:
    // CTR
    g.update_ctr_value(v);
    break;
  default:
    XEINSTRNOTIMPLEMENTED();
    return 1;
  }

  return 0;
}


void RegisterEmitCategoryControl() {
  XEREGISTERINSTR(bx,           0x48000000);
  XEREGISTERINSTR(bcx,          0x40000000);
  XEREGISTERINSTR(bcctrx,       0x4C000420);
  XEREGISTERINSTR(bclrx,        0x4C000020);
  XEREGISTEREMITTER(crand,        0x4C000202);
  XEREGISTEREMITTER(crandc,       0x4C000102);
  XEREGISTEREMITTER(creqv,        0x4C000242);
  XEREGISTEREMITTER(crnand,       0x4C0001C2);
  XEREGISTEREMITTER(crnor,        0x4C000042);
  XEREGISTEREMITTER(cror,         0x4C000382);
  XEREGISTEREMITTER(crorc,        0x4C000342);
  XEREGISTEREMITTER(crxor,        0x4C000182);
  XEREGISTEREMITTER(mcrf,         0x4C000000);
  XEREGISTEREMITTER(sc,           0x44000002);
  XEREGISTERINSTR(td,           0x7C000088);
  XEREGISTERINSTR(tdi,          0x08000000);
  XEREGISTERINSTR(tw,           0x7C000008);
  XEREGISTERINSTR(twi,          0x0C000000);
  XEREGISTEREMITTER(mfcr,         0x7C000026);
  XEREGISTERINSTR(mfspr,        0x7C0002A6);
  XEREGISTEREMITTER(mftb,         0x7C0002E6);
  XEREGISTEREMITTER(mtcrf,        0x7C000120);
  XEREGISTERINSTR(mtspr,        0x7C0003A6);
}


}  // namespace codegen
}  // namespace cpu
}  // namespace xe
