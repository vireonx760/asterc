#include "../core/parallel.h"
#include "target_internal.h"
#include <stdarg.h>

static uint32_t width_class_bits(uint32_t bits) {
  if (bits <= 8u)
    return 8u;
  if (bits <= 16u)
    return 16u;
  if (bits <= 32u)
    return 32u;
  return 64u;
}

static AstTargetRegClass gpr_class(uint32_t bits) {
  if (bits <= 8u)
    return AST_TARGET_REGCLASS_GPR8;
  if (bits <= 16u)
    return AST_TARGET_REGCLASS_GPR16;
  if (bits <= 32u)
    return AST_TARGET_REGCLASS_GPR32;
  return AST_TARGET_REGCLASS_GPR64;
}

int target_name_eq(AstName name, const char *text) {
  if (!text)
    return 0;
  size_t n = strlen(text);
  return name.text && (size_t)name.span.length == n &&
         memcmp(name.text, text, n) == 0;
}

int target_ascii_ieq(const char *a, const char *b) {
  if (!a || !b)
    return 0;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
      return 0;
    ++a;
    ++b;
  }
  return *a == 0 && *b == 0;
}

static char *target_owned_message(AstTargetProgram *program, const char *text) {
  size_t n = strlen(text) + 1u;
  char *copy = (char *)malloc(n);
  if (!copy)
    return NULL;
  memcpy(copy, text, n);
  if (program->owned_message_count == program->owned_message_capacity) {
    size_t cap = program->owned_message_capacity
                     ? program->owned_message_capacity * 2u
                     : 16u;
    char **p = (char **)realloc(program->owned_messages, cap * sizeof(*p));
    if (!p) {
      free(copy);
      return NULL;
    }
    program->owned_messages = p;
    program->owned_message_capacity = cap;
  }
  program->owned_messages[program->owned_message_count++] = copy;
  return copy;
}

void target_diag(AstTargetProgram *program, AstTargetDiagCode code,
                 AstDiagSeverity severity, AstSpan span,
                 uint32_t function_index, AstMirInstId instruction,
                 const char *fmt, ...) {
  if (!program || program->diagnostic_count >= program->config.max_diagnostics)
    return;
  char buffer[512];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(buffer, sizeof(buffer), fmt, ap);
  va_end(ap);
  if (program->diagnostic_count == program->diagnostic_capacity) {
    size_t cap =
        program->diagnostic_capacity ? program->diagnostic_capacity * 2u : 16u;
    AstTargetDiagnostic *p =
        (AstTargetDiagnostic *)realloc(program->diagnostics, cap * sizeof(*p));
    if (!p)
      return;
    program->diagnostics = p;
    program->diagnostic_capacity = cap;
  }
  char *msg = target_owned_message(program, buffer);
  if (!msg)
    return;
  AstTargetDiagnostic *d = &program->diagnostics[program->diagnostic_count++];
  d->code = code;
  d->severity = severity;
  d->span = span;
  d->function_index = function_index;
  d->instruction = instruction;
  d->message = msg;
  if (severity == AST_DIAG_ERROR)
    program->stats.error_count++;
  else if (severity == AST_DIAG_WARNING)
    program->stats.warning_count++;
}

AstTargetConfig ast_target_config_default(AstTargetArch arch) {
  AstTargetConfig c;
  memset(&c, 0, sizeof(c));
  c.arch = arch;
  c.os = AST_TARGET_OS_UNKNOWN;
  c.pointer_bits =
      arch == AST_TARGET_ARCH_X86_32 || arch == AST_TARGET_ARCH_X86_16 ? 32u
                                                                       : 64u;
  c.max_diagnostics = 128u;
  c.verify_mir_before_build = 1u;
  c.fail_on_unsupported = 1u;
  c.exact_native_fail_closed = 1u;
  if (arch == AST_TARGET_ARCH_X86_64)
    c.features = AST_TARGET_FEATURE_X86_SSE2;
  return c;
}

uint64_t ast_target_feature_closure(AstTargetArch arch, uint64_t f) {
  if (arch == AST_TARGET_ARCH_X86_64 || arch == AST_TARGET_ARCH_X86_32 ||
      arch == AST_TARGET_ARCH_X86_16) {
    if (arch == AST_TARGET_ARCH_X86_64)
      f |= AST_TARGET_FEATURE_X86_SSE2;
    if (f & AST_TARGET_FEATURE_X86_CMPXCHG8B)
      f |= AST_TARGET_FEATURE_X86_CMPXCHG;
  }
  if (arch == AST_TARGET_ARCH_X86_64) {
    if (f & AST_TARGET_FEATURE_X86_SSE41)
      f |= AST_TARGET_FEATURE_X86_SSE2;
    if (f & AST_TARGET_FEATURE_X86_AVX)
      f |= AST_TARGET_FEATURE_X86_SSE2;
    if (f & AST_TARGET_FEATURE_X86_AVX2)
      f |= AST_TARGET_FEATURE_X86_AVX | AST_TARGET_FEATURE_X86_SSE2;
    if (f & AST_TARGET_FEATURE_X86_AVX512F)
      f |= AST_TARGET_FEATURE_X86_AVX | AST_TARGET_FEATURE_X86_SSE2;
    if (f & (AST_TARGET_FEATURE_X86_AVX512BW | AST_TARGET_FEATURE_X86_AVX512DQ |
             AST_TARGET_FEATURE_X86_AVX512VL))
      f |= AST_TARGET_FEATURE_X86_AVX512F | AST_TARGET_FEATURE_X86_AVX |
           AST_TARGET_FEATURE_X86_SSE2;
    if (f & AST_TARGET_FEATURE_X86_F16C)
      f |= AST_TARGET_FEATURE_X86_AVX | AST_TARGET_FEATURE_X86_SSE2;
  } else if (arch == AST_TARGET_ARCH_AARCH64) {
    if (f &
        (AST_TARGET_FEATURE_A64_SVE2_AES | AST_TARGET_FEATURE_A64_SVE2_SHA3 |
         AST_TARGET_FEATURE_A64_SVE2_SM4 | AST_TARGET_FEATURE_A64_SVE2_BITPERM))
      f |= AST_TARGET_FEATURE_A64_SVE2;
    if (f & AST_TARGET_FEATURE_A64_SVE2)
      f |= AST_TARGET_FEATURE_A64_SVE;
    if (f &
        (AST_TARGET_FEATURE_A64_SME_I16I64 | AST_TARGET_FEATURE_A64_SME_F64F64 |
         AST_TARGET_FEATURE_A64_SME_FA64))
      f |= AST_TARGET_FEATURE_A64_SME;
    if (f & AST_TARGET_FEATURE_A64_SME_FA64)
      f |= AST_TARGET_FEATURE_A64_SVE2 | AST_TARGET_FEATURE_A64_SVE;
  } else if (arch == AST_TARGET_ARCH_RISCV64) {
    if (f & AST_TARGET_FEATURE_RV_D)
      f |= AST_TARGET_FEATURE_RV_F;
    if (f & AST_TARGET_FEATURE_RV_ZFH)
      f |= AST_TARGET_FEATURE_RV_F;
  }
  return f;
}

