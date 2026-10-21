#include "../core/parallel.h"
#include "abi_internal.h"

static char *abi_owned_message(AstAbiProgram *p, const char *text) {
  size_t n = strlen(text) + 1u;
  char *copy = (char *)malloc(n);
  if (!copy)
    return NULL;
  memcpy(copy, text, n);
  if (p->owned_message_count == p->owned_message_capacity) {
    size_t cap =
        p->owned_message_capacity ? p->owned_message_capacity * 2u : 16u;
    char **q = (char **)realloc(p->owned_messages, cap * sizeof(*q));
    if (!q) {
      free(copy);
      return NULL;
    }
    p->owned_messages = q;
    p->owned_message_capacity = cap;
  }
  p->owned_messages[p->owned_message_count++] = copy;
  return copy;
}

void abi_diag(AstAbiProgram *p, AstAbiDiagCode code, AstDiagSeverity severity,
              AstSpan span, uint32_t fi, AstMirInstId inst, const char *fmt,
              ...) {
  if (!p || p->diagnostic_count >= p->config.max_diagnostics)
    return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (p->diagnostic_count == p->diagnostic_capacity) {
    size_t cap = p->diagnostic_capacity ? p->diagnostic_capacity * 2u : 16u;
    AstAbiDiagnostic *q =
        (AstAbiDiagnostic *)realloc(p->diagnostics, cap * sizeof(*q));
    if (!q)
      return;
    p->diagnostics = q;
    p->diagnostic_capacity = cap;
  }
  char *msg = abi_owned_message(p, buf);
  if (!msg)
    return;
  AstAbiDiagnostic *d = &p->diagnostics[p->diagnostic_count++];
  d->code = code;
  d->severity = severity;
  d->span = span;
  d->function_index = fi;
  d->instruction = inst;
  d->message = msg;
  if (severity == AST_DIAG_ERROR)
    p->stats.error_count++;
  else if (severity == AST_DIAG_WARNING)
    p->stats.warning_count++;
}

uint64_t abi_align_up(uint64_t v, uint64_t a) {
  if (a <= 1u)
    return v;
  uint64_t r = v % a;
  return r ? v + (a - r) : v;
}
uint64_t abi_type_size(const AstSemaType *t) {
  if (!t)
    return 0u;
  uint64_t s = ast_sema_type_size(t);
  return s == UINT64_MAX ? 0u : s;
}
uint32_t abi_type_align(const AstSemaType *t) {
  uint32_t a = t ? ast_sema_type_alignment(t) : 1u;
  return a ? a : 1u;
}
int abi_is_aggregate(const AstSemaType *t) {
  if (!t)
    return 0;
  AstSemaTypeKind k = ast_sema_type_kind(t);
  return k == AST_SEMA_TYPE_ARRAY || k == AST_SEMA_TYPE_TUPLE ||
         k == AST_SEMA_TYPE_STRUCT;
}
int abi_is_integer(const AstSemaType *t) {
  if (!t)
    return 0;
  AstSemaTypeKind k = ast_sema_type_kind(t);
  return k == AST_SEMA_TYPE_BOOL || k == AST_SEMA_TYPE_INT ||
         k == AST_SEMA_TYPE_BITS || k == AST_SEMA_TYPE_PTR ||
         k == AST_SEMA_TYPE_CAP || k == AST_SEMA_TYPE_MASK ||
         k == AST_SEMA_TYPE_STRING || k == AST_SEMA_TYPE_BYTE_STRING ||
         k == AST_SEMA_TYPE_C_STRING;
}
int abi_is_float(const AstSemaType *t) {
  return t && ast_sema_type_kind(t) == AST_SEMA_TYPE_FLOAT;
}
int abi_is_vector(const AstSemaType *t) {
  if (!t)
    return 0;
  AstSemaTypeKind k = ast_sema_type_kind(t);
  return k == AST_SEMA_TYPE_VECTOR;
}
int abi_is_scalable(const AstSemaType *t) {
  if (!t)
    return 0;
  AstSemaTypeKind k = ast_sema_type_kind(t);
  return k == AST_SEMA_TYPE_SVECTOR || k == AST_SEMA_TYPE_PRED;
}
void abi_set_reg(AstAbiLocation *loc, uint32_t slot, const char *name) {
  if (!loc || slot >= AST_ABI_MAX_REGISTERS || !name)
    return;
  snprintf(loc->registers[slot], AST_ABI_REGISTER_NAME_CAP, "%s", name);
  if (slot + 1u > loc->register_count)
    loc->register_count = slot + 1u;
  loc->kind = loc->register_count == 1u ? AST_ABI_LOCATION_REGISTER
                                        : AST_ABI_LOCATION_REGISTER_LIST;
}
void abi_init_value(AstAbiValueInfo *v, const AstSemaType *t, AstMirVReg vr,
                    uint32_t idx) {
  memset(v, 0, sizeof(*v));
  v->source_vreg = vr;
  v->source_index = idx;
  v->type = t;
  v->payload_size = abi_type_size(t);
  v->alignment = abi_type_align(t);
}

