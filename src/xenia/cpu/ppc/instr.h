/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_INSTR_H_
#define XENIA_CPU_PPC_INSTR_H_

#include <xenia/common.h>

#include <string>
#include <vector>


namespace xe {
namespace cpu {
namespace ppc {


// TODO(benvanik): rename these
typedef enum {
  kXEPPCInstrFormatI    = 0,
  kXEPPCInstrFormatB    = 1,
  kXEPPCInstrFormatSC   = 2,
  kXEPPCInstrFormatD    = 3,
  kXEPPCInstrFormatDS   = 4,
  kXEPPCInstrFormatX    = 5,
  kXEPPCInstrFormatXL   = 6,
  kXEPPCInstrFormatXFX  = 7,
  kXEPPCInstrFormatXFL  = 8,
  kXEPPCInstrFormatXS   = 9,
  kXEPPCInstrFormatXO   = 10,
  kXEPPCInstrFormatA    = 11,
  kXEPPCInstrFormatM    = 12,
  kXEPPCInstrFormatMD   = 13,
  kXEPPCInstrFormatMDS  = 14,
  kXEPPCInstrFormatVA   = 15,
  kXEPPCInstrFormatVX   = 16,
  kXEPPCInstrFormatVXR  = 17,
} xe_ppc_instr_format_e;

typedef enum {
  kXEPPCInstrTypeGeneral      = (1 << 0),
  kXEPPCInstrTypeBranch       = (1 << 1),
  kXEPPCInstrTypeBranchCond   = kXEPPCInstrTypeBranch | (1 << 2),
  kXEPPCInstrTypeBranchAlways = kXEPPCInstrTypeBranch | (1 << 3),
  kXEPPCInstrTypeSyscall      = (1 << 4),
} xe_ppc_instr_type_e;

typedef enum {
  kXEPPCInstrFlagReserved     = 0,
} xe_ppc_instr_flag_e;


class InstrType;


static inline int32_t XEEXTS16(uint32_t v) {
  return (int32_t)((int16_t)v);
}
static inline int32_t XEEXTS26(uint32_t v) {
  return v & 0x02000000 ? (int32_t)v | 0xFC000000 : (int32_t)(v);
}
static inline uint64_t XEMASK(uint32_t mstart, uint32_t mstop) {
  // if mstart ≤ mstop then
  //   mask[mstart:mstop] = ones
  //   mask[all other bits] = zeros
  // else
  //   mask[mstart:63] = ones
  //   mask[0:mstop] = ones
  //   mask[all other bits] = zeros
  uint64_t value =
      (UINT64_MAX >> mstart) ^ ((mstop >= 63) ? 0 : UINT64_MAX >> (mstop + 1));
  return mstart <= mstop ? value : ~value;
}


typedef struct {
  InstrType*        type;
  uint32_t          address;

  union {
    uint32_t          code;

    // kXEPPCInstrFormatI
    struct {
      uint32_t        LK      : 1;
      uint32_t        AA      : 1;
      uint32_t        LI      : 24;
      uint32_t                : 6;
    } I;
    // kXEPPCInstrFormatB
    struct {
      uint32_t        LK      : 1;
      uint32_t        AA      : 1;
      uint32_t        BD      : 14;
      uint32_t        BI      : 5;
      uint32_t        BO      : 5;
      uint32_t                : 6;
    } B;

    // kXEPPCInstrFormatSC
    // kXEPPCInstrFormatD
    struct {
      uint32_t        DS      : 16;
      uint32_t        RA      : 5;
      uint32_t        RT      : 5;
      uint32_t                : 6;
    } D;
    // kXEPPCInstrFormatDS
    struct {
      uint32_t                : 2;
      uint32_t        DS      : 14;
      uint32_t        RA      : 5;
      uint32_t        RT      : 5;
      uint32_t                : 6;
    } DS;
    // kXEPPCInstrFormatX
    struct {
      uint32_t        Rc      : 1;
      uint32_t                : 10;
      uint32_t        RB      : 5;
      uint32_t        RA      : 5;
      uint32_t        RT      : 5;
      uint32_t                : 6;
    } X;
    // kXEPPCInstrFormatXL
    struct {
      uint32_t        LK      : 1;
      uint32_t                : 10;
      uint32_t        BB      : 5;
      uint32_t        BI      : 5;
      uint32_t        BO      : 5;
      uint32_t                : 6;
    } XL;
    // kXEPPCInstrFormatXFX
    struct {
      uint32_t                : 1;
      uint32_t                : 10;
      uint32_t        spr     : 10;
      uint32_t        RT      : 5;
      uint32_t                : 6;
    } XFX;
    // kXEPPCInstrFormatXFL
    // kXEPPCInstrFormatXS
    struct {
      uint32_t        Rc      : 1;
      uint32_t        SH5     : 1;
      uint32_t                : 9;
      uint32_t        SH      : 5;
      uint32_t        RA      : 5;
      uint32_t        RT      : 5;
      uint32_t                : 6;
    } XS;
    // kXEPPCInstrFormatXO
    struct {
      uint32_t        Rc      : 1;
      uint32_t                : 9;
      uint32_t        OE      : 1;
      uint32_t        RB      : 5;
      uint32_t        RA      : 5;
      uint32_t        RT      : 5;
      uint32_t                : 6;
    } XO;
    // kXEPPCInstrFormatA
    // kXEPPCInstrFormatM
    struct {
      uint32_t        Rc      : 1;
      uint32_t        ME      : 5;
      uint32_t        MB      : 5;
      uint32_t        SH      : 5;
      uint32_t        RA      : 5;
      uint32_t        RT      : 5;
      uint32_t                : 6;
    } M;
    // kXEPPCInstrFormatMD
    struct {
      uint32_t        Rc      : 1;
      uint32_t        SH5     : 1;
      uint32_t                : 3;
      uint32_t        MB5     : 1;
      uint32_t        MB      : 5;
      uint32_t        SH      : 5;
      uint32_t        RA      : 5;
      uint32_t        RT      : 5;
      uint32_t                : 6;
    } MD;
    // kXEPPCInstrFormatMDS
    struct {
      uint32_t        Rc      : 1;
      uint32_t                : 4;
      uint32_t        MB5     : 1;
      uint32_t        MB      : 5;
      uint32_t        RB      : 5;
      uint32_t        RA      : 5;
      uint32_t        RT      : 5;
      uint32_t                : 6;
    } MDS;
    // kXEPPCInstrFormatVA
    // kXEPPCInstrFormatVX
    // kXEPPCInstrFormatVXR
  };
} InstrData;


typedef struct {
  enum RegisterSet {
    kXER,
    kLR,
    kCTR,
    kCR,    // 0-7
    kFPSCR,
    kGPR,   // 0-31
    kFPR,   // 0-31
    kVMX,   // 0-127
  };

  enum Access {
    kRead       = 1 << 0,
    kWrite      = 1 << 1,
    kReadWrite  = kRead | kWrite,
  };

  RegisterSet set;
  uint32_t    ordinal;
  Access      access;
} InstrRegister;


typedef struct {
  enum OperandType {
    kRegister,
    kImmediate,
  };

  OperandType type;
  union {
    InstrRegister reg;
    struct {
      bool        is_signed;
      uint64_t    value;
      size_t      width;
    } imm;
  };
  char        display[32];
} InstrOperand;


class InstrAccessBits {
public:
  InstrAccessBits() : spr(0), cr(0), gpr(0), fpr(0) {}