int ast_target_config_from_triple(const char *triple, AstTargetConfig *out) {
  if (!triple || !out)
    return 0;
  AstTargetArch arch = AST_TARGET_ARCH_INVALID;
  if (strstr(triple, "x86_64") || strstr(triple, "amd64"))
    arch = AST_TARGET_ARCH_X86_64;
  else if (strstr(triple, "aarch64") || strstr(triple, "arm64"))
    arch = AST_TARGET_ARCH_AARCH64;
  else if (strstr(triple, "riscv64"))
    arch = AST_TARGET_ARCH_RISCV64;
  else if (strstr(triple, "x86_16"))
    arch = AST_TARGET_ARCH_X86_16;
  else if (strstr(triple, "x86_32") || strstr(triple, "i386") ||
           strstr(triple, "i686"))
    arch = AST_TARGET_ARCH_X86_32;
  if (arch == AST_TARGET_ARCH_INVALID)
    return 0;
  *out = ast_target_config_default(arch);
  if (strstr(triple, "windows") || strstr(triple, "win32") ||
      strstr(triple, "mingw") || strstr(triple, "msvc"))
    out->os = AST_TARGET_OS_WINDOWS;
  else if (strstr(triple, "linux"))
    out->os = AST_TARGET_OS_LINUX;
  else if (strstr(triple, "darwin") || strstr(triple, "macos") ||
           strstr(triple, "apple"))
    out->os = AST_TARGET_OS_MACOS;
  else if (strstr(triple, "none") || strstr(triple, "elf"))
    out->os = AST_TARGET_OS_BARE_METAL;
  return out->os != AST_TARGET_OS_UNKNOWN;
}

const char *ast_target_arch_name(AstTargetArch arch) {
  switch (arch) {
  case AST_TARGET_ARCH_X86_32:
    return "x86_32";
  case AST_TARGET_ARCH_X86_16:
    return "x86_16";
  case AST_TARGET_ARCH_X86_64:
    return "x86_64";
  case AST_TARGET_ARCH_AARCH64:
    return "aarch64";
  case AST_TARGET_ARCH_RISCV64:
    return "riscv64";
  default:
    return "invalid";
  }
}

const char *ast_target_legalize_action_name(AstLegalizeAction action) {
  switch (action) {
  case AST_LEGALIZE_LEGAL:
    return "legal";
  case AST_LEGALIZE_PROMOTE:
    return "promote";
  case AST_LEGALIZE_SPLIT:
    return "split";
  case AST_LEGALIZE_CUSTOM:
    return "custom";
  case AST_LEGALIZE_INDIRECT:
    return "indirect";
  case AST_LEGALIZE_UNSUPPORTED:
    return "unsupported";
  default:
    return "invalid";
  }
}

const char *ast_target_reg_class_name(AstTargetRegClass c) {
  switch (c) {
  case AST_TARGET_REGCLASS_GPR8:
    return "gpr8";
  case AST_TARGET_REGCLASS_GPR16:
    return "gpr16";
  case AST_TARGET_REGCLASS_GPR32:
    return "gpr32";
  case AST_TARGET_REGCLASS_GPR64:
    return "gpr64";
  case AST_TARGET_REGCLASS_GPR_PAIR:
    return "gpr_pair";
  case AST_TARGET_REGCLASS_FP16:
    return "fp16";
  case AST_TARGET_REGCLASS_FP32:
    return "fp32";
  case AST_TARGET_REGCLASS_FP64:
    return "fp64";
  case AST_TARGET_REGCLASS_FP128:
    return "fp128";
  case AST_TARGET_REGCLASS_VEC64:
    return "vec64";
  case AST_TARGET_REGCLASS_VEC128:
    return "vec128";
  case AST_TARGET_REGCLASS_VEC256:
    return "vec256";
  case AST_TARGET_REGCLASS_VEC512:
    return "vec512";
  case AST_TARGET_REGCLASS_MASK:
    return "mask";
  case AST_TARGET_REGCLASS_SCALABLE_VECTOR:
    return "scalable_vector";
  case AST_TARGET_REGCLASS_PREDICATE:
    return "predicate";
  case AST_TARGET_REGCLASS_MEMORY:
    return "memory";
  case AST_TARGET_REGCLASS_SPECIAL:
    return "special";
  default:
    return "invalid";
  }
}

const char *ast_target_diag_code_name(AstTargetDiagCode code) {
  switch (code) {
  case AST_TARGET_DIAG_MIR_REQUIRED:
    return "target.mir_required";
  case AST_TARGET_DIAG_MIR_INVALID:
    return "target.mir_invalid";
  case AST_TARGET_DIAG_INVALID_CONFIG:
    return "target.invalid_config";
  case AST_TARGET_DIAG_UNSUPPORTED_TYPE:
    return "target.unsupported_type";
  case AST_TARGET_DIAG_UNSUPPORTED_OPERATION:
    return "target.unsupported_operation";
  case AST_TARGET_DIAG_REQUIRED_FEATURE:
    return "target.required_feature";
  case AST_TARGET_DIAG_NATIVE_ARCH_MISMATCH:
    return "target.native_arch_mismatch";
  case AST_TARGET_DIAG_INVALID_NATIVE_REGISTER:
    return "target.invalid_native_register";
  case AST_TARGET_DIAG_INVALID_NATIVE_FORM:
    return "target.invalid_native_form";
  case AST_TARGET_DIAG_INTERNAL:
    return "target.internal";
  case AST_TARGET_DIAG_TOO_MANY_DIAGNOSTICS:
    return "target.too_many_diagnostics";
  default:
    return "target.none";
  }
}