AstAbiConfig ast_abi_config_default(AstAbiKind kind) {
  AstAbiConfig c;
  memset(&c, 0, sizeof(c));
  c.kind = kind;
  c.max_diagnostics = 128u;
  c.verify_target_before_lowering = 1u;
  c.fail_closed_scalable_pcs = 1u;
  return c;
}
const char *ast_abi_kind_name(AstAbiKind k) {
  switch (k) {
  case AST_ABI_I386_CDECL:
    return "cdecl";
  case AST_ABI_SYSV_AMD64:
    return "sysv_amd64";
  case AST_ABI_WINDOWS_X64:
    return "windows_x64";
  case AST_ABI_AAPCS64:
    return "aapcs64";
  case AST_ABI_AAPCS64_SVE:
    return "aapcs64_sve";
  case AST_ABI_RISCV_LP64:
    return "riscv_lp64";
  case AST_ABI_RISCV_LP64F:
    return "riscv_lp64f";
  case AST_ABI_RISCV_LP64D:
    return "riscv_lp64d";
  case AST_ABI_RISCV_LP64V:
    return "riscv_lp64v";
  case AST_ABI_RISCV_LP64FV:
    return "riscv_lp64fv";
  case AST_ABI_RISCV_LP64DV:
    return "riscv_lp64dv";
  default:
    return "invalid";
  }
}
const char *ast_abi_location_kind_name(AstAbiLocationKind k) {
  switch (k) {
  case AST_ABI_LOCATION_REGISTER:
    return "register";
  case AST_ABI_LOCATION_REGISTER_LIST:
    return "register_list";
  case AST_ABI_LOCATION_STACK:
    return "stack";
  case AST_ABI_LOCATION_INDIRECT:
    return "indirect";
  case AST_ABI_LOCATION_SPLIT_REG_STACK:
    return "split_reg_stack";
  default:
    return "none";
  }
}
const char *ast_abi_diag_code_name(AstAbiDiagCode c) {
  switch (c) {
  case AST_ABI_DIAG_TARGET_REQUIRED:
    return "abi.target_required";
  case AST_ABI_DIAG_TARGET_INVALID:
    return "abi.target_invalid";
  case AST_ABI_DIAG_ABI_TARGET_MISMATCH:
    return "abi.target_mismatch";
  case AST_ABI_DIAG_UNSUPPORTED_SIGNATURE:
    return "abi.unsupported_signature";
  case AST_ABI_DIAG_UNSUPPORTED_SCALABLE_PCS:
    return "abi.unsupported_scalable_pcs";
  case AST_ABI_DIAG_REQUIRED_FEATURE:
    return "abi.required_feature";
  case AST_ABI_DIAG_INVALID_CALL_SIGNATURE:
    return "abi.invalid_call_signature";
  case AST_ABI_DIAG_INTERNAL:
    return "abi.internal";
  case AST_ABI_DIAG_TOO_MANY_DIAGNOSTICS:
    return "abi.too_many_diagnostics";
  default:
    return "abi.none";
  }
}
AstAbiKind ast_abi_default_for_target(const AstTargetConfig *t) {
  if (!t)
    return AST_ABI_INVALID;
  if (t->arch == AST_TARGET_ARCH_X86_32 || t->arch == AST_TARGET_ARCH_X86_16)
    return AST_ABI_I386_CDECL;
  if (t->arch == AST_TARGET_ARCH_X86_64)
    return t->os == AST_TARGET_OS_WINDOWS ? AST_ABI_WINDOWS_X64
                                          : AST_ABI_SYSV_AMD64;
  if (t->arch == AST_TARGET_ARCH_AARCH64)
    return AST_ABI_AAPCS64;
  if (t->arch == AST_TARGET_ARCH_RISCV64) {
    if (t->features & AST_TARGET_FEATURE_RV_D)
      return AST_ABI_RISCV_LP64D;
    if (t->features & AST_TARGET_FEATURE_RV_F)
      return AST_ABI_RISCV_LP64F;
    return AST_ABI_RISCV_LP64;
  }
  return AST_ABI_INVALID;
}

static int abi_matches_target(AstAbiKind k, AstTargetConfig t) {
  if (k == AST_ABI_I386_CDECL)
    return t.arch == AST_TARGET_ARCH_X86_32 || t.arch == AST_TARGET_ARCH_X86_16;
  if (k == AST_ABI_SYSV_AMD64 || k == AST_ABI_WINDOWS_X64)
    return t.arch == AST_TARGET_ARCH_X86_64;
  if (k == AST_ABI_AAPCS64 || k == AST_ABI_AAPCS64_SVE)
    return t.arch == AST_TARGET_ARCH_AARCH64;
  if (k == AST_ABI_RISCV_LP64 || k == AST_ABI_RISCV_LP64F ||
      k == AST_ABI_RISCV_LP64D || k == AST_ABI_RISCV_LP64V ||
      k == AST_ABI_RISCV_LP64FV || k == AST_ABI_RISCV_LP64DV)
    return t.arch == AST_TARGET_ARCH_RISCV64;
  return 0;
}

