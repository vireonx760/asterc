/* SPDX-License-Identifier: MPL-2.0 */
/* Conservative x86-64/A64/RV64 disassembler.
   Every unsupported/ambiguous encoding is printed verbatim, never guessed.
   No LLVM/Capstone runtime dependency; the oracle tests use Clang separately.
 */
#include "../isa/aarch64_data_contract.h"
#include "../isa/aarch64_scalar_contract.h"
#include "../isa/riscv64_scalar_contract.h"
#include "asterasm/common.h"
#include "asterasm/version.h"
#include "asterdump_coff.h"
#include "asterdump_elf.h"
#include "asterdump_scalable.h"
#include "asterdump_system.h"
#include "asterdump_vector.h"
#include "asterdump_x86.h"
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUMP_MAX_BYTES (64u * 1024u * 1024u)

typedef enum DumpArch { DUMP_A64, DUMP_RV64, DUMP_X86 } DumpArch;
typedef struct DumpOptions {
  DumpArch arch;
  const char *path;
  uint64_t base;
  int arch_set;
  int json;
  int strict;
} DumpOptions;
static void usage(FILE *out) {
  fputs(
      "Usage: asterdump --target x86_64|aarch64|riscv64 [--format text|json] "
      "[--base HEX] [--strict] file.bin|file.o\n"
      "Recognizes conservative subsets of x86-64, A64 and RV64I instructions.\n"
      "Unknown encodings are displayed as .byte (x86) or .word (A64/RV64); "
      "strict mode returns nonzero.\n"
      "Supports ELF64 ET_REL/EXEC/DYN, x64/Arm64 COFF objects and PE32+ "
      "images.\n"
      "ET_REL addresses are synthetic; linked images use sh_addr/p_vaddr.\n",
      out);
}
static int parse_args(int argc, char **argv, DumpOptions *o) {
  for (int k = 1; k < argc; ++k) {
    const char *arg = argv[k];
    if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
      usage(stdout);
      return 2;
    }
    if (!strcmp(arg, "--strict")) {
      o->strict = 1;
      continue;
    }
    if (!strcmp(arg, "--target") && ++k < argc) {
      if (!strcmp(argv[k], "aarch64"))
        o->arch = DUMP_A64;
      else if (!strcmp(argv[k], "riscv64"))
        o->arch = DUMP_RV64;
      else if (!strcmp(argv[k], "x86_64"))
        o->arch = DUMP_X86;
      else
        return 0;
      o->arch_set = 1;
      continue;
    }
    if (!strcmp(arg, "--format") && ++k < argc) {
      if (!strcmp(argv[k], "json"))
        o->json = 1;
      else if (!strcmp(argv[k], "text"))
        o->json = 0;
      else
        return 0;
      continue;
    }
    if (!strcmp(arg, "--base") && ++k < argc) {
      char *end = NULL;
      if (argv[k][0] == '-')
        return 0;
      errno = 0;
      uint64_t base = strtoull(argv[k], &end, 0);
      if (errno || !end || *end || end == argv[k])
        return 0;
      o->base = base;
      continue;
    }
    if (arg[0] == '-' || o->path)
      return 0;
    o->path = arg;
  }
  return o->arch_set && o->path;
}
static int64_t sign_extend(uint32_t x, uint32_t width) {
  uint32_t mask = (UINT32_C(1) << width) - 1u;
  x &= mask;
  return (int64_t)(x ^ (UINT32_C(1) << (width - 1u))) -
         (int64_t)(UINT32_C(1) << (width - 1u));
}
static const char *rv_reg(uint32_t reg) {
  static const char *const names[] = {
      "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
      "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
      "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
  return names[reg & 31u];
}
static void a64_reg(char *out, size_t len, uint32_t reg, int bits, int sp) {
  if (reg == 31u)
    (void)snprintf(out, len, "%s",
                   sp ? (bits == 64 ? "sp" : "wsp")
                      : (bits == 64 ? "xzr" : "wzr"));
  else
    (void)snprintf(out, len, "%c%u", bits == 64 ? 'x' : 'w', reg);
}
/* Decoder pattern masks are narrow by design: unmatched opcode variations,
   relocations, system instructions and extensions are never reinterpreted. */
static int target_address(uint64_t pc, int64_t delta, uint64_t *result) {
  if (delta < 0) {
    uint64_t magnitude = (uint64_t)(-delta);
    if (pc < magnitude)
      return 0;
    *result = pc - magnitude;
  } else {
    if (pc > UINT64_MAX - (uint64_t)delta)
      return 0;
    *result = pc + (uint64_t)delta;
  }
  return 1;
}
static int decode_a64_data(uint32_t w, char *out, size_t n) {
  A64DataDecoded d;
  if (!a64_data_decode(w, &d))
    return 0;
  static const char *const conditions[] = {"eq", "ne", "cs", "cc", "mi", "pl",
                                           "vs", "vc", "hi", "ls", "ge", "lt",
                                           "gt", "le", "al", "nv"};
  static const char *const shifts[] = {"lsl", "lsr", "asr", "ror"};
  static const char *const extends[] = {"uxtb", "uxth", "uxtw", "uxtx",
                                        "sxtb", "sxth", "sxtw", "sxtx"};
  const A64DataForm *f = d.form;
  char dst[12], lhs[12], rhs[12];
  uint32_t rd = w & 31u, rn = (w >> 5u) & 31u, rm = (w >> 16u) & 31u;
  int arithmetic = f->shape == A64_DATA_ADD,
      flag = (f->word32 & 0x20000000u) != 0;
  int sp_dst = arithmetic
                   ? d.mode >= A64_DATA_EXTEND && !flag
                   : f->shape == A64_DATA_LOGIC && d.mode == A64_DATA_MASK &&
                         (f->word32 & 0x60000000u) != 0x60000000u;
  a64_reg(dst, sizeof(dst), rd, (int)d.bits, sp_dst);
  a64_reg(lhs, sizeof(lhs), rn, (int)d.bits,
          arithmetic && d.mode >= A64_DATA_EXTEND);
  uint32_t rhs_bits = arithmetic && d.mode >= A64_DATA_EXTEND &&
                              d.mode < A64_DATA_MASK &&
                              ((d.mode - A64_DATA_EXTEND) & 3u) != 3u
                          ? 32u
                          : d.bits;
  a64_reg(rhs, sizeof(rhs), rm, (int)rhs_bits, 0);
  if (f->shape == A64_DATA_COMPARE) {
    if (w & 0x800u)
      (void)snprintf(rhs, sizeof(rhs), "#%u", rm);
    (void)snprintf(out, n, "%s %s, %s, #%u, %s", f->name, lhs, rhs, w & 15u,
                   conditions[(w >> 12u) & 15u]);
  } else if (f->shape == A64_DATA_SELECT)
    (void)snprintf(out, n, "%s %s, %s, %s, %s", f->name, dst, lhs, rhs,
                   conditions[(w >> 12u) & 15u]);
  else if (d.mode == A64_DATA_MASK)
    (void)snprintf(out, n, "%s %s, %s, #0x%" PRIx64, f->name, dst, lhs, d.mask);
  else if (d.mode == A64_DATA_IMMEDIATE)
    (void)snprintf(out, n, "%s %s, %s, #%u", f->name, dst, lhs,
                   ((w >> 10u) & 4095u) << ((w & 0x00400000u) ? 12u : 0u));
  else
    (void)snprintf(out, n, "%s %s, %s, %s, %s #%u", f->name, dst, lhs, rhs,
                   d.mode >= A64_DATA_EXTEND ? extends[d.mode - A64_DATA_EXTEND]
                                             : shifts[d.mode],
                   d.amount);
  return 1;
}
static int decode_a64_scalar(uint32_t w, char *out, size_t n) {
  const A64ScalarForm *f = a64_scalar_decode(w);
  int bits = w >> 31u ? 64 : 32;
  char dst[12], lhs[12], rhs[12], acc[12];
  a64_reg(dst, sizeof(dst), w & 31u, bits, 0);
  if (!f) {
    uint32_t root = w & UINT32_C(0x7f800000);
    if (root != UINT32_C(0x31000000) && root != UINT32_C(0x71000000))
      return 0;
    a64_reg(lhs, sizeof(lhs), (w >> 5u) & 31u, bits, 1);
    (void)snprintf(out, n, "%s %s, %s, #%u",
                   root == UINT32_C(0x31000000) ? "adds" : "subs", dst, lhs,
                   ((w >> 10u) & 4095u)
                       << ((w & UINT32_C(0x00400000)) ? 12u : 0u));
    return 1;
  }
  a64_reg(lhs, sizeof(lhs), (w >> 5u) & 31u,
          f->shape == A64_SCALAR_LONG ? 32 : bits, 0);
  a64_reg(rhs, sizeof(rhs), (w >> 16u) & 31u,
          f->shape == A64_SCALAR_LONG ? 32 : bits, 0);
  if (f->shape == A64_SCALAR_WIDE)
    (void)snprintf(out, n, "%s %s, #%u, lsl #%u", f->name, dst,
                   (w >> 5u) & 65535u, ((w >> 21u) & 3u) * 16u);
  else if (f->shape == A64_SCALAR_UNARY)
    (void)snprintf(out, n, "%s %s, %s", f->name, dst, lhs);
  else if (f->shape == A64_SCALAR_BITFIELD)
    (void)snprintf(out, n, "%s %s, %s, #%u, #%u", f->name, dst, lhs,
                   (w >> 16u) & 63u, (w >> 10u) & 63u);
  else if (f->shape == A64_SCALAR_EXTRACT)
    (void)snprintf(out, n, "%s %s, %s, %s, #%u", f->name, dst, lhs, rhs,
                   (w >> 10u) & 63u);
  else if (f->shape == A64_SCALAR_FOUR || f->shape == A64_SCALAR_LONG) {
    a64_reg(acc, sizeof(acc), (w >> 10u) & 31u, bits, 0);
    (void)snprintf(out, n, "%s %s, %s, %s, %s", f->name, dst, lhs, rhs, acc);
  } else
    (void)snprintf(out, n, "%s %s, %s, %s", f->name, dst, lhs, rhs);
  return 1;
}
/* Scalar memory, floating transfers and atomics emitted by the backend.
   Keep fixed bits in every mask so adjacent/reserved forms stay unknown. */
static int decode_a64_backend(uint32_t w, char *out, size_t n) {
  unsigned rd = w & 31u, rn = (w >> 5u) & 31u, rm = (w >> 16u) & 31u;
  unsigned size = w >> 30u;
  char dst[12], base[12], src[12];
  uint32_t root = w & UINT32_C(0x3ffffc00);
  const char *name = NULL;
  if (root == UINT32_C(0x08dffc00) || root == UINT32_C(0x089ffc00))
    name = root == UINT32_C(0x08dffc00) ? "ldar" : "stlr";
  else if ((w & UINT32_C(0x3fff7c00)) == UINT32_C(0x085f7c00))
    name = w & UINT32_C(0x8000) ? "ldaxr" : "ldxr";
  else if ((w & UINT32_C(0x3fe07c00)) == UINT32_C(0x08007c00)) {
    a64_reg(dst, sizeof(dst), rd, size == 3u ? 64 : 32, 0);
    a64_reg(base, sizeof(base), rn, 64, 1);
    a64_reg(src, sizeof(src), rm, 32, 0);
    (void)snprintf(out, n, "%s%s %s, %s, [%s]",
                   w & UINT32_C(0x8000) ? "stlxr" : "stxr",
                   size == 0u   ? "b"
                   : size == 1u ? "h"
                                : "",
                   src, dst, base);
    return 1;
  }
  if (name) {
    a64_reg(dst, sizeof(dst), rd, size == 3u ? 64 : 32, 0);
    a64_reg(base, sizeof(base), rn, 64, 1);
    (void)snprintf(out, n, "%s%s %s, [%s]", name,
                   size == 0u   ? "b"
                   : size == 1u ? "h"
                                : "",
                   dst, base);
    return 1;
  }
  if (w == UINT32_C(0xd5033f5f)) {
    (void)snprintf(out, n, "clrex");
    return 1;
  }
  int scaled = (w & UINT32_C(0x3b000000)) == UINT32_C(0x39000000);
  int unscaled = (w & UINT32_C(0x3b200c00)) == UINT32_C(0x38000000);
  if (scaled || unscaled) {
    int vector = (w & UINT32_C(0x04000000)) != 0;
    unsigned opc = (w >> 22u) & 3u, shift = size;
    int load = (opc & 1u) != 0;
    if (!vector) {
      if (opc > 1u)
        return 0;
      a64_reg(dst, sizeof(dst), rd, size == 3u ? 64 : 32, 0);
    } else {
      if (opc >= 2u && size != 0u)
        return 0;
      shift = opc >= 2u ? 4u : size;
      (void)snprintf(dst, sizeof(dst), "%c%u", "bhsdq"[shift], rd);
    }
    a64_reg(base, sizeof(base), rn, 64, 1);
    int64_t offset = scaled ? (int64_t)(((w >> 10u) & 4095u) << shift)
                            : sign_extend((w >> 12u) & 511u, 9u);
    (void)snprintf(out, n, "%s%s %s, [%s, #%" PRId64 "]",
                   scaled ? (load ? "ldr" : "str") : (load ? "ldur" : "stur"),
                   !vector && size < 2u ? (size ? "h" : "b") : "", dst, base,
                   offset);
    return 1;
  }
  if ((w & UINT32_C(0xfffffc1f)) == UINT32_C(0xd61f0000) ||
      (w & UINT32_C(0xfffffc1f)) == UINT32_C(0xd63f0000)) {
    if (rn == 31u)
      return 0;
    (void)snprintf(out, n, "%s x%u", w & UINT32_C(0x00200000) ? "blr" : "br",
                   rn);
    return 1;
  }
  char fp = w & UINT32_C(0x00400000) ? 'd' : 's';
  if ((w & UINT32_C(0xffbffc00)) == UINT32_C(0x1e204000)) {
    (void)snprintf(out, n, "fmov %c%u, %c%u", fp, rd, fp, rn);
    return 1;
  }
  if ((w & UINT32_C(0xffa00c00)) == UINT32_C(0x1e200c00)) {
    static const char *const conditions[] = {"eq", "ne", "cs", "cc", "mi", "pl",
                                             "vs", "vc", "hi", "ls", "ge", "lt",
                                             "gt", "le", "al", "nv"};
    (void)snprintf(out, n, "fcsel %c%u, %c%u, %c%u, %s", fp, rd, fp, rn, fp, rm,
                   conditions[(w >> 12u) & 15u]);
    return 1;
  }
  root = w & UINT32_C(0x7fbffc00);
  int to_fp = root == UINT32_C(0x1e270000) || root == UINT32_C(0x1e220000) ||
              root == UINT32_C(0x1e230000);
  int from_fp = root == UINT32_C(0x1e260000) || root == UINT32_C(0x1e380000) ||
                root == UINT32_C(0x1e390000);
  if (to_fp || from_fp) {
    int bits = w >> 31u ? 64 : 32;
    int move = root == UINT32_C(0x1e270000) || root == UINT32_C(0x1e260000);
    if (move && bits != (fp == 'd' ? 64 : 32))
      return 0;
    a64_reg(dst, sizeof(dst), to_fp ? rn : rd, bits, 0);
    if (to_fp)
      (void)snprintf(out, n, "%s %c%u, %s",
                     move                ? "fmov"
                     : (root & 0x10000u) ? "ucvtf"
                                         : "scvtf",
                     fp, rd, dst);
    else
      (void)snprintf(out, n, "%s %s, %c%u",
                     move                ? "fmov"
                     : (root & 0x10000u) ? "fcvtzu"
                                         : "fcvtzs",
                     dst, fp, rn);
    return 1;
  }
  return 0;
}
static int decode_a64(uint32_t w, uint64_t pc, char *out, size_t n) {
  if (dump_scalable_word(w, out, n))
    return 1;
  if (dump_vector_word(AST_TARGET_ARCH_AARCH64, w, out, n))
    return 1;
  if (dump_system_word(AST_TARGET_ARCH_AARCH64, w, out, n))
    return 1;
  if (decode_a64_backend(w, out, n))
    return 1;
  unsigned rd = w & 31u, rn = (w >> 5u) & 31u, rm = (w >> 16u) & 31u;
  char d[12], a[12], b[12];
  if (w == UINT32_C(0xd65f03c0)) {
    (void)snprintf(out, n, "ret");
    return 1;
  }
  if ((w & UINT32_C(0x7c000000)) == UINT32_C(0x14000000)) {
    uint64_t to;
    if (!target_address(pc, 4 * sign_extend(w & 0x03ffffffu, 26u), &to))
      return 0;
    (void)snprintf(out, n, "%s 0x%" PRIx64, (w & 0x80000000u) ? "bl" : "b", to);
    return 1;
  }
  if ((w & UINT32_C(0xff000010)) == UINT32_C(0x54000000)) {
    static const char *const cond[] = {"eq", "ne", "cs", "cc", "mi",
                                       "pl", "vs", "vc", "hi", "ls",
                                       "ge", "lt", "gt", "le"};
    uint32_t cc = w & 15u;
    uint64_t to;
    if (cc >= 14u ||
        !target_address(pc, 4 * sign_extend((w >> 5u) & 0x7ffffu, 19u), &to))
      return 0;
    (void)snprintf(out, n, "b.%s 0x%" PRIx64, cond[cc], to);
    return 1;
  }
  if ((w & UINT32_C(0x7e000000)) == UINT32_C(0x34000000)) {
    uint64_t to;
    int bits = (w & 0x80000000u) ? 64 : 32;
    a64_reg(d, sizeof(d), rd, bits, 0);
    if (!target_address(pc, 4 * sign_extend((w >> 5u) & 0x7ffffu, 19u), &to))
      return 0;
    (void)snprintf(out, n, "%s %s, 0x%" PRIx64,
                   (w & 0x01000000u) ? "cbnz" : "cbz", d, to);
    return 1;
  }
  /* A64 compiler-produced move-wide immediates. The W-form permits only
     halfwords 0 and 1; reject reserved high-half choices under --strict. */
  if ((w & UINT32_C(0x7f800000)) == UINT32_C(0x12800000) ||
      (w & UINT32_C(0x7f800000)) == UINT32_C(0x52800000) ||
      (w & UINT32_C(0x7f800000)) == UINT32_C(0x72800000)) {
    unsigned sf = w >> 31u, half = (w >> 21u) & 3u;
    if (!sf && half >= 2u)
      return 0;
    const char *name =
        (w & UINT32_C(0x7f800000)) == UINT32_C(0x12800000)   ? "movn"
        : (w & UINT32_C(0x7f800000)) == UINT32_C(0x52800000) ? "movz"
                                                             : "movk";
    a64_reg(d, sizeof(d), rd, sf ? 64 : 32, 0);
    (void)snprintf(out, n, "%s %s, #0x%x, lsl #%u", name, d,
                   (w >> 5u) & 0xffffu, half * 16u);
    return 1;
  }
  /* Compiler-emitted scalar A64 FP arithmetic. Preserve precision and
     reject alternate/unknown opcode fields rather than guessing. */
  if ((w & UINT32_C(0xffa0fc00)) == UINT32_C(0x1e202800) ||
      (w & UINT32_C(0xffa0fc00)) == UINT32_C(0x1e203800) ||
      (w & UINT32_C(0xffa0fc00)) == UINT32_C(0x1e200800) ||
      (w & UINT32_C(0xffa0fc00)) == UINT32_C(0x1e201800)) {
    uint32_t pattern = w & UINT32_C(0xffa0fc00);
    const char *mnemonic = pattern == UINT32_C(0x1e202800)   ? "fadd"
                           : pattern == UINT32_C(0x1e203800) ? "fsub"
                           : pattern == UINT32_C(0x1e200800) ? "fmul"
                                                             : "fdiv";
    char kind = w & UINT32_C(0x00400000) ? 'd' : 's';
    (void)snprintf(out, n, "%s %c%u, %c%u, %c%u", mnemonic, kind, rd, kind, rn,
                   kind, rm);
    return 1;
  }
  if ((w & UINT32_C(0xffbffc00)) == UINT32_C(0x1e214000)) {
    char kind = w & UINT32_C(0x00400000) ? 'd' : 's';
    (void)snprintf(out, n, "fneg %c%u, %c%u", kind, rd, kind, rn);
    return 1;
  }
  if ((w & UINT32_C(0xffa0fc1f)) == UINT32_C(0x1e202000)) {
    char kind = w & UINT32_C(0x00400000) ? 'd' : 's';
    (void)snprintf(out, n, "fcmp %c%u, %c%u", kind, rn, kind, rm);
    return 1;
  }
  if ((w & UINT32_C(0xfffffc00)) == UINT32_C(0x1e22c000) ||
      (w & UINT32_C(0xfffffc00)) == UINT32_C(0x1e624000)) {
    int to_double = (w & UINT32_C(0xfffffc00)) == UINT32_C(0x1e22c000);
    (void)snprintf(out, n, "fcvt %c%u, %c%u", to_double ? 'd' : 's', rd,
                   to_double ? 's' : 'd', rn);
    return 1;
  }
  if ((w & UINT32_C(0x7fe00000)) == UINT32_C(0x1b000000)) {
    int bits = (w & UINT32_C(0x80000000)) ? 64 : 32;
    unsigned ra = (w >> 10u) & 31u;
    a64_reg(d, sizeof(d), rd, bits, 0);
    a64_reg(a, sizeof(a), rn, bits, 0);
    a64_reg(b, sizeof(b), rm, bits, 0);
    const char *name = w & UINT32_C(0x8000) ? "msub" : "madd";
    if (ra == 31u && !(w & UINT32_C(0x8000)))
      (void)snprintf(out, n, "mul %s, %s, %s", d, a, b);
    else {
      char accumulator[12];
      a64_reg(accumulator, sizeof(accumulator), ra, bits, 0);
      (void)snprintf(out, n, "%s %s, %s, %s, %s", name, d, a, b, accumulator);
    }
    return 1;
  }
  if ((w & UINT32_C(0xffe0fc00)) == UINT32_C(0x8b206000) ||
      (w & UINT32_C(0xffe0fc00)) == UINT32_C(0xcb206000)) {
    a64_reg(d, sizeof(d), rd, 64, 1);
    a64_reg(a, sizeof(a), rn, 64, 1);
    a64_reg(b, sizeof(b), rm, 64, 0);
    (void)snprintf(out, n, "%s %s, %s, %s, uxtx",
                   w & UINT32_C(0x40000000) ? "sub" : "add", d, a, b);
    return 1;
  }
  /* Exact A64 unsigned high-half multiplication, 64-bit form only.
     Reserved encodings must not be mistaken for this instruction. */
  if ((w & UINT32_C(0xffe0fc00)) == UINT32_C(0x9bc07c00)) {
    a64_reg(d, sizeof(d), rd, 64, 0);
    a64_reg(a, sizeof(a), rn, 64, 0);
    a64_reg(b, sizeof(b), rm, 64, 0);
    (void)snprintf(out, n, "umulh %s, %s, %s", d, a, b);
    return 1;
  }
  /* UBFM underlies unsigned constant-modulo and logical shifts.
     Do not reinterpret reserved W-form bit patterns as valid operations. */
  /* A64 variable register shifts and signed/unsigned division. The mask
     rejects unsupported operand shifts and opcodes rather than guessing. */
  if ((w & UINT32_C(0x7fe0fc00)) == UINT32_C(0x1ac02000) ||
      (w & UINT32_C(0x7fe0fc00)) == UINT32_C(0x1ac02400) ||
      (w & UINT32_C(0x7fe0fc00)) == UINT32_C(0x1ac02800) ||
      (w & UINT32_C(0x7fe0fc00)) == UINT32_C(0x1ac00800) ||
      (w & UINT32_C(0x7fe0fc00)) == UINT32_C(0x1ac00c00)) {
    uint32_t form = w & UINT32_C(0x7fe0fc00);
    const char *op = form == UINT32_C(0x1ac02000)   ? "lslv"
                     : form == UINT32_C(0x1ac02400) ? "lsrv"
                     : form == UINT32_C(0x1ac02800) ? "asrv"
                     : form == UINT32_C(0x1ac00800) ? "udiv"
                                                    : "sdiv";
    int bits = w & UINT32_C(0x80000000) ? 64 : 32;
    a64_reg(d, sizeof(d), rd, bits, 0);
    a64_reg(a, sizeof(a), rn, bits, 0);
    a64_reg(b, sizeof(b), rm, bits, 0);
    (void)snprintf(out, n, "%s %s, %s, %s", op, d, a, b);
    return 1;
  }
  if ((w & UINT32_C(0x7fe0fc1f)) == UINT32_C(0x6b00001f)) {
    int bits = w & UINT32_C(0x80000000) ? 64 : 32;
    a64_reg(a, sizeof(a), rn, bits, 0);
    a64_reg(b, sizeof(b), rm, bits, 0);
    (void)snprintf(out, n, "cmp %s, %s", a, b);
    return 1;
  }
  if ((w & UINT32_C(0x7ffffc1f)) == UINT32_C(0x7100001f)) {
    /* CMN/CMP immediate-zero alias emitted by CMP_ZERO. */
    int bits = w & UINT32_C(0x80000000) ? 64 : 32;
    a64_reg(a, sizeof(a), rn, bits, 1);
    (void)snprintf(out, n, "cmp %s, #0", a);
    return 1;
  }
  if ((w & UINT32_C(0x7f800000)) == UINT32_C(0x53000000)) {
    unsigned bits = (w & UINT32_C(0x80000000)) ? 64u : 32u;
    unsigned nbit = (w >> 22u) & 1u;
    unsigned immr = (w >> 16u) & 63u, imms = (w >> 10u) & 63u;
    if (nbit != (bits == 64u ? 1u : 0u) || immr >= bits || imms >= bits)
      return 0;
    a64_reg(d, sizeof(d), rd, (int)bits, 0);
    a64_reg(a, sizeof(a), rn, (int)bits, 0);
    if (imms == bits - 1u)
      (void)snprintf(out, n, "lsr %s, %s, #%u", d, a, immr);
    else if (imms + 1u == immr)
      (void)snprintf(out, n, "lsl %s, %s, #%u", d, a, bits - immr);
    else
      (void)snprintf(out, n, "ubfm %s, %s, #%u, #%u", d, a, immr, imms);
    return 1;
  }
  if ((w & UINT32_C(0x7f000000)) == UINT32_C(0x11000000) ||
      (w & UINT32_C(0x7f000000)) == UINT32_C(0x51000000)) {
    int bits = (w & UINT32_C(0x80000000)) ? 64 : 32;
    int sub = (w & UINT32_C(0x40000000)) != 0;
    int shift = (w & UINT32_C(0x00400000)) != 0;
    uint32_t imm = (w >> 10u) & 4095u;
    if (w & UINT32_C(0x00800000))
      return 0;
    a64_reg(d, sizeof(d), rd, bits, 1);
    a64_reg(a, sizeof(a), rn, bits, 1);
    (void)snprintf(out, n, "%s %s, %s, #%" PRIu32, sub ? "sub" : "add", d, a,
                   imm << (shift ? 12u : 0u));
    return 1;
  }
  if ((w & UINT32_C(0x3b000000)) == UINT32_C(0x39000000) &&
      ((w & UINT32_C(0xffc00000)) == UINT32_C(0xf9400000) ||
       (w & UINT32_C(0xffc00000)) == UINT32_C(0xf9000000) ||
       (w & UINT32_C(0xffc00000)) == UINT32_C(0xb9400000) ||
       (w & UINT32_C(0xffc00000)) == UINT32_C(0xb9000000))) {
    int bits = (w & UINT32_C(0x40000000)) ? 64 : 32;
    int load = (w & UINT32_C(0x00400000)) != 0;
    uint32_t offset = ((w >> 10u) & 4095u) * (uint32_t)(bits / 8);
    a64_reg(d, sizeof(d), rd, bits, 0);
    a64_reg(a, sizeof(a), rn, 64, 1);
    (void)snprintf(out, n, "%s %s, [%s, #%" PRIu32 "]", load ? "ldr" : "str", d,
                   a, offset);
    return 1;
  }
  static const struct {
    uint32_t pattern;
    const char *mnemonic;
  } ops[] = {{0x0b000000u, "add"},
             {0x4b000000u, "sub"},
             {0x0a000000u, "and"},
             {0x2a000000u, "orr"},
             {0x4a000000u, "eor"}};
  for (size_t j = 0; j < sizeof(ops) / sizeof(ops[0]); ++j) {
    if ((w & UINT32_C(0x7fe0fc00)) != ops[j].pattern)
      continue;
    int bits = (w & UINT32_C(0x80000000)) ? 64 : 32;
    a64_reg(d, sizeof(d), rd, bits, 0);
    a64_reg(a, sizeof(a), rn, bits, 0);
    a64_reg(b, sizeof(b), rm, bits, 0);
    (void)snprintf(out, n, "%s %s, %s, %s", ops[j].mnemonic, d, a, b);
    return 1;
  }
  return decode_a64_scalar(w, out, n) || decode_a64_data(w, out, n);
}
static int decode_rv64_backend(uint32_t w, char *out, size_t n) {
  unsigned op = w & 127u, rd = (w >> 7u) & 31u;
  unsigned rs1 = (w >> 15u) & 31u, rs2 = (w >> 20u) & 31u;
  unsigned mode = (w >> 12u) & 7u, function = w >> 25u;
  if (op == 0x2fu) {
    if (mode != 2u && mode != 3u)
      return 0;
    static const struct { unsigned function; const char *name; } forms[] = {
        {0u, "amoadd"}, {1u, "amoswap"}, {2u, "lr"}, {3u, "sc"},
        {4u, "amoxor"}, {8u, "amoor"}, {12u, "amoand"},
        {16u, "amomin"}, {20u, "amomax"}, {24u, "amominu"}, {28u, "amomaxu"}};
    unsigned code = w >> 27u, ordering = (w >> 25u) & 3u;
    const char *suffix = ordering == 3u ? ".aqrl" : ordering == 2u ? ".aq" : ordering == 1u ? ".rl" : "";
    for (size_t i = 0; i < sizeof(forms)/sizeof(forms[0]); ++i) {
      if (forms[i].function != code)
        continue;
      if (code == 2u) {
        if (rs2)
          return 0;
        (void)snprintf(out, n, "lr.%c%s %s, (%s)", mode == 3u ? 'd' : 'w', suffix, rv_reg(rd), rv_reg(rs1));
      } else
        (void)snprintf(out, n, "%s.%c%s %s, %s, (%s)", forms[i].name, mode == 3u ? 'd' : 'w', suffix,
                       rv_reg(rd), rv_reg(rs2), rv_reg(rs1));
      return 1;
    }
    return 0;
  }
  if (op != 0x53u)
    return 0;
  char kind = function & 1u ? 'd' : 's';
  unsigned family = function & ~1u;
  static const char *const rounding[] = {"rne", "rtz", "rdn", "rup",
                                         "rmm", NULL,  NULL,  "dyn"};
  if (family == 0u || family == 4u || family == 8u || family == 12u) {
    if (!rounding[mode])
      return 0;
    const char *name = family == 0u   ? "fadd"
                       : family == 4u ? "fsub"
                       : family == 8u ? "fmul"
                                      : "fdiv";
    (void)snprintf(out, n, "%s.%c f%u, f%u, f%u, %s", name, kind, rd, rs1, rs2,
                   rounding[mode]);
    return 1;
  }
  if (family == 0x10u) {
    if (mode > 2u)
      return 0;
    const char *name = mode == 0u ? "fsgnj" : mode == 1u ? "fsgnjn" : "fsgnjx";
    (void)snprintf(out, n, "%s.%c f%u, f%u, f%u", name, kind, rd, rs1, rs2);
    return 1;
  }
  if (family == 0x50u) {
    if (mode > 2u)
      return 0;
    const char *name = mode == 2u ? "feq" : mode == 1u ? "flt" : "fle";
    (void)snprintf(out, n, "%s.%c %s, f%u, f%u", name, kind, rv_reg(rd), rs1,
                   rs2);
    return 1;
  }
  if (family == 0x60u || family == 0x68u) {
    if (rs2 > 3u || !rounding[mode])
      return 0;
    static const char *const integer[] = {"w", "wu", "l", "lu"};
    if (family == 0x60u)
      (void)snprintf(out, n, "fcvt.%s.%c %s, f%u, %s", integer[rs2], kind,
                     rv_reg(rd), rs1, rounding[mode]);
    else
      (void)snprintf(out, n, "fcvt.%c.%s f%u, %s, %s", kind, integer[rs2], rd,
                     rv_reg(rs1), rounding[mode]);
    return 1;
  }
  if ((family == 0x70u || family == 0x78u) && !mode && !rs2) {
    if (family == 0x70u)
      (void)snprintf(out, n, "fmv.x.%c %s, f%u", kind == 's' ? 'w' : 'd',
                     rv_reg(rd), rs1);
    else
      (void)snprintf(out, n, "fmv.%c.x f%u, %s", kind == 's' ? 'w' : 'd', rd,
                     rv_reg(rs1));
    return 1;
  }
  if (family == 0x20u && rs2 == (kind == 's' ? 1u : 0u) && rounding[mode]) {
    (void)snprintf(out, n, "fcvt.%c.%c f%u, f%u, %s", kind,
                   kind == 's' ? 'd' : 's', rd, rs1, rounding[mode]);
    return 1;
  }
  return 0;
}
static int decode_rv64(uint32_t w, uint64_t pc, char *out, size_t n) {
  if (dump_vector_word(AST_TARGET_ARCH_RISCV64, w, out, n))
    return 1;
  if (dump_system_word(AST_TARGET_ARCH_RISCV64, w, out, n))
    return 1;
  if (decode_rv64_backend(w, out, n))
    return 1;
  uint32_t op = w & 127u;
  uint32_t rd = (w >> 7u) & 31u, rs1 = (w >> 15u) & 31u, rs2 = (w >> 20u) & 31u;
  const RvScalarForm *scalar = rv_scalar_decode(w);
  if (scalar) {
    if (scalar->shape == RV_SCALAR_U)
      (void)snprintf(out, n, "%s %s, 0x%05" PRIx32, scalar->name, rv_reg(rd),
                     w >> 12u);
    else if (scalar->shape == RV_SCALAR_N)
      (void)snprintf(out, n, "%s %s, %s", scalar->name, rv_reg(rd),
                     rv_reg(rs1));
    else if (scalar->shape == RV_SCALAR_R)
      (void)snprintf(out, n, "%s %s, %s, %s", scalar->name, rv_reg(rd),
                     rv_reg(rs1), rv_reg(rs2));
    else {
      int64_t immediate =
          scalar->shape == RV_SCALAR_I
              ? sign_extend(w >> 20u, 12u)
              : (int64_t)((w >> 20u) &
                          (scalar->shape == RV_SCALAR_S32 ? 31u : 63u));
      (void)snprintf(out, n, "%s %s, %s, %" PRId64, scalar->name, rv_reg(rd),
                     rv_reg(rs1), immediate);
    }
    return 1;
  }
  if (w == UINT32_C(0x00008067)) {
    (void)snprintf(out, n, "ret");
    return 1;
  }
  if (op == 0x6fu) { /* JAL/J immediate: 20|10:1|11|19:12 */
    uint32_t imm = (((w >> 31u) & 1u) << 20u) | (((w >> 21u) & 1023u) << 1u) |
                   (((w >> 20u) & 1u) << 11u) | (w & 0x000ff000u);
    uint64_t to;
    if (!target_address(pc, sign_extend(imm, 21u), &to))
      return 0;
    (void)snprintf(out, n, "jal %s, 0x%" PRIx64, rv_reg(rd), to);
    return 1;
  }
  if (op == 0x63u) { /* B-type branch */
    static const char *const branch[8] = {"beq", "bne", NULL,   NULL,
                                          "blt", "bge", "bltu", "bgeu"};
    uint32_t f = (w >> 12u) & 7u;
    uint32_t imm = (((w >> 31u) & 1u) << 12u) | (((w >> 7u) & 1u) << 11u) |
                   (((w >> 25u) & 63u) << 5u) | (((w >> 8u) & 15u) << 1u);
    uint64_t to;
    if (!branch[f] || !target_address(pc, sign_extend(imm, 13u), &to))
      return 0;
    (void)snprintf(out, n, "%s %s, %s, 0x%" PRIx64, branch[f], rv_reg(rs1),
                   rv_reg(rs2), to);
    return 1;
  }
  if (op == 0x67u && ((w >> 12u) & 7u) == 0u) {
    (void)snprintf(out, n, "jalr %s, %" PRId64 "(%s)", rv_reg(rd),
                   sign_extend(w >> 20u, 12u), rv_reg(rs1));
    return 1;
  }
  if (op == 0x03u || op == 0x23u || op == 0x07u || op == 0x27u) {
    /* The encoder produces signed/unsigned GPR memory and F/D memory;
       reject reserved funct3 instead of fabricating a load/store. */
    static const char *const loads[8] = {"lb",  "lh",  "lw",  "ld",
                                         "lbu", "lhu", "lwu", NULL};
    static const char *const stores[8] = {"sb", "sh", "sw", "sd",
                                          NULL, NULL, NULL, NULL};
    uint32_t f3 = (w >> 12u) & 7u;
    int fp = op == 0x07u || op == 0x27u;
    int load = op == 0x03u || op == 0x07u;
    const char *name = fp ? (f3 == 2u   ? (load ? "flw" : "fsw")
                             : f3 == 3u ? (load ? "fld" : "fsd")
                                        : NULL)
                          : (load ? loads[f3] : stores[f3]);
    uint32_t imm = load ? w >> 20u : ((w >> 25u) << 5u) | ((w >> 7u) & 31u);
    if (!name)
      return 0;
    if (fp)
      (void)snprintf(out, n, "%s f%u, %" PRId64 "(%s)", name, load ? rd : rs2,
                     sign_extend(imm, 12u), rv_reg(rs1));
    else
      (void)snprintf(out, n, "%s %s, %" PRId64 "(%s)", name,
                     rv_reg(load ? rd : rs2), sign_extend(imm, 12u),
                     rv_reg(rs1));
    return 1;
  }
  return 0;
}
/* Stable machine word presentation shared by raw and ELF inputs. */
static uint32_t load_word(const uint8_t *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
         ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}
static int instruction(const DumpOptions *opt, uint32_t w, uint64_t addr,
                       char *out, size_t len) {
  if (opt->arch == DUMP_A64)
    return decode_a64(w, addr, out, len);
  return decode_rv64(w, addr, out, len);
}
static void json_string(const char *str) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)str; *p; ++p) {
    if (*p == '"' || *p == '\\') {
      putchar('\\');
      putchar(*p);
    } else if (*p < 32u)
      printf("\\u%04x", (unsigned)*p);
    else
      putchar(*p);
  }
  putchar('"');
}
static void relative_assembly(const DumpOptions *o, uint32_t w,
                              const char *decoded, char *out, size_t n) {
  int branch = 0;
  int64_t delta = 0;
  if (o->arch == DUMP_A64) {
    AstPhysicalInstructionInfo scalable;
    if (a64_scalable_decode(w, &scalable) &&
        a64_scalable_word(a64_scalable_form(&scalable), &scalable) != w) {
      (void)snprintf(out, n, ".inst 0x%08" PRIx32, w);
      return;
    }
    A64DataDecoded data;
    if (a64_data_decode(w, &data) && data.mode == A64_DATA_MASK) {
      uint32_t fields = 0u;
      if (a64_logical_encode(data.mask, data.bits, &fields) &&
          fields != (w & 0x007ffc00u)) {
        /* Valid redundant immr high bits cannot be requested with a
           mnemonic mask. Preserve the literal word for reassembly. */
        (void)snprintf(out, n, ".inst 0x%08" PRIx32, w);
        return;
      }
    }
    if ((w & UINT32_C(0x7c000000)) == UINT32_C(0x14000000)) {
      branch = 1;
      delta = 4 * sign_extend(w & UINT32_C(0x03ffffff), 26u);
    } else if ((w & UINT32_C(0xff000010)) == UINT32_C(0x54000000) ||
               (w & UINT32_C(0x7e000000)) == UINT32_C(0x34000000)) {
      branch = 1;
      delta = 4 * sign_extend((w >> 5u) & UINT32_C(0x7ffff), 19u);
    }
  } else if (o->arch == DUMP_RV64) {
    if ((w & 127u) == 0x6fu) {
      branch = 1;
      delta = sign_extend((((w >> 31u) & 1u) << 20u) |
                              (((w >> 21u) & 1023u) << 1u) |
                              (((w >> 20u) & 1u) << 11u) | (w & 0x000ff000u),
                          21u);
    } else if ((w & 127u) == 0x63u) {
      branch = 1;
      delta = sign_extend(
          (((w >> 31u) & 1u) << 12u) | (((w >> 7u) & 1u) << 11u) |
              (((w >> 25u) & 63u) << 5u) | (((w >> 8u) & 15u) << 1u),
          13u);
    }
  }
  const char *last = branch ? strrchr(decoded, ' ') : NULL;
  if (last)
    (void)snprintf(out, n, "%.*s .%+" PRId64, (int)(last - decoded), decoded,
                   delta);
  else
    (void)snprintf(out, n, "%s", decoded);
}
static void json_string(const char *str);
static const char *arch_name(DumpArch arch) {
  return arch == DUMP_A64    ? "aarch64"
         : arch == DUMP_RV64 ? "riscv64"
                             : "x86_64";
}
/* A variable-length x86 decoder must never advance over unknown instruction
   boundaries. On an unsupported opcode the conservative resynchronization
   unit is a single raw byte. */