uint64_t ast_target_feature_from_name(AstTargetArch arch, const char *name) {
  if (!name)
    return 0u;
  if (arch == AST_TARGET_ARCH_X86_64 || arch == AST_TARGET_ARCH_X86_32 ||
      arch == AST_TARGET_ARCH_X86_16) {
    if (target_ascii_ieq(name, "cmpxchg") || target_ascii_ieq(name, "i486"))
      return AST_TARGET_FEATURE_X86_CMPXCHG;
    if (target_ascii_ieq(name, "cmpxchg8b") || target_ascii_ieq(name, "i586"))
      return AST_TARGET_FEATURE_X86_CMPXCHG8B;
  }
  if (arch == AST_TARGET_ARCH_X86_64) {
    if (target_ascii_ieq(name, "sse2"))
      return AST_TARGET_FEATURE_X86_SSE2;
    if (target_ascii_ieq(name, "sse4.1") || target_ascii_ieq(name, "sse41"))
      return AST_TARGET_FEATURE_X86_SSE41;
    if (target_ascii_ieq(name, "avx"))
      return AST_TARGET_FEATURE_X86_AVX;
    if (target_ascii_ieq(name, "avx2"))
      return AST_TARGET_FEATURE_X86_AVX2;
    if (target_ascii_ieq(name, "avx512f"))
      return AST_TARGET_FEATURE_X86_AVX512F;
    if (target_ascii_ieq(name, "avx512bw"))
      return AST_TARGET_FEATURE_X86_AVX512BW;
    if (target_ascii_ieq(name, "avx512dq"))
      return AST_TARGET_FEATURE_X86_AVX512DQ;
    if (target_ascii_ieq(name, "avx512vl"))
      return AST_TARGET_FEATURE_X86_AVX512VL;
    if (target_ascii_ieq(name, "f16c"))
      return AST_TARGET_FEATURE_X86_F16C;
    if (target_ascii_ieq(name, "popcnt"))
      return AST_TARGET_FEATURE_X86_POPCNT;
    if (target_ascii_ieq(name, "xsave"))
      return AST_TARGET_FEATURE_X86_XSAVE;
    if (target_ascii_ieq(name, "smap"))
      return AST_TARGET_FEATURE_X86_SMAP;
    if (target_ascii_ieq(name, "rdtscp"))
      return AST_TARGET_FEATURE_X86_RDTSCP;
  } else if (arch == AST_TARGET_ARCH_AARCH64) {
    if (target_ascii_ieq(name, "fp16"))
      return AST_TARGET_FEATURE_A64_FP16;
    if (target_ascii_ieq(name, "bf16"))
      return AST_TARGET_FEATURE_A64_BF16;
    if (target_ascii_ieq(name, "sve"))
      return AST_TARGET_FEATURE_A64_SVE;
    if (target_ascii_ieq(name, "sve2"))
      return AST_TARGET_FEATURE_A64_SVE2;
    if (target_ascii_ieq(name, "sme"))
      return AST_TARGET_FEATURE_A64_SME;
    if (target_ascii_ieq(name, "sme-i16i64"))
      return AST_TARGET_FEATURE_A64_SME_I16I64;
    if (target_ascii_ieq(name, "sme-f64f64"))
      return AST_TARGET_FEATURE_A64_SME_F64F64;
    if (target_ascii_ieq(name, "sme-fa64"))
      return AST_TARGET_FEATURE_A64_SME_FA64;
    if (target_ascii_ieq(name, "sve2-aes"))
      return AST_TARGET_FEATURE_A64_SVE2_AES;
    if (target_ascii_ieq(name, "sve2-sha3"))
      return AST_TARGET_FEATURE_A64_SVE2_SHA3;
    if (target_ascii_ieq(name, "sve2-sm4"))
      return AST_TARGET_FEATURE_A64_SVE2_SM4;
    if (target_ascii_ieq(name, "sve2-bitperm"))
      return AST_TARGET_FEATURE_A64_SVE2_BITPERM;
    if (target_ascii_ieq(name, "i8mm"))
      return AST_TARGET_FEATURE_A64_I8MM;
    if (target_ascii_ieq(name, "f32mm"))
      return AST_TARGET_FEATURE_A64_F32MM;
    if (target_ascii_ieq(name, "f64mm"))
      return AST_TARGET_FEATURE_A64_F64MM;
  } else if (arch == AST_TARGET_ARCH_RISCV64) {
    if (target_ascii_ieq(name, "m"))
      return AST_TARGET_FEATURE_RV_M;
    if (target_ascii_ieq(name, "a"))
      return AST_TARGET_FEATURE_RV_A;
    if (target_ascii_ieq(name, "f"))
      return AST_TARGET_FEATURE_RV_F;
    if (target_ascii_ieq(name, "d"))
      return AST_TARGET_FEATURE_RV_D;
    if (target_ascii_ieq(name, "zbb"))
      return AST_TARGET_FEATURE_RV_ZBB;
    if (target_ascii_ieq(name, "zba"))
      return AST_TARGET_FEATURE_RV_ZBA;
    if (target_ascii_ieq(name, "zbc"))
      return AST_TARGET_FEATURE_RV_ZBC;
    if (target_ascii_ieq(name, "zbs"))
      return AST_TARGET_FEATURE_RV_ZBS;
    if (target_ascii_ieq(name, "zfh"))
      return AST_TARGET_FEATURE_RV_ZFH;
    if (target_ascii_ieq(name, "v"))
      return AST_TARGET_FEATURE_RV_V;
    if (target_ascii_ieq(name, "zicsr"))
      return AST_TARGET_FEATURE_RV_ZICSR;
    if (target_ascii_ieq(name, "zifencei"))
      return AST_TARGET_FEATURE_RV_ZIFENCEI;
    if (target_ascii_ieq(name, "h"))
      return AST_TARGET_FEATURE_RV_H;
    if (target_ascii_ieq(name, "svinval"))
      return AST_TARGET_FEATURE_RV_SVINVAL;
  }
  return 0u;
}

static int target_is_legacy_x86(const AstTargetConfig *config) {
  return config && (config->arch == AST_TARGET_ARCH_X86_32 ||
                    config->arch == AST_TARGET_ARCH_X86_16);
}

static TargetTypeClass target_scalar_integer(const AstTargetConfig *config,
                                             const AstSemaType *type) {
  TargetTypeClass r = {AST_LEGALIZE_UNSUPPORTED, AST_TARGET_REGCLASS_INVALID,
                       0u, 0u, 0u};
  uint32_t bits = ast_sema_type_kind(type) == AST_SEMA_TYPE_UNTYPED_INT
                      ? config->pointer_bits
                      : ast_sema_type_bit_width(type);
  if (!bits) {
    uint64_t size = ast_sema_type_size(type);
    if (size != UINT64_MAX && size <= UINT32_MAX / 8u)
      bits = (uint32_t)size * 8u;
  }
  if (!bits)
    bits = config->pointer_bits;
  const int legacy = target_is_legacy_x86(config);
  const uint32_t word = legacy ? 32u : 64u;

  /* The dedicated 386+ encoder has a deliberately smaller scalar contract
   * than the 64-bit machine backends.  Do not advertise a generic SPLIT
   * action for widths that the encoder cannot actually materialize: that
   * used to let u128/bits<N> pass target verification only to fail much
   * later in code emission.  Legacy integer/bits values are currently
   * frozen at the native storage widths 8/16/32 plus the explicit EDX:EAX
   * 64-bit pair.  Pointer-like values remain 32-bit through pointer_bits. */
  if (legacy) {
    AstSemaTypeKind kind = ast_sema_type_kind(type);
    if ((kind == AST_SEMA_TYPE_INT || kind == AST_SEMA_TYPE_BITS) &&
        bits != 8u && bits != 16u && bits != 32u && bits != 64u)
      return r;
    if (bits > 64u)
      return r;
  }

  if (bits <= word) {
    r.action = AST_LEGALIZE_LEGAL;
    r.reg_class = gpr_class(bits);
    r.legal_width = width_class_bits(bits);
    r.part_count = 1u;
  } else if (bits <= (legacy ? 64u : 512u)) {
    r.action = AST_LEGALIZE_SPLIT;
    r.reg_class = AST_TARGET_REGCLASS_GPR_PAIR;
    r.legal_width = word;
    r.part_count = (bits + word - 1u) / word;
  }
  return r;
}

static TargetTypeClass target_float_type(const AstTargetConfig *config,
                                         const AstSemaType *type) {
  TargetTypeClass r = {AST_LEGALIZE_UNSUPPORTED, AST_TARGET_REGCLASS_INVALID,
                       0u, 0u, 0u};
  uint32_t bits = ast_sema_type_bit_width(type);
  if (bits == 16u) {
    r.reg_class = AST_TARGET_REGCLASS_FP16;
    r.legal_width = 16u;
    r.part_count = 1u;
    if (target_is_legacy_x86(config)) {
      /* The legacy x87 path has no native IEEE binary16 contract. */
      r.action = AST_LEGALIZE_UNSUPPORTED;
    } else if (config->arch == AST_TARGET_ARCH_X86_64) {
      /* F16C provides conversion, not native scalar/vector f16 arithmetic. */
      r.required_features = AST_TARGET_FEATURE_X86_F16C;
      r.action = (config->features & r.required_features)
                     ? AST_LEGALIZE_CUSTOM
                     : AST_LEGALIZE_PROMOTE;
    } else if (config->arch == AST_TARGET_ARCH_AARCH64) {
      r.required_features = AST_TARGET_FEATURE_A64_FP16;
      r.action = (config->features & r.required_features)
                     ? AST_LEGALIZE_LEGAL
                     : AST_LEGALIZE_PROMOTE;
    } else if (config->arch == AST_TARGET_ARCH_RISCV64) {
      r.required_features = AST_TARGET_FEATURE_RV_ZFH;
      if (config->features & AST_TARGET_FEATURE_RV_ZFH)
        r.action = AST_LEGALIZE_LEGAL;
      else if (config->features & AST_TARGET_FEATURE_RV_F)
        r.action = AST_LEGALIZE_PROMOTE;
      else {
        r.action = AST_LEGALIZE_CUSTOM;
        r.reg_class = AST_TARGET_REGCLASS_GPR16;
      }
    }
  } else if (bits == 32u) {
    r.reg_class = AST_TARGET_REGCLASS_FP32;
    r.legal_width = 32u;
    r.part_count = 1u;
    if (target_is_legacy_x86(config)) {
      r.action = AST_LEGALIZE_CUSTOM; /* x87 memory-stack lowering */
    } else if (config->arch == AST_TARGET_ARCH_RISCV64) {
      r.required_features = AST_TARGET_FEATURE_RV_F;
      if (config->features & r.required_features)
        r.action = AST_LEGALIZE_LEGAL;
      else {
        r.action = AST_LEGALIZE_CUSTOM;
        r.reg_class = AST_TARGET_REGCLASS_GPR32;
      }
    } else
      r.action = AST_LEGALIZE_LEGAL;
  } else if (bits == 64u) {
    r.reg_class = AST_TARGET_REGCLASS_FP64;
    r.legal_width = 64u;
    r.part_count = 1u;
    if (target_is_legacy_x86(config)) {
      r.action = AST_LEGALIZE_CUSTOM; /* x87 memory-stack lowering */
    } else if (config->arch == AST_TARGET_ARCH_RISCV64) {
      r.required_features = AST_TARGET_FEATURE_RV_D;
      if (config->features & r.required_features)
        r.action = AST_LEGALIZE_LEGAL;
      else {
        r.action = AST_LEGALIZE_CUSTOM;
        r.reg_class = AST_TARGET_REGCLASS_GPR64;
      }
    } else
      r.action = AST_LEGALIZE_LEGAL;
  } else if (bits == 128u) {
    r.reg_class = AST_TARGET_REGCLASS_FP128;
    r.legal_width = 128u;
    r.part_count = 1u;
    /* No x87 binary128 representation exists in the frozen legacy ABI. */
    r.action = target_is_legacy_x86(config) ? AST_LEGALIZE_UNSUPPORTED
                                            : AST_LEGALIZE_CUSTOM;
  }
  return r;
}