int abi_lower_signature(AstAbiProgram *p, const AstSemaType *sig,
                        const AstMirVReg *vregs, uint32_t nv, uint32_t fi,
                        AstMirInstId ci, AbiSignatureResult *out) {
  if (!sig || ast_sema_type_kind(sig) != AST_SEMA_TYPE_FUNCTION)
    return 0;
  if (p->config.kind == AST_ABI_I386_CDECL)
    return abi_lower_i386(p, sig, vregs, nv, fi, ci, out);
  if (p->config.kind == AST_ABI_SYSV_AMD64)
    return abi_lower_sysv(p, sig, vregs, nv, fi, ci, out);
  if (p->config.kind == AST_ABI_WINDOWS_X64)
    return abi_lower_win64(p, sig, vregs, nv, fi, ci, out);
  if (p->config.kind == AST_ABI_AAPCS64 ||
      p->config.kind == AST_ABI_AAPCS64_SVE)
    return abi_lower_aapcs64(p, sig, vregs, nv, fi, ci, out);
  return abi_lower_riscv(p, sig, vregs, nv, fi, ci, out);
}

void abi_fill_masks(AstAbiKind k, AstAbiRegisterMask *caller,
                    AstAbiRegisterMask *callee) {
  memset(caller, 0, sizeof(*caller));
  memset(callee, 0, sizeof(*callee));
  if (k ==
      AST_ABI_I386_CDECL) { /* x86 bits 0..7: eax ecx edx ebx esp ebp esi edi */
    caller->gpr = (UINT64_C(1) << 0) | (UINT64_C(1) << 1) | (UINT64_C(1) << 2);
    callee->gpr = (UINT64_C(1) << 3) | (UINT64_C(1) << 4) | (UINT64_C(1) << 5) |
                  (UINT64_C(1) << 6) | (UINT64_C(1) << 7);
    caller->fp_vector = ~UINT64_C(0);
  } else if (k == AST_ABI_SYSV_AMD64) { /* x86 bits: rax rcx rdx rbx rsp rbp rsi
                                           rdi r8..r15 */
    caller->gpr = (UINT64_C(1) << 0) | (UINT64_C(1) << 1) | (UINT64_C(1) << 2) |
                  (UINT64_C(1) << 6) | (UINT64_C(1) << 7) | (UINT64_C(1) << 8) |
                  (UINT64_C(1) << 9) | (UINT64_C(1) << 10) |
                  (UINT64_C(1) << 11);
    callee->gpr = (UINT64_C(1) << 3) | (UINT64_C(1) << 4) | (UINT64_C(1) << 5) |
                  (UINT64_C(1) << 12) | (UINT64_C(1) << 13) |
                  (UINT64_C(1) << 14) | (UINT64_C(1) << 15);
    caller->fp_vector = UINT64_C(0xffff);
  } else if (k == AST_ABI_WINDOWS_X64) {
    caller->gpr = (UINT64_C(1) << 0) | (UINT64_C(1) << 1) | (UINT64_C(1) << 2) |
                  (UINT64_C(1) << 8) | (UINT64_C(1) << 9) |
                  (UINT64_C(1) << 10) | (UINT64_C(1) << 11);
    callee->gpr = (UINT64_C(1) << 3) | (UINT64_C(1) << 4) | (UINT64_C(1) << 5) |
                  (UINT64_C(1) << 6) | (UINT64_C(1) << 7) |
                  (UINT64_C(1) << 12) | (UINT64_C(1) << 13) |
                  (UINT64_C(1) << 14) | (UINT64_C(1) << 15);
    caller->fp_vector = UINT64_C(0x3f);
    callee->fp_vector = UINT64_C(0xffc0);
  } else if (k == AST_ABI_AAPCS64 || k == AST_ABI_AAPCS64_SVE) {
    for (unsigned i = 0; i <= 17u; i++)
      caller->gpr |= UINT64_C(1) << i;
    caller->gpr |= UINT64_C(1) << 30;
    for (unsigned i = 19; i <= 29u; i++)
      callee->gpr |= UINT64_C(1) << i;
    caller->fp_vector = ~UINT64_C(0);
    for (unsigned i = 8; i <= 15u; i++) {
      caller->fp_vector &= ~(UINT64_C(1) << i);
      callee->fp_vector |= UINT64_C(1) << i;
      callee->fp_vector_low64_only |= UINT64_C(1) << i;
    }
    if (k == AST_ABI_AAPCS64_SVE) {
      for (unsigned i = 8; i <= 23u; i++) {
        caller->fp_vector &= ~(UINT64_C(1) << i);
        callee->fp_vector |= UINT64_C(1) << i;
      }
      callee->fp_vector_low64_only = 0u;
      for (unsigned i = 4; i <= 15u; i++) {
        caller->predicate &= ~(UINT64_C(1) << i);
        callee->predicate |= UINT64_C(1) << i;
      }
    }
  } else {
    for (unsigned i = 5; i <= 7u; i++)
      caller->gpr |= UINT64_C(1) << i;
    for (unsigned i = 10; i <= 17u; i++)
      caller->gpr |= UINT64_C(1) << i;
    for (unsigned i = 28; i <= 31u; i++)
      caller->gpr |= UINT64_C(1) << i;
    caller->gpr |= UINT64_C(1) << 1;
    callee->gpr |= (UINT64_C(1) << 8) | (UINT64_C(1) << 9);
    for (unsigned i = 18; i <= 27u; i++)
      callee->gpr |= UINT64_C(1) << i;
    if (k == AST_ABI_RISCV_LP64F || k == AST_ABI_RISCV_LP64D ||
        k == AST_ABI_RISCV_LP64FV || k == AST_ABI_RISCV_LP64DV) {
      for (unsigned i = 0; i <= 7u; i++)
        caller->fp_vector |= UINT64_C(1) << i;
      for (unsigned i = 10; i <= 17u; i++)
        caller->fp_vector |= UINT64_C(1) << i;
      for (unsigned i = 28; i <= 31u; i++)
        caller->fp_vector |= UINT64_C(1) << i;
      callee->fp_vector |= (UINT64_C(1) << 8) | (UINT64_C(1) << 9);
      for (unsigned i = 18; i <= 27u; i++)
        callee->fp_vector |= UINT64_C(1) << i;
    } else
      caller->fp_vector = ~UINT64_C(0);
    if (k == AST_ABI_RISCV_LP64V || k == AST_ABI_RISCV_LP64FV ||
        k == AST_ABI_RISCV_LP64DV) {
      uint64_t vector_callee = (UINT64_C(0x7f) << 1) | (UINT64_C(0xff) << 24);
      caller->fp_vector |= UINT64_C(0xffffffff);
      caller->fp_vector &= ~vector_callee;
      callee->fp_vector |= vector_callee;
    }
  }
}

