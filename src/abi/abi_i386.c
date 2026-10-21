#include "abi_internal.h"

/* AsterASM's i386 cdecl contract follows the System V i386 value locations
 * used by the legacy encoder.  Source arguments are passed by value in a
 * contiguous 32-bit-word stack area.  We retain 16-byte call-site alignment
 * (the encoder already reserves outgoing areas to 16 bytes), while each
 * argument begins at a 4-byte-aligned offset.  Integer/pointer results use
 * EAX or EDX:EAX and floating results use x87 ST(0).
 *
 * Aggregate returns use the historical SysV hidden return-buffer pointer as
 * the first stack argument and report EAX as the returned buffer address.
 * The current legacy encoder may still reject aggregate-result codegen until
 * its callee-pop return-buffer sequence is selected; keeping the ABI model
 * explicit prevents it from silently using another architecture's ABI.
 */

static int i386_stack_value(AstAbiValueInfo *v, uint64_t *stack) {
  uint64_t size = abi_type_size(v->type);
  if (!size || size == UINT64_MAX)
    return 0;
  uint64_t slot = abi_align_up(size, 4u);
  *stack = abi_align_up(*stack, 4u);
  v->location.kind = AST_ABI_LOCATION_STACK;
  v->location.stack_offset = *stack;
  v->location.stack_size = slot;
  v->location.stack_alignment = 4u;
  v->payload_size = size;
  *stack += slot;
  return 1;
}

int abi_lower_i386(AstAbiProgram *p, const AstSemaType *signature,
                   const AstMirVReg *source_vregs, uint32_t source_vreg_count,
                   uint32_t function_index, AstMirInstId call_inst,
                   AbiSignatureResult *out) {
  (void)function_index;
  (void)call_inst;
  memset(out, 0, sizeof(*out));
  uint32_t pc = ast_sema_type_function_param_count(signature);
  if (source_vreg_count != pc)
    return 0;

  const AstSemaType *ret = ast_sema_type_function_result(signature);
  const int sret = ret && ast_sema_type_kind(ret) != AST_SEMA_TYPE_VOID &&
                   abi_is_aggregate(ret);
  abi_init_value(&out->result, ret, AST_MIR_INVALID_ID, UINT32_MAX);
  out->result.flags |= AST_ABI_VALUE_RESULT;
  if (ret && ast_sema_type_kind(ret) != AST_SEMA_TYPE_VOID) {
    uint64_t size = abi_type_size(ret);
    if (sret) {
      out->result.location.kind = AST_ABI_LOCATION_INDIRECT;
      abi_set_reg(&out->result.location, 0u, "eax");
      out->result.location.kind = AST_ABI_LOCATION_INDIRECT;
      out->result.extension = AST_ABI_EXT_BY_REFERENCE;
      out->result.flags |=
          AST_ABI_VALUE_INDIRECT_COPY | AST_ABI_VALUE_RETURN_POINTER_IN_GPR;
    } else if (abi_is_float(ret)) {
      if (size != 4u && size != 8u)
        return 0;
      abi_set_reg(&out->result.location, 0u, "st0");
    } else if (abi_is_integer(ret)) {
      if (size <= 4u)
        abi_set_reg(&out->result.location, 0u, "eax");
      else if (size <= 8u) {
        abi_set_reg(&out->result.location, 0u, "eax");
        abi_set_reg(&out->result.location, 1u, "edx");
      } else
        return 0;
    } else
      return 0;
  }

  out->hidden_count = sret ? 1u : 0u;
  out->argument_count = pc + out->hidden_count;
  out->arguments = (AstAbiValueInfo *)calloc(
      out->argument_count ? out->argument_count : 1u, sizeof(*out->arguments));
  if (!out->arguments)
    return 0;
  uint64_t stack = 0u;
  if (sret) {
    AstAbiValueInfo *h = &out->arguments[0];
    abi_init_value(h, ret, AST_MIR_INVALID_ID, UINT32_MAX);
    h->flags = AST_ABI_VALUE_HIDDEN | AST_ABI_VALUE_INDIRECT_COPY;
    h->extension = AST_ABI_EXT_BY_REFERENCE;
    h->payload_size = 4u;
    h->alignment = 4u;
    h->location.kind = AST_ABI_LOCATION_STACK;
    h->location.stack_offset = 0u;
    h->location.stack_size = 4u;
    h->location.stack_alignment = 4u;
    stack = 4u;
    p->stats.hidden_sret_count++;
    p->stats.stack_argument_count++;
  }
  for (uint32_t i = 0; i < pc; ++i) {
    const AstSemaType *type = ast_sema_type_function_param(signature, i);
    if (!type || abi_is_scalable(type) || abi_is_vector(type))
      return 0;
    AstAbiValueInfo *v = &out->arguments[out->hidden_count + i];
    abi_init_value(v, type, source_vregs ? source_vregs[i] : AST_MIR_INVALID_ID,
                   i);
    if (!i386_stack_value(v, &stack))
      return 0;
    p->stats.stack_argument_count++;
  }
  out->stack_alignment = 16u;
  out->stack_bytes = abi_align_up(stack, out->stack_alignment);
  return 1;
}