static TargetTypeClass target_vector_type(const AstTargetConfig *config,
                                          const AstSemaType *type, int mask) {
  if (mask)
    return target_scalar_integer(config, type);
  TargetTypeClass r = {AST_LEGALIZE_UNSUPPORTED, AST_TARGET_REGCLASS_INVALID,
                       0u, 0u, 0u};
  uint64_t size = ast_sema_type_size(type);
  if (size == UINT64_MAX || size > UINT32_MAX / 8u)
    return r;
  uint32_t bits = (uint32_t)size * 8u;
  r.legal_width = bits;
  r.part_count = 1u;
  if (config->arch == AST_TARGET_ARCH_X86_64) {
    if (bits <= 64u) {
      r.action = AST_LEGALIZE_LEGAL;
      r.reg_class = AST_TARGET_REGCLASS_VEC64;
      r.legal_width = 64u;
    } else if (bits <= 128u) {
      r.action = AST_LEGALIZE_LEGAL;
      r.reg_class = AST_TARGET_REGCLASS_VEC128;
      r.legal_width = 128u;
    } else if (bits <= 512u) {
      r.action = AST_LEGALIZE_SPLIT;
      r.reg_class = AST_TARGET_REGCLASS_VEC128;
      r.legal_width = 128u;
      r.part_count = (bits + 127u) / 128u;
      r.required_features = AST_TARGET_FEATURE_X86_SSE2;
    }
  } else if (config->arch == AST_TARGET_ARCH_AARCH64) {
    if (bits <= 64u) {
      r.action = AST_LEGALIZE_LEGAL;
      r.reg_class = AST_TARGET_REGCLASS_VEC64;
      r.legal_width = 64u;
    } else if (bits <= 128u) {
      r.action = AST_LEGALIZE_LEGAL;
      r.reg_class = AST_TARGET_REGCLASS_VEC128;
      r.legal_width = 128u;
    } else if (bits <= 512u) {
      r.action = AST_LEGALIZE_SPLIT;
      r.reg_class = AST_TARGET_REGCLASS_VEC128;
      r.legal_width = 128u;
      r.part_count = (bits + 127u) / 128u;
    }
  } else if (config->arch == AST_TARGET_ARCH_RISCV64) {
    /* Fixed vectors are a language type, not an RVV-only type.  Keep a
       correctness representation available on baseline RV64 by splitting
       the value into XLEN GPR chunks.  RVV remains an optimization for
       selected vector instructions; scalable vectors continue to require
       V explicitly. */
    if (bits <= 64u) {
      r.action = AST_LEGALIZE_CUSTOM;
      r.reg_class = AST_TARGET_REGCLASS_GPR64;
      r.legal_width = 64u;
      r.part_count = 1u;
    } else if (bits <= 512u) {
      r.action = AST_LEGALIZE_SPLIT;
      r.reg_class = AST_TARGET_REGCLASS_GPR_PAIR;
      r.legal_width = 64u;
      r.part_count = (bits + 63u) / 64u;
    }
  }
  return r;
}

TargetTypeClass target_classify_type(const AstTargetConfig *config,
                                     const AstSemaType *type) {
  TargetTypeClass r = {AST_LEGALIZE_UNSUPPORTED, AST_TARGET_REGCLASS_INVALID,
                       0u, 0u, 0u};
  if (!config || !type)
    return r;
  AstSemaTypeKind k = ast_sema_type_kind(type);
  switch (k) {
  case AST_SEMA_TYPE_BOOL:
  case AST_SEMA_TYPE_INT:
  case AST_SEMA_TYPE_UNTYPED_INT:
  case AST_SEMA_TYPE_PTR:
  case AST_SEMA_TYPE_CAP:
  case AST_SEMA_TYPE_BITS:
  case AST_SEMA_TYPE_STRING:
  case AST_SEMA_TYPE_BYTE_STRING:
  case AST_SEMA_TYPE_C_STRING:
    return target_scalar_integer(config, type);
  case AST_SEMA_TYPE_FLOAT:
    return target_float_type(config, type);
  case AST_SEMA_TYPE_VECTOR:
    return target_vector_type(config, type, 0);
  case AST_SEMA_TYPE_MASK:
    return target_vector_type(config, type, 1);
  case AST_SEMA_TYPE_SVECTOR:
    r.reg_class = AST_TARGET_REGCLASS_SCALABLE_VECTOR;
    if (config->arch == AST_TARGET_ARCH_AARCH64) {
      r.required_features = AST_TARGET_FEATURE_A64_SVE;
      r.action = (config->features & r.required_features)
                     ? AST_LEGALIZE_LEGAL
                     : AST_LEGALIZE_UNSUPPORTED;
    } else if (config->arch == AST_TARGET_ARCH_RISCV64) {
      r.required_features = AST_TARGET_FEATURE_RV_V;
      r.action = (config->features & r.required_features)
                     ? AST_LEGALIZE_LEGAL
                     : AST_LEGALIZE_UNSUPPORTED;
    }
    return r;
  case AST_SEMA_TYPE_PRED:
    r.reg_class = AST_TARGET_REGCLASS_PREDICATE;
    if (config->arch == AST_TARGET_ARCH_AARCH64) {
      r.required_features = AST_TARGET_FEATURE_A64_SVE;
      r.action = (config->features & r.required_features)
                     ? AST_LEGALIZE_LEGAL
                     : AST_LEGALIZE_UNSUPPORTED;
    } else if (config->arch == AST_TARGET_ARCH_RISCV64) {
      r.required_features = AST_TARGET_FEATURE_RV_V;
      r.action = (config->features & r.required_features)
                     ? AST_LEGALIZE_LEGAL
                     : AST_LEGALIZE_UNSUPPORTED;
    }
    return r;
  case AST_SEMA_TYPE_ARRAY:
  case AST_SEMA_TYPE_TUPLE:
  case AST_SEMA_TYPE_STRUCT: {
    uint64_t size = ast_sema_type_size(type);
    r.reg_class = AST_TARGET_REGCLASS_MEMORY;
    if (size == UINT64_MAX)
      r.action = AST_LEGALIZE_INDIRECT;
    else if (target_is_legacy_x86(config)) {
      if (size <= 4u) {
        r.action = AST_LEGALIZE_CUSTOM;
        r.reg_class = AST_TARGET_REGCLASS_GPR32;
        r.legal_width = 32u;
        r.part_count = 1u;
      } else if (size <= 8u) {
        r.action = AST_LEGALIZE_SPLIT;
        r.reg_class = AST_TARGET_REGCLASS_GPR_PAIR;
        r.legal_width = 32u;
        r.part_count = 2u;
      } else
        r.action = AST_LEGALIZE_INDIRECT;
    } else if (size <= 8u) {
      r.action = AST_LEGALIZE_CUSTOM;
      r.reg_class = AST_TARGET_REGCLASS_GPR64;
      r.legal_width = 64u;
      r.part_count = 1u;
    } else if (size <= 16u) {
      r.action = AST_LEGALIZE_SPLIT;
      r.reg_class = AST_TARGET_REGCLASS_GPR_PAIR;
      r.legal_width = 64u;
      r.part_count = 2u;
    } else
      r.action = AST_LEGALIZE_INDIRECT;
    return r;
  }
  case AST_SEMA_TYPE_VOID:
    r.action = AST_LEGALIZE_LEGAL;
    return r;
  default:
    return r;
  }
}