static int append_call(AstAbiProgram *p, AbiCall **out) {
  if (p->call_count == p->call_capacity) {
    size_t cap = p->call_capacity ? p->call_capacity * 2u : 16u;
    AbiCall *q = (AbiCall *)realloc(p->calls, cap * sizeof(*q));
    if (!q)
      return 0;
    memset(q + p->call_capacity, 0, (cap - p->call_capacity) * sizeof(*q));
    p->calls = q;
    p->call_capacity = cap;
  }
  *out = &p->calls[p->call_count++];
  return 1;
}

typedef struct AbiLowerContext {
  AstAbiProgram *program;
  AstAbiProgram *results;
  const size_t *offsets;
} AbiLowerContext;
static int abi_lower_function(AstAbiProgram *p, size_t fi) {
  const AstMirFunction *fn = ast_mir_function_at(p->mir, fi);
  AstMirFunctionInfo mi;
  if (!fn || !ast_mir_function_info(fn, &mi))
    return 1;
  uint32_t pc = mi.type ? ast_sema_type_function_param_count(mi.type) : 0u;
  AstMirVReg *vr = (AstMirVReg *)malloc((pc ? pc : 1u) * sizeof(*vr));
  if (!vr) {
    return 0;
  }
  for (uint32_t i = 0; i < pc; i++)
    vr[i] = AST_MIR_INVALID_ID;
  uint32_t found = 0;
  for (uint32_t v = 0; v < mi.vreg_count && found < pc; v++) {
    AstMirVRegInfo vi;
    if (ast_mir_function_vreg(fn, v, &vi) &&
        (vi.flags & AST_MIR_VREG_FUNCTION_ARGUMENT))
      vr[found++] = v;
  }
  AbiSignatureResult sr;
  memset(&sr, 0, sizeof(sr));
  if (!abi_lower_signature(p, mi.type, vr, pc, (uint32_t)fi, AST_MIR_INVALID_ID,
                           &sr)) {
    abi_diag(p, AST_ABI_DIAG_UNSUPPORTED_SIGNATURE, AST_DIAG_ERROR, mi.span,
             (uint32_t)fi, AST_MIR_INVALID_ID,
             "cannot lower function signature for %s",
             ast_abi_kind_name(p->config.kind));
    free(vr);
    return 1;
  }
  free(vr);
  AbiFunction *af = &p->functions[fi];
  af->arguments = sr.arguments;
  af->total_argument_count = sr.argument_count;
  af->info.module_name = mi.module_name;
  af->info.name = mi.name;
  af->info.function_index = (uint32_t)fi;
  af->info.argument_count = sr.argument_count - sr.hidden_count;
  af->info.hidden_argument_count = sr.hidden_count;
  af->info.result = sr.result;
  af->info.outgoing_stack_bytes = sr.stack_bytes;
  af->info.stack_alignment = sr.stack_alignment;
  af->info.shadow_space_bytes = sr.shadow_space;
  af->info.red_zone_bytes = sr.red_zone;
  abi_fill_masks(p->config.kind, &af->info.caller_saved,
                 &af->info.callee_saved);
  p->stats.argument_count += sr.argument_count;
  for (uint32_t ii = 0; ii < mi.instruction_count; ii++) {
    AstMirInstructionInfo inst;
    if (!ast_mir_function_instruction(fn, ii, &inst))
      continue;
    if (inst.op != AST_MIR_OP_CALL_DIRECT &&
        inst.op != AST_MIR_OP_CALL_INDIRECT)
      continue;
    if (!inst.call_signature) {
      abi_diag(p, AST_ABI_DIAG_INVALID_CALL_SIGNATURE, AST_DIAG_ERROR,
               inst.span, (uint32_t)fi, ii,
               "call has no frozen function signature");
      continue;
    }
    uint32_t call_pc = ast_sema_type_function_param_count(inst.call_signature);
    uint32_t first = inst.op == AST_MIR_OP_CALL_INDIRECT ? 1u : 0u;
    if (inst.operand_count < first || inst.operand_count - first != call_pc) {
      abi_diag(p, AST_ABI_DIAG_INVALID_CALL_SIGNATURE, AST_DIAG_ERROR,
               inst.span, (uint32_t)fi, ii,
               "MIR call operand count does not match frozen signature");
      continue;
    }
    AbiSignatureResult cr;
    memset(&cr, 0, sizeof(cr));
    const AstMirVReg *call_args = call_pc ? inst.operands + first : NULL;
    if (!abi_lower_signature(p, inst.call_signature, call_args, call_pc,
                             (uint32_t)fi, ii, &cr))
      continue;
    AbiCall *ac;
    if (!append_call(p, &ac)) {
      free(cr.arguments);
      return 0;
    }
    ac->arguments = cr.arguments;
    ac->total_argument_count = cr.argument_count;
    ac->info.caller_function_index = (uint32_t)fi;
    ac->info.instruction = ii;
    ac->info.is_direct = inst.op == AST_MIR_OP_CALL_DIRECT;
    ac->info.argument_count = cr.argument_count - cr.hidden_count;
    ac->info.result = cr.result;
    ac->info.result.source_vreg = inst.result;
    ac->info.outgoing_stack_bytes = cr.stack_bytes;
    ac->info.shadow_space_bytes = cr.shadow_space;
    ac->info.stack_alignment = cr.stack_alignment;
    p->stats.call_site_count++;
    p->stats.call_argument_count += cr.argument_count;
  }
  return 1;
}
static int abi_lower_task(void *opaque, size_t fi) {
  AbiLowerContext *ctx = opaque;
  AstAbiProgram *local = &ctx->results[fi];
  *local = *ctx->program;
  local->stats = (AstAbiStats){0};
  local->diagnostics = NULL;
  local->diagnostic_count = local->diagnostic_capacity = 0u;
  local->owned_messages = NULL;
  local->owned_message_count = local->owned_message_capacity = 0u;
  local->calls = ctx->program->calls + ctx->offsets[fi];
  local->call_capacity = ctx->offsets[fi + 1u] - ctx->offsets[fi];
  local->call_count = 0u;
  return abi_lower_function(local, fi);
}
static void abi_merge_result(AstAbiProgram *p, AstAbiProgram *r) {
  p->stats.argument_count += r->stats.argument_count;
  p->stats.call_site_count += r->stats.call_site_count;
  p->stats.call_argument_count += r->stats.call_argument_count;
  for (size_t i = 0; i < r->diagnostic_count; ++i) {
    const AstAbiDiagnostic *d = &r->diagnostics[i];
    abi_diag(p, d->code, d->severity, d->span, d->function_index,
             d->instruction, "%s", d->message);
  }
  for (size_t i = 0; i < r->owned_message_count; ++i)
    free(r->owned_messages[i]);
  free(r->owned_messages);
  free(r->diagnostics);
}

