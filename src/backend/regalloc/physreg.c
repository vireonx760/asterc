#include "regalloc_internal.h"

static MachinePhysRegFile register_file_for_class(AstTargetArch arch,
                                                  AstTargetRegClass rc) {
  switch (rc) {
  case AST_TARGET_REGCLASS_GPR8:
  case AST_TARGET_REGCLASS_GPR16:
  case AST_TARGET_REGCLASS_GPR32:
  case AST_TARGET_REGCLASS_GPR64:
  case AST_TARGET_REGCLASS_GPR_PAIR:
    return MACHINE_PHYSREG_FILE_GPR;
  case AST_TARGET_REGCLASS_FP16:
  case AST_TARGET_REGCLASS_FP32:
  case AST_TARGET_REGCLASS_FP64:
  case AST_TARGET_REGCLASS_FP128:
    return MACHINE_PHYSREG_FILE_FP_VECTOR;
  case AST_TARGET_REGCLASS_VEC64:
  case AST_TARGET_REGCLASS_VEC128:
  case AST_TARGET_REGCLASS_VEC256:
  case AST_TARGET_REGCLASS_VEC512:
  case AST_TARGET_REGCLASS_SCALABLE_VECTOR:
    return arch == AST_TARGET_ARCH_RISCV64 ? MACHINE_PHYSREG_FILE_VECTOR
                                           : MACHINE_PHYSREG_FILE_FP_VECTOR;
  case AST_TARGET_REGCLASS_MASK:
  case AST_TARGET_REGCLASS_PREDICATE:
    return MACHINE_PHYSREG_FILE_MASK_PREDICATE;
  case AST_TARGET_REGCLASS_SPECIAL:
    return MACHINE_PHYSREG_FILE_SPECIAL;
  default:
    return MACHINE_PHYSREG_FILE_INVALID;
  }
}

static int name_is(const char *a, const char *b) {
  if (!a || !b)
    return 0;
  while (*a && *b) {
    unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
    if (x >= 'A' && x <= 'Z')
      x = (unsigned char)(x + ('a' - 'A'));
    if (y >= 'A' && y <= 'Z')
      y = (unsigned char)(y + ('a' - 'A'));
    if (x != y)
      return 0;
  }
  return *a == *b;
}

static uint32_t special_alias_index(AstTargetArch arch, const char *name,
                                    uint32_t fallback) {
  if (!name)
    return fallback;
  if (arch == AST_TARGET_ARCH_X86_64) {
    if (name_is(name, "rip") || name_is(name, "eip"))
      return 0u;
    if (name_is(name, "rflags") || name_is(name, "eflags"))
      return 1u;
  } else if (arch == AST_TARGET_ARCH_AARCH64) {
    if (name_is(name, "sp") || name_is(name, "wsp"))
      return 0u;
    if (name_is(name, "xzr") || name_is(name, "wzr"))
      return 1u;
  }
  return fallback;
}

static uint8_t register_is_reserved(AstTargetArch arch, const char *name) {
  if (!name)
    return 0u;
  switch (arch) {
  case AST_TARGET_ARCH_X86_64:
    return (uint8_t)(name_is(name, "rsp") || name_is(name, "esp") ||
                     name_is(name, "sp") || name_is(name, "spl") ||
                     name_is(name, "rip") || name_is(name, "eip"));
  case AST_TARGET_ARCH_AARCH64:
    return (uint8_t)(name_is(name, "sp") || name_is(name, "wsp") ||
                     name_is(name, "xzr") || name_is(name, "wzr"));
  case AST_TARGET_ARCH_RISCV64:
    return (uint8_t)(name_is(name, "x0") || name_is(name, "zero") ||
                     name_is(name, "x2") || name_is(name, "sp") ||
                     name_is(name, "x3") || name_is(name, "gp") ||
                     name_is(name, "x4") || name_is(name, "tp"));
  default:
    return 1u;
  }
}

int machine_physreg_view(const AstTargetConfig *config, const char *name,
                         MachinePhysRegView *out) {
  AstTargetRegisterInfo ri;
  MachinePhysRegView v;
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  if (!config || !name || !ast_target_parse_register(config, name, &ri) ||
      !ri.valid)
    return 0;
  memset(&v, 0, sizeof(v));
  v.file = register_file_for_class(config->arch, ri.reg_class);
  v.bank = machine_bank_for_class(ri.reg_class);
  v.index =
      ri.special ? special_alias_index(config->arch, name, ri.index) : ri.index;
  v.width = ri.width;
  v.special = ri.special;
  v.reserved = register_is_reserved(config->arch, name);
  if (v.file == MACHINE_PHYSREG_FILE_INVALID || v.index >= 64u)
    return 0;
  /* Alias units are stable target-local register-file/index pairs. Views such
     as rax/eax/ax/al, x0/w0 and xmm0/ymm0/zmm0 intentionally share one unit. */
  v.alias_unit = (uint32_t)v.file * 64u + v.index;
  *out = v;
  return 1;
}

int machine_physreg_alias(const AstTargetConfig *config, const char *lhs,
                          const char *rhs) {
  MachinePhysRegView a, b;
  if (!machine_physreg_view(config, lhs, &a) ||
      !machine_physreg_view(config, rhs, &b))
    return 0;
  return a.alias_unit == b.alias_unit;
}