static const AstSemaType *
instruction_operand_type(const AstMirFunction *function,
                         const AstMirInstructionInfo *inst, uint32_t index) {
  if (!function || !inst || index >= inst->operand_count)
    return NULL;
  AstMirVRegInfo v;
  if (!ast_mir_function_vreg(function, inst->operands[index], &v))
    return NULL;
  return v.type;
}

static int type_is_integerish(const AstSemaType *t) {
  if (!t)
    return 0;
  AstSemaTypeKind k = ast_sema_type_kind(t);
  return k == AST_SEMA_TYPE_BOOL || k == AST_SEMA_TYPE_INT ||
         k == AST_SEMA_TYPE_BITS || k == AST_SEMA_TYPE_PTR ||
         k == AST_SEMA_TYPE_CAP;
}

static int type_is_vectorish(const AstSemaType *t) {
  if (!t)
    return 0;
  AstSemaTypeKind k = ast_sema_type_kind(t);
  return k == AST_SEMA_TYPE_VECTOR || k == AST_SEMA_TYPE_SVECTOR ||
         k == AST_SEMA_TYPE_PRED;
}

static uint64_t vector_required_features(const AstTargetConfig *config,
                                         const AstSemaType *type,
                                         AstTokenKind op) {
  uint64_t size = ast_sema_type_size(type);
  uint32_t bits =
      (size == UINT64_MAX || size > UINT32_MAX / 8u) ? 0u : (uint32_t)size * 8u;
  const AstSemaType *elem = ast_sema_type_element(type);
  AstSemaTypeKind ek = elem ? ast_sema_type_kind(elem) : AST_SEMA_TYPE_ERROR;
  if (config->arch == AST_TARGET_ARCH_X86_64) {
    if (bits > 256u) {
      uint64_t f = AST_TARGET_FEATURE_X86_AVX512F;
      uint32_t eb = elem ? ast_sema_type_bit_width(elem) : 0u;
      if (ek == AST_SEMA_TYPE_INT && eb <= 16u)
        f |= AST_TARGET_FEATURE_X86_AVX512BW;
      if (ek == AST_SEMA_TYPE_INT && eb == 64u &&
          (op == AST_TOK_STAR || op == AST_TOK_EQ_EQ || op == AST_TOK_LT ||
           op == AST_TOK_GT))
        f |= AST_TARGET_FEATURE_X86_AVX512DQ;
      return f;
    }
    if (bits > 128u)
      return ek == AST_SEMA_TYPE_INT ? AST_TARGET_FEATURE_X86_AVX2
                                     : AST_TARGET_FEATURE_X86_AVX;
    return AST_TARGET_FEATURE_X86_SSE2;
  }
  if (config->arch == AST_TARGET_ARCH_RISCV64) {
    AstSemaTypeKind kind = ast_sema_type_kind(type);
    return (kind == AST_SEMA_TYPE_SVECTOR || kind == AST_SEMA_TYPE_PRED)
               ? AST_TARGET_FEATURE_RV_V
               : 0u;
  }
  return 0u;
}