AstAbiProgram *ast_abi_lower(const AstTargetProgram *target,
                             const AstAbiConfig *config_in) {
  AstAbiConfig c =
      config_in ? *config_in : ast_abi_config_default(AST_ABI_INVALID);
  AstAbiProgram *p = (AstAbiProgram *)calloc(1, sizeof(*p));
  if (!p)
    return NULL;
  p->target = target;
  p->config = c;
  if (!p->config.max_diagnostics)
    p->config.max_diagnostics = 128u;
  if (!target) {
    abi_diag(p, AST_ABI_DIAG_TARGET_REQUIRED, AST_DIAG_ERROR, (AstSpan){0},
             UINT32_MAX, AST_MIR_INVALID_ID,
             "ABI lowering requires a legalized target program");
    return p;
  }
  p->mir = ast_target_source_mir(target);
  AstTargetConfig tc = ast_target_config(target);
  if (c.kind == AST_ABI_INVALID)
    p->config.kind = ast_abi_default_for_target(&tc);
  if (c.verify_target_before_lowering && !ast_target_verify(target)) {
    abi_diag(p, AST_ABI_DIAG_TARGET_INVALID, AST_DIAG_ERROR, (AstSpan){0},
             UINT32_MAX, AST_MIR_INVALID_ID,
             "target legalization must verify before ABI lowering");
    return p;
  }
  if (!abi_matches_target(p->config.kind, tc)) {
    abi_diag(p, AST_ABI_DIAG_ABI_TARGET_MISMATCH, AST_DIAG_ERROR, (AstSpan){0},
             UINT32_MAX, AST_MIR_INVALID_ID, "ABI %s does not match target %s",
             ast_abi_kind_name(p->config.kind), ast_target_arch_name(tc.arch));
    return p;
  }
  if ((p->config.kind == AST_ABI_AAPCS64_SVE) &&
      !(tc.features & AST_TARGET_FEATURE_A64_SVE)) {
    abi_diag(p, AST_ABI_DIAG_REQUIRED_FEATURE, AST_DIAG_ERROR, (AstSpan){0},
             UINT32_MAX, AST_MIR_INVALID_ID,
             "AAPCS64 SVE PCS requires the AArch64 SVE feature");
    return p;
  }
  if ((p->config.kind == AST_ABI_RISCV_LP64F ||
       p->config.kind == AST_ABI_RISCV_LP64FV) &&
      !(tc.features & AST_TARGET_FEATURE_RV_F)) {
    abi_diag(p, AST_ABI_DIAG_REQUIRED_FEATURE, AST_DIAG_ERROR, (AstSpan){0},
             UINT32_MAX, AST_MIR_INVALID_ID,
             "LP64F requires the RISC-V F extension");
    return p;
  }
  if ((p->config.kind == AST_ABI_RISCV_LP64D ||
       p->config.kind == AST_ABI_RISCV_LP64DV) &&
      !(tc.features & AST_TARGET_FEATURE_RV_D)) {
    abi_diag(p, AST_ABI_DIAG_REQUIRED_FEATURE, AST_DIAG_ERROR, (AstSpan){0},
             UINT32_MAX, AST_MIR_INVALID_ID,
             "LP64D requires the RISC-V D extension");
    return p;
  }
  if ((p->config.kind == AST_ABI_RISCV_LP64V ||
       p->config.kind == AST_ABI_RISCV_LP64FV ||
       p->config.kind == AST_ABI_RISCV_LP64DV) &&
      !(tc.features & AST_TARGET_FEATURE_RV_V)) {
    abi_diag(p, AST_ABI_DIAG_REQUIRED_FEATURE, AST_DIAG_ERROR, (AstSpan){0},
             UINT32_MAX, AST_MIR_INVALID_ID,
             "RISC-V vector PCS requires the V extension");
    return p;
  }
  p->function_count = ast_mir_function_count(p->mir);
  p->functions = (AbiFunction *)calloc(
      p->function_count ? p->function_count : 1u, sizeof(*p->functions));
  if (!p->functions) {
    ast_abi_destroy(p);
    return NULL;
  }
  p->stats.function_count = p->function_count;
  size_t *offsets = calloc(p->function_count + 1u, sizeof(*offsets));
  AstAbiProgram *results =
      calloc(p->function_count ? p->function_count : 1u, sizeof(*results));
  if (!offsets || !results) {
    free(offsets);
    free(results);
    ast_abi_destroy(p);
    return NULL;
  }
  for (size_t fi = 0; fi < p->function_count; ++fi) {
    offsets[fi + 1u] = offsets[fi];
    const AstMirFunction *fn = ast_mir_function_at(p->mir, fi);
    AstMirFunctionInfo mi;
    if (!fn || !ast_mir_function_info(fn, &mi))
      continue;
    for (uint32_t ii = 0; ii < mi.instruction_count; ++ii) {
      AstMirInstructionInfo inst;
      if (ast_mir_function_instruction(fn, ii, &inst) &&
          (inst.op == AST_MIR_OP_CALL_DIRECT ||
           inst.op == AST_MIR_OP_CALL_INDIRECT)) {
        if (offsets[fi + 1u] == SIZE_MAX) {
          free(offsets);
          free(results);
          ast_abi_destroy(p);
          return NULL;
        }
        ++offsets[fi + 1u];
      }
    }
  }
  p->call_capacity = p->call_count = offsets[p->function_count];
  p->calls = calloc(p->call_count ? p->call_count : 1u, sizeof(*p->calls));
  if (!p->calls) {
    free(offsets);
    free(results);
    ast_abi_destroy(p);
    return NULL;
  }
  AbiLowerContext ctx = {p, results, offsets};
  int ok =
      ast_parallel_check(p->function_count, 0u, abi_lower_task, &ctx, NULL);
  size_t written = 0u;
  for (size_t fi = 0; fi < p->function_count; ++fi) {
    AstAbiProgram *r = &results[fi];
    if (r->call_count)
      memmove(p->calls + written, r->calls, r->call_count * sizeof(*p->calls));
    written += r->call_count;
    abi_merge_result(p, r);
  }
  p->call_count = written;
  free(offsets);
  free(results);
  if (!ok) {
    ast_abi_destroy(p);
    return NULL;
  }
  return p;
}