  // Bitmasks derived from the accesses to registers.
  // Format is 2 bits for each register, even bits indicating reads and odds
  // indicating writes.
  uint64_t spr;   // fpcsr/ctr/lr/xer
  uint64_t cr;    // cr7/6/5/4/3/2/1/0
  uint64_t gpr;   // r31-0
  uint64_t fpr;   // f31-0

  void Clear();
  void Extend(InstrAccessBits& other);
  void MarkAccess(InstrRegister& reg);
  void Dump(std::string& out_str);
};


class InstrDisasm {
public:
  enum Flags {
    kOE = 1 << 0,
    kRc = 1 << 1,
    kCA = 1 << 2,
    kLR = 1 << 4,
  };

  char      name[16];
  char      info[64];
  std::vector<InstrOperand> operands;
  std::vector<InstrRegister> special_registers;
  InstrAccessBits access_bits;

  void Init(std::string name, std::string info, uint32_t flags);
  void AddLR(InstrRegister::Access access);
  void AddCTR(InstrRegister::Access access);
  void AddCR(uint32_t bf, InstrRegister::Access access);
  void AddRegOperand(InstrRegister::RegisterSet set, uint32_t ordinal,
                     InstrRegister::Access access, std::string display = "");
  void AddSImmOperand(uint64_t value, size_t width, std::string display = "");
  void AddUImmOperand(uint64_t value, size_t width, std::string display = "");
  int Finish();

  void Dump(std::string& str, size_t pad = 8);
};


typedef int (*InstrDisassembleFn)(InstrData& i, InstrDisasm& d);
typedef void* InstrEmitFn;


class InstrType {
public:
  uint32_t    opcode;
  uint32_t    format;   // xe_ppc_instr_format_e
  uint32_t    type;     // xe_ppc_instr_type_e
  uint32_t    flags;    // xe_ppc_instr_flag_e
  char        name[16];

  InstrDisassembleFn disassemble;
  InstrEmitFn        emit;
};

InstrType* GetInstrType(uint32_t code);
int RegisterInstrDisassemble(uint32_t code, InstrDisassembleFn disassemble);
int RegisterInstrEmit(uint32_t code, InstrEmitFn emit);


}  // namespace ppc
}  // namespace cpu
}  // namespace xe


#endif  // XENIA_CPU_PPC_INSTR_H_