AstLegalizeAction target_classify_instruction(const AstTargetConfig *config,
                                              const AstMirFunction *function,
                                              const AstMirInstructionInfo *inst,
                                              AstTargetRegClass *result_class,
                                              uint64_t *required_features) {
  if (result_class)
    *result_class = AST_TARGET_REGCLASS_INVALID;
  if (required_features)
    *required_features = 0u;
  if (!config || !inst)
    return AST_LEGALIZE_UNSUPPORTED;
  TargetTypeClass tc = target_classify_type(config, inst->type);
  if (result_class)
    *result_class = tc.reg_class;
  uint64_t req = tc.required_features;
  AstLegalizeAction action = tc.action;
  if (inst->result == AST_MIR_INVALID_ID && !inst->type)
    action = AST_LEGALIZE_LEGAL;

  const AstSemaType *operand0 = instruction_operand_type(function, inst, 0u);
  if (inst->op == AST_MIR_OP_BINARY || inst->op == AST_MIR_OP_UNARY ||
      inst->op == AST_MIR_OP_SELECT) {
    const AstSemaType *basis = operand0 ? operand0 : inst->type;
    if (type_is_vectorish(basis)) {
      TargetTypeClass basis_tc = target_classify_type(config, basis);
      if (config->arch == AST_TARGET_ARCH_X86_64 &&
          basis_tc.action == AST_LEGALIZE_SPLIT)
        req = AST_TARGET_FEATURE_X86_SSE2;
      else
        req |= vector_required_features(config, basis, inst->token_op);
      if ((config->features & req) != req && req)
        action = AST_LEGALIZE_UNSUPPORTED;
      else if (config->arch == AST_TARGET_ARCH_RISCV64)
        action = AST_LEGALIZE_CUSTOM;
      else {
        const AstSemaType *elem = ast_sema_type_element(basis);
        AstSemaTypeKind ek =
            elem ? ast_sema_type_kind(elem) : AST_SEMA_TYPE_ERROR;
        uint32_t eb = elem ? ast_sema_type_bit_width(elem) : 0u;
        if (ek == AST_SEMA_TYPE_INT || ek == AST_SEMA_TYPE_BOOL ||
            ek == AST_SEMA_TYPE_BITS) {
          if (inst->token_op == AST_TOK_SLASH ||
              inst->token_op == AST_TOK_PERCENT)
            action = AST_LEGALIZE_CUSTOM;
          else if (config->arch == AST_TARGET_ARCH_AARCH64) {
            if (inst->token_op == AST_TOK_PERCENT)
              action = AST_LEGALIZE_CUSTOM;
          } else if (config->arch == AST_TARGET_ARCH_X86_64 &&
                     inst->token_op == AST_TOK_STAR) {
            /* Packed PMULLD/AVX512 forms are an optimization. The
               physical backend has a lane-scalar correctness path
               using baseline scalar IMUL for every 8/16/32/64-bit
               fixed-vector lane. */
            (void)eb;
            action = AST_LEGALIZE_CUSTOM;
          }
        }
      }
    } else if (type_is_integerish(basis) &&
               config->arch == AST_TARGET_ARCH_RISCV64 &&
               (inst->token_op == AST_TOK_STAR ||
                inst->token_op == AST_TOK_SLASH ||
                inst->token_op == AST_TOK_PERCENT)) {
      /* RV64M is the fast path, not a correctness requirement.  The
       * physical RV64I backend contains self-contained software
       * multiply/divide/remainder lowering for the baseline ISA. */
      req |= AST_TARGET_FEATURE_RV_M;
      action = (config->features & AST_TARGET_FEATURE_RV_M)
                   ? AST_LEGALIZE_LEGAL
                   : AST_LEGALIZE_CUSTOM;
    } else if (basis && ast_sema_type_kind(basis) == AST_SEMA_TYPE_FLOAT) {
      uint32_t bits = ast_sema_type_bit_width(basis);
      if (config->arch == AST_TARGET_ARCH_AARCH64 && bits == 16u) {
        req |= AST_TARGET_FEATURE_A64_FP16;
        action = (config->features & AST_TARGET_FEATURE_A64_FP16)
                     ? AST_LEGALIZE_LEGAL
                     : AST_LEGALIZE_PROMOTE;
      } else if (config->arch == AST_TARGET_ARCH_RISCV64) {
        uint64_t f = bits == 64u   ? AST_TARGET_FEATURE_RV_D
                     : bits == 32u ? AST_TARGET_FEATURE_RV_F
                                   : AST_TARGET_FEATURE_RV_ZFH;
        req |= f;
        action = ((config->features & f) == f) ? AST_LEGALIZE_LEGAL
                                               : AST_LEGALIZE_UNSUPPORTED;
      }
    }
  } else if (inst->op == AST_MIR_OP_PACK) {
    action = tc.action == AST_LEGALIZE_UNSUPPORTED ? AST_LEGALIZE_UNSUPPORTED
                                                   : AST_LEGALIZE_CUSTOM;
  } else if (inst->op == AST_MIR_OP_ATOMIC) {
    const AstSemaType *storage =
        inst->operand_count > 1u ? instruction_operand_type(function, inst, 1u)
                                 : inst->type;
    uint32_t atomic_bits = ast_sema_type_bit_width(storage);
    if (ast_sema_type_kind(storage) == AST_SEMA_TYPE_PTR ||
        ast_sema_type_kind(storage) == AST_SEMA_TYPE_CAP)
      atomic_bits = config->pointer_bits;
    if (atomic_bits == 1u)
      atomic_bits = 8u;
    if (target_is_legacy_x86(config)) {
      /* 80386 already provides naturally atomic aligned scalar loads/
       * stores through 32 bits plus memory XCHG.  General 8/16/32-bit
       * CAS/RMW needs i486 CMPXCHG; 64-bit atomicity needs Pentium-era
       * CMPXCHG8B.  Keep the baseline 386 target honest by making the
       * later CPU contract explicit instead of silently emitting it. */
      const int load = target_name_eq(inst->symbol, "load");
      const int store = target_name_eq(inst->symbol, "store");
      const int xchg = target_name_eq(inst->symbol, "rmw.xchg");
      if (atomic_bits == 64u)
        req |= AST_TARGET_FEATURE_X86_CMPXCHG8B;
      else if (atomic_bits <= 32u && !load && !store && !xchg)
        req |= AST_TARGET_FEATURE_X86_CMPXCHG;
      action = (req & ~config->features) ? AST_LEGALIZE_UNSUPPORTED
                                         : AST_LEGALIZE_LEGAL;
    } else if (config->arch == AST_TARGET_ARCH_RISCV64) {
      req |= AST_TARGET_FEATURE_RV_A;
      action = (config->features & AST_TARGET_FEATURE_RV_A)
                   ? AST_LEGALIZE_LEGAL
                   : AST_LEGALIZE_UNSUPPORTED;
    } else
      action = AST_LEGALIZE_LEGAL;
    if (atomic_bits != 8u && atomic_bits != 16u && atomic_bits != 32u &&
        atomic_bits != 64u)
      action = AST_LEGALIZE_UNSUPPORTED;
  } else if (inst->op == AST_MIR_OP_INTRINSIC) {
    if (config->arch == AST_TARGET_ARCH_RISCV64 &&
        inst->intrinsic == AST_HIR_INTRINSIC_MUL_WRAP) {
      req |= AST_TARGET_FEATURE_RV_M;
      action = (config->features & AST_TARGET_FEATURE_RV_M)
                   ? AST_LEGALIZE_LEGAL
                   : AST_LEGALIZE_CUSTOM;
    } else if (config->arch == AST_TARGET_ARCH_RISCV64 &&
               (inst->intrinsic == AST_HIR_INTRINSIC_CLZ ||
                inst->intrinsic == AST_HIR_INTRINSIC_CTZ ||
                inst->intrinsic == AST_HIR_INTRINSIC_POPCNT ||
                inst->intrinsic == AST_HIR_INTRINSIC_ROL ||
                inst->intrinsic == AST_HIR_INTRINSIC_ROR)) {
      req |= AST_TARGET_FEATURE_RV_ZBB;
      action = (config->features & AST_TARGET_FEATURE_RV_ZBB)
                   ? AST_LEGALIZE_LEGAL
                   : AST_LEGALIZE_CUSTOM;
    } else if (inst->intrinsic == AST_HIR_INTRINSIC_ADD_SAT ||
               inst->intrinsic == AST_HIR_INTRINSIC_SUB_SAT ||
               inst->intrinsic == AST_HIR_INTRINSIC_BITREV)
      action = AST_LEGALIZE_CUSTOM;
    else
      action = tc.action == AST_LEGALIZE_UNSUPPORTED ? AST_LEGALIZE_CUSTOM
                                                     : tc.action;
  } else if (inst->op == AST_MIR_OP_CALL_DIRECT ||
             inst->op == AST_MIR_OP_CALL_INDIRECT ||
             inst->op == AST_MIR_OP_STACK_LOAD ||
             inst->op == AST_MIR_OP_STACK_STORE ||
             inst->op == AST_MIR_OP_GLOBAL_LOAD ||
             inst->op == AST_MIR_OP_GLOBAL_STORE ||
             inst->op == AST_MIR_OP_LOAD || inst->op == AST_MIR_OP_STORE ||
             inst->op == AST_MIR_OP_FENCE ||
             inst->op == AST_MIR_OP_STACK_ADDR ||
             inst->op == AST_MIR_OP_GLOBAL_ADDR ||
             inst->op == AST_MIR_OP_FUNCTION_ADDR ||
             inst->op == AST_MIR_OP_ADDRESS_OF || inst->op == AST_MIR_OP_COPY ||
             inst->op == AST_MIR_OP_CONSTANT || inst->op == AST_MIR_OP_UNDEF ||
             inst->op == AST_MIR_OP_INDEX || inst->op == AST_MIR_OP_MEMBER ||
             inst->op == AST_MIR_OP_CAST || inst->op == AST_MIR_OP_BIND ||
             inst->op == AST_MIR_OP_NATIVE_RESULT) {
    if (action == AST_LEGALIZE_INVALID || action == AST_LEGALIZE_UNSUPPORTED) {
      if (inst->result == AST_MIR_INVALID_ID)
        action = AST_LEGALIZE_LEGAL;
    }
  } else if (inst->op == AST_MIR_OP_NATIVE ||
             inst->op == AST_MIR_OP_NATIVE_RESULT) {
    /* Native regions currently have register/encoding contracts for the
     * three machine backends only.  The dedicated code16/i386 encoder is
     * deliberately fail-closed until a versioned legacy-native grammar is
     * defined. */
    action = target_is_legacy_x86(config) ? AST_LEGALIZE_UNSUPPORTED
                                          : AST_LEGALIZE_CUSTOM;
  } else if (inst->op == AST_MIR_OP_GENERIC_CONST)
    action = AST_LEGALIZE_CUSTOM;

  if (required_features)
    *required_features = req;
  return action;
}

static void count_action(AstTargetStats *s, AstLegalizeAction a) {
  if (a == AST_LEGALIZE_LEGAL)
    s->legal_count++;
  else if (a == AST_LEGALIZE_PROMOTE)
    s->promote_count++;
  else if (a == AST_LEGALIZE_SPLIT)
    s->split_count++;
  else if (a == AST_LEGALIZE_CUSTOM)
    s->custom_count++;
  else if (a == AST_LEGALIZE_INDIRECT)
    s->indirect_count++;
  else if (a == AST_LEGALIZE_UNSUPPORTED)
    s->unsupported_count++;
}

