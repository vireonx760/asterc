#ifndef ASTERASM_ABI_LOOKUP_H
#define ASTERASM_ABI_LOOKUP_H
#include "asterasm/abi.h"

/* ABI lowering and verification guarantee (function, instruction) order.
   Binary lookup avoids rescanning every other function's calls at each
   Machine IR instruction and at every frame/physical lowering boundary. */
static inline size_t abi_call_lower_bound(const AstAbiProgram *p,
                                          uint32_t function,
                                          AstMirInstId instruction) {
  size_t lo = 0u, hi = ast_abi_call_count(p);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    AstAbiCallInfo call;
    if (!ast_abi_call_info(p, mid, &call))
      return ast_abi_call_count(p);
    if (call.caller_function_index < function ||
        (call.caller_function_index == function &&
         call.instruction < instruction))
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo;
}
static inline int abi_find_call(const AstAbiProgram *p, uint32_t function,
                                AstMirInstId instruction, size_t *index,
                                AstAbiCallInfo *out) {
  size_t i = abi_call_lower_bound(p, function, instruction);
  AstAbiCallInfo call;
  if (!ast_abi_call_info(p, i, &call) ||
      call.caller_function_index != function || call.instruction != instruction)
    return 0;
  if (index)
    *index = i;
  if (out)
    *out = call;
  return 1;
}
#endif