void ast_abi_destroy(AstAbiProgram *p) {
  if (!p)
    return;
  if (p->functions)
    for (size_t i = 0; i < p->function_count; i++)
      free(p->functions[i].arguments);
  if (p->calls)
    for (size_t i = 0; i < p->call_count; i++)
      free(p->calls[i].arguments);
  for (size_t i = 0; i < p->owned_message_count; i++)
    free(p->owned_messages[i]);
  free(p->owned_messages);
  free(p->diagnostics);
  free(p->functions);
  free(p->calls);
  free(p);
}
static int abi_location_valid(const AstAbiLocation *l, int allow_none) {
  if (!l)
    return 0;
  if (l->kind == AST_ABI_LOCATION_NONE)
    return allow_none;
  if (l->register_count > AST_ABI_MAX_REGISTERS)
    return 0;
  for (uint32_t i = 0u; i < l->register_count; ++i)
    if (!l->registers[i][0])
      return 0;
  if (l->kind == AST_ABI_LOCATION_REGISTER)
    return l->register_count == 1u;
  if (l->kind == AST_ABI_LOCATION_REGISTER_LIST)
    return l->register_count >= 2u;
  if (l->kind == AST_ABI_LOCATION_STACK)
    return l->register_count == 0u && l->stack_alignment != 0u &&
           (l->stack_alignment & (l->stack_alignment - 1u)) == 0u &&
           (l->stack_offset % l->stack_alignment) == 0u && l->stack_size != 0u;
  if (l->kind == AST_ABI_LOCATION_INDIRECT)
    return l->register_count <= 1u &&
           (l->register_count == 1u ||
            (l->stack_alignment != 0u &&
             (l->stack_alignment & (l->stack_alignment - 1u)) == 0u));
  if (l->kind == AST_ABI_LOCATION_SPLIT_REG_STACK)
    return l->register_count >= 1u && l->stack_size != 0u &&
           l->stack_alignment != 0u &&
           (l->stack_alignment & (l->stack_alignment - 1u)) == 0u &&
           (l->stack_offset % l->stack_alignment) == 0u;
  return 0;
}