static size_t show_x86_instruction(const DumpOptions *o, const uint8_t *data,
                                   size_t length, uint64_t addr, int comma,
                                   int relocated, size_t *known_count,
                                   size_t *unknown_count) {
  char decoded[160];
  size_t width = 1u;
  int known =
      dump_x86_decode(data, length, addr, decoded, sizeof(decoded), &width);
  if (!known) {
    (void)snprintf(decoded, sizeof(decoded), ".byte 0x%02x", (unsigned)data[0]);
    ++*unknown_count;
    width = 1u;
  } else
    ++*known_count;
  if (o->json) {
    printf("%s{\"offset\":%" PRIu64 ",\"size\":%zu,\"bytes\":\"",
           comma ? "," : "", addr, width);
    for (size_t j = 0; j < width; ++j)
      printf("%02x", (unsigned)data[j]);
    printf("\",\"mnemonic\":");
    json_string(decoded);
    printf(",\"recognized\":%s,\"relocation_pending\":%s}",
           known ? "true" : "false", relocated ? "true" : "false");
  } else {
    printf("%016" PRIx64 "  ", addr);
    for (size_t j = 0; j < width; ++j)
      printf("%02x ", (unsigned)data[j]);
    printf(" %s%s\n", decoded, relocated ? "  ; relocation pending" : "");
  }
  return width;
}
static void show_instruction(const DumpOptions *o, const uint8_t *data,
                             uint64_t addr, int comma, int relocated,
                             size_t *known_count, size_t *unknown_count) {
  uint32_t w = load_word(data);
  char decoded[160];
  int known = instruction(o, w, addr, decoded, sizeof(decoded));
  if (!known) {
    (void)snprintf(decoded, sizeof(decoded), ".word 0x%08" PRIx32, w);
    ++*unknown_count;
  } else
    ++*known_count;
  if (o->json) {
    printf("%s{\"offset\":%" PRIu64 ",\"word\":\"%08" PRIx32 "\",\"mnemonic\":",
           comma ? "," : "", addr, w);
    json_string(decoded);
    char assembly[160];
    if (known)
      relative_assembly(o, w, decoded, assembly, sizeof(assembly));
    else
      (void)snprintf(assembly, sizeof(assembly), "%s", decoded);
    fputs(",\"assembly\":", stdout);
    json_string(assembly);
    printf(",\"recognized\":%s,\"relocation_pending\":%s}",
           known ? "true" : "false", relocated ? "true" : "false");
  } else {
    printf("%016" PRIx64 "  %02x %02x %02x %02x  %s%s\n", addr,
           (unsigned)data[0], (unsigned)data[1], (unsigned)data[2],
           (unsigned)data[3], decoded,
           relocated ? "  ; relocation pending" : "");
  }
}
static const char *reloc_name(DumpArch arch, uint32_t type) {
  if (arch == DUMP_X86) {
    switch (type) {
    case 0:
      return "R_X86_64_NONE";
    case 1:
      return "R_X86_64_64";
    case 2:
      return "R_X86_64_PC32";
    case 4:
      return "R_X86_64_PLT32";
    case 10:
      return "R_X86_64_32";
    case 11:
      return "R_X86_64_32S";
    default:
      return NULL;
    }
  }
  if (arch == DUMP_A64) {
    switch (type) {
    case 0:
      return "R_AARCH64_NONE";
    case 257:
      return "R_AARCH64_ABS64";
    case 258:
      return "R_AARCH64_ABS32";
    case 275:
      return "R_AARCH64_ADR_PREL_PG_HI21";
    case 277:
      return "R_AARCH64_ADD_ABS_LO12_NC";
    case 282:
      return "R_AARCH64_JUMP26";
    case 283:
      return "R_AARCH64_CALL26";
    default:
      return NULL;
    }
  }
  switch (type) {
  case 0:
    return "R_RISCV_NONE";
  case 1:
    return "R_RISCV_32";
  case 2:
    return "R_RISCV_64";
  case 16:
    return "R_RISCV_BRANCH";
  case 17:
    return "R_RISCV_JAL";
  case 18:
    return "R_RISCV_CALL";
  case 19:
    return "R_RISCV_CALL_PLT";
  case 23:
    return "R_RISCV_PCREL_HI20";
  case 24:
    return "R_RISCV_PCREL_LO12_I";
  case 25:
    return "R_RISCV_PCREL_LO12_S";
  case 51:
    return "R_RISCV_RELAX";
  default:
    return NULL;
  }
}
static int show_raw(const DumpOptions *o, const uint8_t *data, size_t length) {
  size_t known = 0u, unknown = 0u;
  if ((o->arch != DUMP_X86 && length % 4u) || o->base > UINT64_MAX - length) {
    fputs(
        "asterdump: raw BIN requires a multiple of four bytes and valid base\n",
        stderr);
    return 2;
  }
  if (o->json)
    printf("{\"target\":\"%s\",\"instructions\":[", arch_name(o->arch));
  for (size_t i = 0; i < length;) {
    size_t count =
        o->arch == DUMP_X86
            ? show_x86_instruction(o, data + i, length - i, o->base + i,
                                   i != 0u, 0, &known, &unknown)
            : 4u;
    if (o->arch != DUMP_X86)
      show_instruction(o, data + i, o->base + i, i != 0u, 0, &known, &unknown);
    i += count;
  }
  if (o->json)
    printf("],\"recognized\":%zu,\"unknown\":%zu}\n", known, unknown);
  return o->strict && unknown ? 3 : 0;
}
static int show_sectioned(const DumpOptions *o, const uint8_t *data,
                          const DumpElfView *view, const char *container,
                          const char *heading, int coff_format) {
  const DumpElfView elf = *view;
  size_t recognized = 0u, unknown = 0u;
  if (o->json) {
    printf("{\"target\":\"%s\",\"container\":\"%s\",\"program_headers_only\":%"
           "s,\"sections\":[",
           arch_name(o->arch), container,
           elf.program_headers_only ? "true" : "false");
  } else
    printf("%s (%s); %s addresses\n", heading, arch_name(o->arch),
           coff_format
               ? (elf.elf_type == 1u ? "section-relative" : "image virtual")
               : (elf.elf_type == 1u ? "synthetic" : "virtual"));
  if (!o->json && elf.program_headers_only)
    puts("  section table absent; decoding executable PT_LOAD segments only "
         "(no symbols)");
  size_t symbol_cursor = 0u, reloc_cursor = 0u;
  for (size_t s = 0; s < elf.section_count; ++s) {
    const DumpElfSection *sec = &elf.sections[s];
    if (o->base > UINT64_MAX - sec->virtual_offset ||
        o->base + sec->virtual_offset > UINT64_MAX - sec->size) {
      fputs("asterdump: object address overflow\n", stderr);
      return 2;
    }
    uint64_t address = o->base + sec->virtual_offset;
    while (symbol_cursor < elf.symbol_count &&
           elf.symbols[symbol_cursor].section < sec->index)
      ++symbol_cursor;
    while (reloc_cursor < elf.reloc_count &&
           elf.relocs[reloc_cursor].section < sec->index)
      ++reloc_cursor;
    if (o->json) {
      printf("%s{\"name\":", s ? "," : "");
      json_string(sec->name);
      printf(",\"index\":%u,\"address\":%" PRIu64 ",\"size\":%" PRIu64
             ",\"instructions\":[",
             sec->index, address, sec->size);
    } else
      printf("\nsection %s [index %u, base 0x%" PRIx64 "]:\n", sec->name,
             sec->index, address);
    for (uint64_t i = 0; i < sec->size;) {
      while (symbol_cursor < elf.symbol_count &&
             elf.symbols[symbol_cursor].section == sec->index &&
             elf.symbols[symbol_cursor].offset < i)
        ++symbol_cursor;
      while (symbol_cursor < elf.symbol_count &&
             elf.symbols[symbol_cursor].section == sec->index &&
             elf.symbols[symbol_cursor].offset == i) {
        if (!o->json)
          printf("  <%s>:\n", elf.symbols[symbol_cursor].name);
        ++symbol_cursor;
      }
      size_t span = 4u;
      if (o->arch == DUMP_X86) {
        char scratch[160];
        size_t count = 1u;
        if (dump_x86_decode(data + sec->offset + i, (size_t)(sec->size - i),
                            address + i, scratch, sizeof(scratch), &count))
          span = count;
        else
          span = 1u;
      }
      while (reloc_cursor < elf.reloc_count &&
             elf.relocs[reloc_cursor].section == sec->index &&
             elf.relocs[reloc_cursor].offset < i)
        ++reloc_cursor;
      size_t reloc_end = reloc_cursor;
      while (reloc_end < elf.reloc_count &&
             elf.relocs[reloc_end].section == sec->index &&
             elf.relocs[reloc_end].offset - i < span)
        ++reloc_end;
      int relocated = reloc_end != reloc_cursor;
      if (o->arch == DUMP_X86)
        (void)show_x86_instruction(o, data + sec->offset + i,
                                   (size_t)(sec->size - i), address + i,
                                   i != 0u, relocated, &recognized, &unknown);
      else
        show_instruction(o, data + sec->offset + i, address + i, i != 0u,
                         relocated, &recognized, &unknown);
      if (!o->json) {
        for (size_t z = reloc_cursor; z < reloc_end; ++z) {
          const DumpElfReloc *reloc = &elf.relocs[z];
          const char *name =
              coff_format
                  ? dump_coff_reloc_name(
                        o->arch == DUMP_X86 ? 0x8664u : 0xaa64u, reloc->type)
                  : reloc_name(o->arch, reloc->type);
          if (name)
            printf("                  relocation %s", name);
          else
            printf("                  relocation type-%u", reloc->type);
          printf(" -> %s", reloc->symbol);
          if (reloc->has_addend)
            printf(" %+" PRId64, reloc->addend);
          putchar('\n');
        }
      }
      reloc_cursor = reloc_end;
      i += span;
    }
    if (o->json)
      printf("]}");
  }
  if (o->json) {
    printf("],\"symbols\":[");
    for (size_t i = 0; i < elf.symbol_count; ++i) {
      const DumpElfSymbol *sym = &elf.symbols[i];
      printf("%s{\"name\":", i ? "," : "");
      json_string(sym->name);
      printf(",\"section\":%u,\"offset\":%" PRIu64 "}", sym->section,
             sym->offset);
    }
    printf("],\"relocations\":[");
    for (size_t i = 0; i < elf.reloc_count; ++i) {
      const DumpElfReloc *reloc = &elf.relocs[i];
      printf("%s{\"symbol\":", i ? "," : "");
      json_string(reloc->symbol);
      printf(",\"section\":%u,\"offset\":%" PRIu64
             ",\"type\":%u,\"type_name\":",
             reloc->section, reloc->offset, reloc->type);
      const char *name =
          coff_format
              ? dump_coff_reloc_name(o->arch == DUMP_X86 ? 0x8664u : 0xaa64u,
                                     reloc->type)
              : reloc_name(o->arch, reloc->type);
      if (name)
        json_string(name);
      else
        printf("null");
      printf(",\"addend\":%" PRId64 ",\"explicit_addend\":%s}", reloc->addend,
             reloc->has_addend ? "true" : "false");
    }
    printf("],\"recognized\":%zu,\"unknown\":%zu}\n", recognized, unknown);
  }
  return o->strict && unknown ? 3 : 0;
}
static int show_elf(const DumpOptions *o, const uint8_t *data, size_t length) {
  DumpElfView view;
  char error[128] = {0};
  if (!dump_elf_parse(data, length,
                      o->arch == DUMP_A64    ? 183u
                      : o->arch == DUMP_RV64 ? 243u
                                             : 62u,
                      &view, error, sizeof(error))) {
    fprintf(stderr, "asterdump: ELF: %s\n", error);
    return 2;
  }
  const char *container = view.elf_type == 1u   ? "elf64-rel"
                          : view.elf_type == 2u ? "elf64-exec"
                                                : "elf64-dyn";
  const char *heading = view.elf_type == 1u   ? "ELF64 relocatable"
                        : view.elf_type == 2u ? "ELF64 executable"
                                              : "ELF64 shared object";
  int status = show_sectioned(o, data, &view, container, heading, 0);
  dump_elf_destroy(&view);
  return status;
}
static int show_coff(const DumpOptions *o, const uint8_t *data, size_t length) {
  DumpCoffView view;
  char error[128] = {0};
  uint16_t machine = o->arch == DUMP_X86 ? 0x8664u : 0xaa64u;
  if (!dump_coff_parse(data, length, machine, &view, error, sizeof(error))) {
    fprintf(stderr, "asterdump: COFF/PE: %s\n", error);
    return 2;
  }
  int status = show_sectioned(
      o, data, &view.object, view.is_pe ? "pe32+-image" : "coff-object",
      view.is_pe ? "PE32+ executable image" : "COFF relocatable object", 1);
  dump_coff_destroy(&view);
  return status;
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--version")) {
    puts("asterdump " ASTERASM_VERSION_STRING);
    return 0;
  }
  DumpOptions options = {0};
  int status = parse_args(argc, argv, &options);
  if (status == 2)
    return 0;
  if (!status) {
    usage(stderr);
    return 2;
  }
  FILE *in = ast_fopen(options.path, "rb");
  if (!in) {
    perror(options.path);
    return 2;
  }
  if (fseek(in, 0, SEEK_END) || ftell(in) < 0) {
    fclose(in);
    fputs("asterdump: input is not seekable\n", stderr);
    return 2;
  }
  long length = ftell(in);
  if ((uint64_t)length > DUMP_MAX_BYTES) {
    fclose(in);
    fputs("asterdump: input exceeds 64 MiB\n", stderr);
    return 2;
  }
  if (fseek(in, 0, SEEK_SET)) {
    fclose(in);
    return 2;
  }
  uint8_t *data = (uint8_t *)malloc(length ? (size_t)length : 1u);
  if (!data) {
    fclose(in);
    return 2;
  }
  if (fread(data, 1u, (size_t)length, in) != (size_t)length || ferror(in)) {
    free(data);
    fclose(in);
    fputs("asterdump: short read\n", stderr);
    return 2;
  }
  fclose(in);
  int result;
  if (length >= 4 && !memcmp(data, "\177ELF", 4u))
    result = show_elf(&options, data, (size_t)length);
  else if ((length >= 2 && data[0] == 'M' && data[1] == 'Z') ||
           (length >= 2 &&
            (((uint16_t)data[0] | ((uint16_t)data[1] << 8u)) == 0x8664u ||
             ((uint16_t)data[0] | ((uint16_t)data[1] << 8u)) == 0xaa64u)))
    result = show_coff(&options, data, (size_t)length);
  else
    result = show_raw(&options, data, (size_t)length);
  free(data);
  return result;
}