typedef struct TargetLowerContext {
  AstTargetProgram *program;
  AstTargetProgram *results;
} TargetLowerContext;
static int target_lower_function(AstTargetProgram *p, size_t fi) {
  const AstMirProgram *mir = p->mir;
  const AstMirFunction *fn = ast_mir_function_at(mir, fi);
  AstMirFunctionInfo mi;
  if (!fn || !ast_mir_function_info(fn, &mi)) {
    target_diag(p, AST_TARGET_DIAG_INTERNAL, AST_DIAG_ERROR, (AstSpan){0},
                (uint32_t)fi, AST_MIR_INVALID_ID, "cannot query MIR function");
    return 1;
  }
  TargetFunction *tf = &p->functions[fi];
  tf->info.module_name = mi.module_name;
  tf->info.name = mi.name;
  tf->info.function_index = (uint32_t)fi;
  tf->info.vreg_count = mi.vreg_count;
  tf->info.instruction_count = mi.instruction_count;
  tf->values = (AstTargetValueInfo *)calloc(mi.vreg_count ? mi.vreg_count : 1u,
                                            sizeof(*tf->values));
  tf->instructions = (AstTargetInstructionInfo *)calloc(
      mi.instruction_count ? mi.instruction_count : 1u,
      sizeof(*tf->instructions));
  if (!tf->values || !tf->instructions) {
    return 0;
  }
  p->stats.vreg_count += mi.vreg_count;
  p->stats.instruction_count += mi.instruction_count;
  for (uint32_t vi = 0u; vi < mi.vreg_count; ++vi) {
    AstMirVRegInfo mv;
    if (!ast_mir_function_vreg(fn, vi, &mv))
      continue;
    TargetTypeClass tc = target_classify_type(&p->config, mv.type);
    AstTargetValueInfo *v = &tf->values[vi];
    v->vreg = vi;
    v->type = mv.type;
    v->action = tc.action;
    v->reg_class = tc.reg_class;
    v->legal_bit_width = tc.legal_width;
    v->part_count = tc.part_count;
    v->required_features = tc.required_features;
    count_action(&p->stats, tc.action);
    if (tc.action == AST_LEGALIZE_SPLIT)
      tf->info.split_count++;
    else if (tc.action == AST_LEGALIZE_CUSTOM)
      tf->info.custom_count++;
    else if (tc.action == AST_LEGALIZE_UNSUPPORTED) {
      tf->info.unsupported_count++;
      if (p->config.fail_on_unsupported)
        target_diag(p, AST_TARGET_DIAG_UNSUPPORTED_TYPE, AST_DIAG_ERROR,
                    mv.span, (uint32_t)fi, AST_MIR_INVALID_ID,
                    "vreg %u type has no legal representation for %s", vi,
                    ast_target_arch_name(p->config.arch));
    } else if (tc.action == AST_LEGALIZE_LEGAL)
      tf->info.legal_count++;
  }
  for (uint32_t ii = 0u; ii < mi.instruction_count; ++ii) {
    AstMirInstructionInfo inst;
    if (!ast_mir_function_instruction(fn, ii, &inst))
      continue;
    AstTargetInstructionInfo *ti = &tf->instructions[ii];
    ti->id = ii;
    ti->op = inst.op;
    ti->narrowed_effects = inst.flags;
    ti->action = target_classify_instruction(
        &p->config, fn, &inst, &ti->result_class, &ti->required_features);
    if (inst.op == AST_MIR_OP_NATIVE)
      (void)target_validate_native(p, fi, fn, &inst, ti);
    count_action(&p->stats, ti->action);
    if (ti->action == AST_LEGALIZE_LEGAL)
      tf->info.legal_count++;
    else if (ti->action == AST_LEGALIZE_CUSTOM)
      tf->info.custom_count++;
    else if (ti->action == AST_LEGALIZE_SPLIT)
      tf->info.split_count++;
    else if (ti->action == AST_LEGALIZE_UNSUPPORTED)
      tf->info.unsupported_count++;
    if (ti->action == AST_LEGALIZE_UNSUPPORTED &&
        p->config.fail_on_unsupported) {
      target_diag(p,
                  (ti->required_features &&
                   ((p->config.features & ti->required_features) !=
                    ti->required_features))
                      ? AST_TARGET_DIAG_REQUIRED_FEATURE
                      : AST_TARGET_DIAG_UNSUPPORTED_OPERATION,
                  AST_DIAG_ERROR, inst.span, (uint32_t)fi, ii,
                  "MIR operation %s is not legal for %s with selected features",
                  ast_mir_op_name(inst.op),
                  ast_target_arch_name(p->config.arch));
    }
  }
  return 1;
}
static int target_lower_task(void *opaque, size_t fi) {
  TargetLowerContext *ctx = opaque;
  AstTargetProgram *local = &ctx->results[fi];
  *local = *ctx->program;
  local->stats = (AstTargetStats){0};
  local->diagnostics = NULL;
  local->diagnostic_count = local->diagnostic_capacity = 0u;
  local->owned_messages = NULL;
  local->owned_message_count = local->owned_message_capacity = 0u;
  return target_lower_function(local, fi);
}
static void target_merge_result(AstTargetProgram *p, AstTargetProgram *r) {
  p->stats.vreg_count += r->stats.vreg_count;
  p->stats.instruction_count += r->stats.instruction_count;
  p->stats.legal_count += r->stats.legal_count;
  p->stats.promote_count += r->stats.promote_count;
  p->stats.split_count += r->stats.split_count;
  p->stats.custom_count += r->stats.custom_count;
  p->stats.indirect_count += r->stats.indirect_count;
  p->stats.unsupported_count += r->stats.unsupported_count;

  for (size_t i = 0; i < r->diagnostic_count; ++i) {
    const AstTargetDiagnostic *d = &r->diagnostics[i];
    target_diag(p, d->code, d->severity, d->span, d->function_index,
                d->instruction, "%s", d->message);
  }
  for (size_t i = 0; i < r->owned_message_count; ++i)
    free(r->owned_messages[i]);
  free(r->owned_messages);
  free(r->diagnostics);
}

AstTargetProgram *ast_target_legalize(const AstMirProgram *mir,
                                      const AstTargetConfig *config_in) {
  AstTargetConfig config =
      config_in ? *config_in
                : ast_target_config_default(AST_TARGET_ARCH_X86_64);
  config.features = ast_target_feature_closure(config.arch, config.features);
  if (!config.max_diagnostics)
    config.max_diagnostics = 128u;
  AstTargetProgram *p = (AstTargetProgram *)calloc(1u, sizeof(*p));
  if (!p)
    return NULL;
  p->mir = mir;
  p->config = config;
  if (!mir) {
    target_diag(p, AST_TARGET_DIAG_MIR_REQUIRED, AST_DIAG_ERROR, (AstSpan){0},
                UINT32_MAX, AST_MIR_INVALID_ID,
                "target legalization requires a MIR program");
    return p;
  }
  const int legacy_x86 = config.arch == AST_TARGET_ARCH_X86_32 ||
                         config.arch == AST_TARGET_ARCH_X86_16;
  const int valid_pointer_model =
      legacy_x86 ? config.pointer_bits == 32u : config.pointer_bits == 64u;
  if (config.arch == AST_TARGET_ARCH_INVALID || !valid_pointer_model) {
    target_diag(p, AST_TARGET_DIAG_INVALID_CONFIG, AST_DIAG_ERROR, (AstSpan){0},
                UINT32_MAX, AST_MIR_INVALID_ID,
                "unsupported target configuration");
    return p;
  }
  if (config.verify_mir_before_build && !ast_mir_verify((AstMirProgram *)mir)) {
    target_diag(p, AST_TARGET_DIAG_MIR_INVALID, AST_DIAG_ERROR, (AstSpan){0},
                UINT32_MAX, AST_MIR_INVALID_ID,
                "MIR verification failed before target legalization");
    return p;
  }
  p->function_count = ast_mir_function_count(mir);
  p->functions = (TargetFunction *)calloc(
      p->function_count ? p->function_count : 1u, sizeof(*p->functions));
  if (!p->functions) {
    ast_target_destroy(p);
    return NULL;
  }
  p->stats.function_count = p->function_count;
  AstTargetProgram *results =
      calloc(p->function_count ? p->function_count : 1u, sizeof(*results));
  if (!results) {
    ast_target_destroy(p);
    return NULL;
  }
  TargetLowerContext ctx = {p, results};
  int ok =
      ast_parallel_check(p->function_count, 0u, target_lower_task, &ctx, NULL);
  for (size_t fi = 0; fi < p->function_count; ++fi)
    target_merge_result(p, &results[fi]);
  free(results);
  if (!ok) {
    ast_target_destroy(p);
    return NULL;
  }
  return p;
}