uint32_t machine_physreg_preserved_bits(AstAbiKind abi,
                                        const MachinePhysRegView *v) {
  if (!v || v->index >= 64u || v->reserved)
    return 0u;
  uint32_t i = v->index;
  if (v->file == MACHINE_PHYSREG_FILE_GPR) {
    switch (abi) {
    case AST_ABI_SYSV_AMD64:
      return (i == 3u || i == 5u || (i >= 12u && i <= 15u)) ? 64u : 0u;
    case AST_ABI_WINDOWS_X64:
      return (i == 3u || i == 5u || i == 6u || i == 7u ||
              (i >= 12u && i <= 15u))
                 ? 64u
                 : 0u;
    case AST_ABI_AAPCS64:
      return i >= 19u && i <= 29u ? 64u : 0u;
    case AST_ABI_RISCV_LP64:
    case AST_ABI_RISCV_LP64F:
    case AST_ABI_RISCV_LP64D:
      return (i == 8u || i == 9u || (i >= 18u && i <= 27u)) ? 64u : 0u;
    default:
      return 0u;
    }
  }
  if (v->file == MACHINE_PHYSREG_FILE_VECTOR) {
    switch (abi) {
    case AST_ABI_RISCV_LP64V:
    case AST_ABI_RISCV_LP64FV:
    case AST_ABI_RISCV_LP64DV:
      return (i >= 1u && i <= 7u) || i >= 24u ? UINT32_MAX : 0u;
    default:
      return 0u;
    }
  }
  if (v->file != MACHINE_PHYSREG_FILE_FP_VECTOR)
    return 0u;
  switch (abi) {
  case AST_ABI_WINDOWS_X64:
    return i >= 6u && i <= 15u ? 128u : 0u;
  case AST_ABI_AAPCS64:
    return i >= 8u && i <= 15u ? 64u : 0u;
  case AST_ABI_AAPCS64_SVE:
    return i >= 8u && i <= 23u ? UINT32_MAX : 0u;
  case AST_ABI_RISCV_LP64F:
  case AST_ABI_RISCV_LP64D:
    return (i == 8u || i == 9u || (i >= 18u && i <= 27u))
               ? (abi == AST_ABI_RISCV_LP64F ? 32u : 64u)
               : 0u;
  default:
    return 0u;
  }
}

int machine_physreg_name(const AstTargetConfig *tc, AstMachineRegBank bank,
                         uint32_t index, uint32_t bits,
                         char out[AST_MACHINE_REGISTER_NAME_CAP]) {
  static const char *x64[] = {"rax", "rcx", "rdx", "rbx",
                              "rsp", "rbp", "rsi", "rdi"};
  static const char *x32[] = {"eax", "ecx", "edx", "ebx",
                              "esp", "ebp", "esi", "edi"};
  static const char *x16[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
  static const char *x8[] = {"al",  "cl",  "dl",  "bl",
                             "spl", "bpl", "sil", "dil"};
  AstTargetRegisterInfo info;
  if (!out)
    return 0;
  out[0] = 0;
  if (!tc || !bits)
    return 0;
  if (tc->arch == AST_TARGET_ARCH_X86_64) {
    if (bank == AST_MACHINE_BANK_GPR && bits <= 64u && index < 16u) {
      if (index < 8u) {
        const char *name = bits <= 8u    ? x8[index]
                           : bits <= 16u ? x16[index]
                           : bits <= 32u ? x32[index]
                                         : x64[index];
        memcpy(out, name, strlen(name) + 1u);
      } else
        (void)snprintf(out, AST_MACHINE_REGISTER_NAME_CAP, "r%u%s", index,
                       bits <= 8u    ? "b"
                       : bits <= 16u ? "w"
                       : bits <= 32u ? "d"
                                     : "");
    } else if ((bank == AST_MACHINE_BANK_FP ||
                bank == AST_MACHINE_BANK_VECTOR) &&
               bits <= 512u && index < 32u) {
      (void)snprintf(out, AST_MACHINE_REGISTER_NAME_CAP, "%s%u",
                     bits <= 128u   ? "xmm"
                     : bits <= 256u ? "ymm"
                                    : "zmm",
                     index);
    } else if (bank == AST_MACHINE_BANK_MASK && bits <= 64u && index < 8u) {
      (void)snprintf(out, AST_MACHINE_REGISTER_NAME_CAP, "k%u", index);
    }
  } else if (tc->arch == AST_TARGET_ARCH_AARCH64 && index < 32u) {
    if (bank == AST_MACHINE_BANK_GPR && index < 31u && bits <= 64u)
      (void)snprintf(out, AST_MACHINE_REGISTER_NAME_CAP, "%s%u",
                     bits <= 32u ? "w" : "x", index);
    else if ((bank == AST_MACHINE_BANK_FP || bank == AST_MACHINE_BANK_VECTOR) &&
             bits <= 128u)
      (void)snprintf(out, AST_MACHINE_REGISTER_NAME_CAP, "%s%u",
                     bits <= 32u   ? "s"
                     : bits <= 64u ? "d"
                                   : "q",
                     index);
  } else if (tc->arch == AST_TARGET_ARCH_RISCV64 && index < 32u) {
    if (bank == AST_MACHINE_BANK_GPR && bits <= 64u)
      (void)snprintf(out, AST_MACHINE_REGISTER_NAME_CAP, "x%u", index);
    else if (bank == AST_MACHINE_BANK_FP && bits <= 64u)
      (void)snprintf(out, AST_MACHINE_REGISTER_NAME_CAP, "f%u", index);
  }
  if (!out[0] || !ast_target_parse_register(tc, out, &info)) {
    out[0] = 0;
    return 0;
  }
  return 1;
}