static int stack_contract_valid(AstAbiKind kind, const AstAbiValueInfo *args,
                                uint32_t count, uint64_t bytes,
                                uint32_t alignment) {
  uint32_t required = 16u;
  if (count && !args)
    return 0;
  for (uint32_t i = 0; i < count; ++i) {
    const AstAbiLocation *loc = &args[i].location;
    if (loc->kind != AST_ABI_LOCATION_STACK &&
        loc->kind != AST_ABI_LOCATION_SPLIT_REG_STACK &&
        !(loc->kind == AST_ABI_LOCATION_INDIRECT && !loc->register_count))
      continue;
    if (!loc->stack_alignment ||
        (loc->stack_alignment & (loc->stack_alignment - 1u)) ||
        loc->stack_offset > bytes ||
        loc->stack_size > bytes - loc->stack_offset)
      return 0;
    if (kind == AST_ABI_SYSV_AMD64 && loc->stack_alignment > required)
      required = loc->stack_alignment;
  }
  return alignment == required && !(bytes % alignment);
}
int ast_abi_verify(const AstAbiProgram *p) {
  if (!p || !p->target || !p->mir || p->stats.error_count ||
      !ast_target_verify(p->target))
    return 0;
  if (p->function_count != ast_mir_function_count(p->mir) ||
      p->stats.function_count != p->function_count ||
      p->stats.call_site_count != p->call_count)
    return 0;
  for (size_t fi = 0u; fi < p->function_count; ++fi) {
    const AbiFunction *f = &p->functions[fi];
    const AstMirFunction *mf = ast_mir_function_at(p->mir, fi);
    AstMirFunctionInfo mfi;
    if (!mf || !ast_mir_function_info(mf, &mfi) ||
        f->info.function_index != fi ||
        !stack_contract_valid(
            p->config.kind, f->arguments, f->total_argument_count,
            f->info.outgoing_stack_bytes, f->info.stack_alignment))
      return 0;
    uint32_t pc = mfi.type ? ast_sema_type_function_param_count(mfi.type) : 0u;
    if (f->info.argument_count != pc ||
        f->total_argument_count != pc + f->info.hidden_argument_count ||
        (f->total_argument_count && !f->arguments))
      return 0;
    const AstSemaType *ret =
        mfi.type ? ast_sema_type_function_result(mfi.type) : NULL;
    int void_result = !ret || ast_sema_type_kind(ret) == AST_SEMA_TYPE_VOID;
    int conceptual_sret =
        f->info.hidden_argument_count != 0u &&
        f->info.result.location.kind == AST_ABI_LOCATION_INDIRECT &&
        f->info.result.location.register_count == 0u;
    if (!conceptual_sret &&
        !abi_location_valid(&f->info.result.location, void_result))
      return 0;
    if (p->config.kind == AST_ABI_WINDOWS_X64 &&
        f->info.shadow_space_bytes != 32u)
      return 0;
    if (p->config.kind == AST_ABI_SYSV_AMD64 && f->info.red_zone_bytes != 128u)
      return 0;
    for (uint32_t i = 0u; i < f->total_argument_count; ++i) {
      const AstAbiValueInfo *v = &f->arguments[i];
      if (!v->type || !abi_location_valid(&v->location, 0))
        return 0;
      if (i < f->info.hidden_argument_count) {
        if (v->source_vreg != AST_MIR_INVALID_ID ||
            !(v->flags & AST_ABI_VALUE_HIDDEN))
          return 0;
      } else {
        if (v->source_index != i - f->info.hidden_argument_count)
          return 0;
        if (mfi.flags & AST_HIR_FUNCTION_EXTERN) {
          if (v->source_vreg != AST_MIR_INVALID_ID)
            return 0;
        } else {
          AstMirVRegInfo vi;
          if (v->source_vreg == AST_MIR_INVALID_ID ||
              !ast_mir_function_vreg(mf, v->source_vreg, &vi) ||
              !(vi.flags & AST_MIR_VREG_FUNCTION_ARGUMENT) ||
              vi.type != v->type)
            return 0;
        }
      }
    }
    if (p->config.kind == AST_ABI_AAPCS64) {
      uint64_t low64 = UINT64_C(0xff) << 8u;
      if ((f->info.callee_saved.fp_vector_low64_only & low64) != low64)
        return 0;
    }
  }
  for (size_t ci = 0u; ci < p->call_count; ++ci) {
    const AbiCall *c = &p->calls[ci];
    if (ci && (p->calls[ci - 1u].info.caller_function_index >
                   c->info.caller_function_index ||
               (p->calls[ci - 1u].info.caller_function_index ==
                    c->info.caller_function_index &&
                p->calls[ci - 1u].info.instruction >= c->info.instruction)))
      return 0;
    if (c->info.caller_function_index >= p->function_count ||
        c->info.instruction == AST_MIR_INVALID_ID ||
        c->total_argument_count < c->info.argument_count ||
        !stack_contract_valid(
            p->config.kind, c->arguments, c->total_argument_count,
            c->info.outgoing_stack_bytes, c->info.stack_alignment))
      return 0;
    const AstMirFunction *mf =
        ast_mir_function_at(p->mir, c->info.caller_function_index);
    AstMirInstructionInfo mi;
    if (!mf || !ast_mir_function_instruction(mf, c->info.instruction, &mi) ||
        (mi.op != AST_MIR_OP_CALL_DIRECT &&
         mi.op != AST_MIR_OP_CALL_INDIRECT) ||
        !mi.call_signature)
      return 0;
    uint32_t hidden = c->total_argument_count - c->info.argument_count;
    uint32_t first = mi.op == AST_MIR_OP_CALL_INDIRECT ? 1u : 0u;
    if (c->info.argument_count !=
            ast_sema_type_function_param_count(mi.call_signature) ||
        mi.operand_count < first ||
        mi.operand_count - first != c->info.argument_count)
      return 0;
    if (c->info.result.source_vreg != mi.result)
      return 0;
    int conceptual_sret =
        hidden != 0u &&
        c->info.result.location.kind == AST_ABI_LOCATION_INDIRECT &&
        c->info.result.location.register_count == 0u;
    if (!conceptual_sret &&
        !abi_location_valid(&c->info.result.location,
                            mi.result == AST_MIR_INVALID_ID))
      return 0;
    for (uint32_t i = 0u; i < c->total_argument_count; ++i) {
      const AstAbiValueInfo *v = &c->arguments[i];
      if (!abi_location_valid(&v->location, 0))
        return 0;
      if (i < hidden) {
        if (v->source_vreg != AST_MIR_INVALID_ID ||
            !(v->flags & AST_ABI_VALUE_HIDDEN))
          return 0;
      } else if (v->source_vreg != mi.operands[first + i - hidden])
        return 0;
    }
    if (p->config.kind == AST_ABI_WINDOWS_X64 &&
        c->info.shadow_space_bytes != 32u)
      return 0;
  }
  return 1;
}
const AstTargetProgram *ast_abi_source_target(const AstAbiProgram *p) {
  return p ? p->target : NULL;
}
AstAbiConfig ast_abi_config(const AstAbiProgram *p) {
  return p ? p->config : ast_abi_config_default(AST_ABI_INVALID);
}
AstAbiStats ast_abi_stats(const AstAbiProgram *p) {
  AstAbiStats z;
  memset(&z, 0, sizeof(z));
  return p ? p->stats : z;
}
const AstAbiDiagnostic *ast_abi_diagnostics(const AstAbiProgram *p, size_t *c) {
  if (c)
    *c = p ? p->diagnostic_count : 0u;
  return p ? p->diagnostics : NULL;
}
size_t ast_abi_function_count(const AstAbiProgram *p) {
  return p ? p->function_count : 0u;
}
int ast_abi_function_info(const AstAbiProgram *p, size_t i,
                          AstAbiFunctionInfo *out) {
  if (!p || !out || i >= p->function_count)
    return 0;
  *out = p->functions[i].info;
  return 1;
}
int ast_abi_function_argument(const AstAbiProgram *p, size_t fi, uint32_t i,
                              AstAbiValueInfo *out) {
  if (!p || !out || fi >= p->function_count ||
      i >= p->functions[fi].total_argument_count)
    return 0;
  *out = p->functions[fi].arguments[i];
  return 1;
}
size_t ast_abi_call_count(const AstAbiProgram *p) {
  return p ? p->call_count : 0u;
}
int ast_abi_call_info(const AstAbiProgram *p, size_t i, AstAbiCallInfo *out) {
  if (!p || !out || i >= p->call_count)
    return 0;
  *out = p->calls[i].info;
  return 1;
}
int ast_abi_call_argument(const AstAbiProgram *p, size_t ci, uint32_t i,
                          AstAbiValueInfo *out) {
  if (!p || !out || ci >= p->call_count ||
      i >= p->calls[ci].total_argument_count)
    return 0;
  *out = p->calls[ci].arguments[i];
  return 1;
}