void ast_target_destroy(AstTargetProgram *p) {
  if (!p)
    return;
  if (p->functions)
    for (size_t i = 0u; i < p->function_count; ++i) {
      free(p->functions[i].values);
      free(p->functions[i].instructions);
    }
  for (size_t i = 0u; i < p->owned_message_count; ++i)
    free(p->owned_messages[i]);
  free(p->owned_messages);
  free(p->diagnostics);
  free(p->functions);
  free(p);
}

int ast_target_verify(const AstTargetProgram *p) {
  if (!p || !p->mir || p->config.arch == AST_TARGET_ARCH_INVALID)
    return 0;
  const int legacy_x86 = p->config.arch == AST_TARGET_ARCH_X86_32 ||
                         p->config.arch == AST_TARGET_ARCH_X86_16;
  if (p->config.pointer_bits != (legacy_x86 ? 32u : 64u))
    return 0;
  if (p->stats.error_count != 0u ||
      p->function_count != ast_mir_function_count(p->mir) ||
      p->stats.function_count != p->function_count)
    return 0;
  size_t vregs = 0u, insts = 0u;
  AstTargetStats counted;
  memset(&counted, 0, sizeof(counted));
  for (size_t fi = 0u; fi < p->function_count; ++fi) {
    const AstMirFunction *mf = ast_mir_function_at(p->mir, fi);
    AstMirFunctionInfo mfi;
    const TargetFunction *f = &p->functions[fi];
    if (!mf || !ast_mir_function_info(mf, &mfi) || !f->values ||
        !f->instructions)
      return 0;
    if (f->info.function_index != fi || f->info.vreg_count != mfi.vreg_count ||
        f->info.instruction_count != mfi.instruction_count)
      return 0;
    vregs += mfi.vreg_count;
    insts += mfi.instruction_count;
    for (uint32_t vi = 0u; vi < f->info.vreg_count; ++vi) {
      AstMirVRegInfo mvi;
      const AstTargetValueInfo *v = &f->values[vi];
      if (!ast_mir_function_vreg(mf, vi, &mvi) || v->vreg != vi ||
          v->type != mvi.type || v->action == AST_LEGALIZE_INVALID ||
          v->action == AST_LEGALIZE_UNSUPPORTED)
        return 0;
      if (v->action == AST_LEGALIZE_SPLIT && v->part_count < 2u)
        return 0;
      if (v->action == AST_LEGALIZE_LEGAL &&
          (v->required_features & ~p->config.features) != 0u)
        return 0;
      count_action(&counted, v->action);
    }
    for (uint32_t ii = 0u; ii < f->info.instruction_count; ++ii) {
      AstMirInstructionInfo mi;
      const AstTargetInstructionInfo *in = &f->instructions[ii];
      if (!ast_mir_function_instruction(mf, ii, &mi) || in->id != ii ||
          in->op != mi.op || in->action == AST_LEGALIZE_INVALID ||
          in->action == AST_LEGALIZE_UNSUPPORTED)
        return 0;
      if (in->action == AST_LEGALIZE_LEGAL &&
          (in->required_features & ~p->config.features) != 0u)
        return 0;
      if (mi.op == AST_MIR_OP_NATIVE) {
        const AstHirFunction *hir = ast_mir_function_source_hir(mf);
        const AstHirNativeRegion *region =
            ast_hir_function_native_region(hir, mi.native_region);
        AstHirNativeRegionInfo info;
        if (!region || !ast_hir_native_region_info(region, &info) ||
            (in->required_features & ~p->config.features))
          return 0;
        for (uint32_t s = 0; s < info.statement_count; ++s) {
          AstHirNativeStmtInfo statement;
          if (!ast_hir_native_region_statement(region, s, &statement))
            return 0;
          uint64_t required = target_native_instruction_features(
              p->config.arch, statement.mnemonic);
          if ((required & ~in->required_features) ||
              (required & ~p->config.features))
            return 0;
        }
      }
      if (in->native_effects_refined) {
        if (mi.op != AST_MIR_OP_NATIVE ||
            !(mi.flags & AST_MIR_INST_CONSERVATIVE_EFFECTS) ||
            (in->narrowed_effects & AST_MIR_INST_CONSERVATIVE_EFFECTS) != 0u)
          return 0;
      }
      count_action(&counted, in->action);
    }
  }
  if (p->stats.vreg_count != vregs || p->stats.instruction_count != insts)
    return 0;
  return p->stats.legal_count == counted.legal_count &&
         p->stats.promote_count == counted.promote_count &&
         p->stats.split_count == counted.split_count &&
         p->stats.custom_count == counted.custom_count &&
         p->stats.indirect_count == counted.indirect_count &&
         p->stats.unsupported_count == 0u && counted.unsupported_count == 0u;
}

const AstMirProgram *ast_target_source_mir(const AstTargetProgram *p) {
  return p ? p->mir : NULL;
}
AstTargetConfig ast_target_config(const AstTargetProgram *p) {
  return p ? p->config : ast_target_config_default(AST_TARGET_ARCH_INVALID);
}
AstTargetStats ast_target_stats(const AstTargetProgram *p) {
  AstTargetStats z;
  memset(&z, 0, sizeof(z));
  return p ? p->stats : z;
}
const AstTargetDiagnostic *ast_target_diagnostics(const AstTargetProgram *p,
                                                  size_t *count_out) {
  if (count_out)
    *count_out = p ? p->diagnostic_count : 0u;
  return p ? p->diagnostics : NULL;
}
size_t ast_target_function_count(const AstTargetProgram *p) {
  return p ? p->function_count : 0u;
}
int ast_target_function_info(const AstTargetProgram *p, size_t i,
                             AstTargetFunctionInfo *out) {
  if (!p || !out || i >= p->function_count)
    return 0;
  *out = p->functions[i].info;
  return 1;
}
int ast_target_value_info(const AstTargetProgram *p, size_t fi, AstMirVReg v,
                          AstTargetValueInfo *out) {
  if (!p || !out || fi >= p->function_count ||
      v >= p->functions[fi].info.vreg_count)
    return 0;
  *out = p->functions[fi].values[v];
  return 1;
}
int ast_target_instruction_info(const AstTargetProgram *p, size_t fi,
                                AstMirInstId i, AstTargetInstructionInfo *out) {
  if (!p || !out || fi >= p->function_count ||
      i >= p->functions[fi].info.instruction_count)
    return 0;
  *out = p->functions[fi].instructions[i];
  return 1;
}

int ast_target_immediate_fits(int64_t value, uint32_t bits, int is_signed) {
  if (bits == 0u || bits > 64u)
    return 0;
  if (is_signed) {
    if (bits == 64u)
      return 1;
    int64_t min = -(INT64_C(1) << (bits - 1u));
    int64_t max = (INT64_C(1) << (bits - 1u)) - 1;
    return value >= min && value <= max;
  }
  if (value < 0)
    return 0;
  if (bits == 64u)
    return 1;
  return (uint64_t)value <= ((UINT64_C(1) << bits) - 1u);
}
