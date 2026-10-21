#include "machine_internal.h"

typedef struct Dump {
  void (*write_fn)(void *, const char *, size_t);
  void *user;
} Dump;
static void out(Dump *d, const char *s) {
  if (d && d->write_fn && s)
    d->write_fn(d->user, s, strlen(s));
}
static void pf(Dump *d, const char *fmt, ...) ASTER_PRINTF_LIKE(2, 3);
static void pf(Dump *d, const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if (n <= 0)
    return;
  size_t z = (size_t)n < sizeof(b) ? (size_t)n : sizeof(b) - 1u;
  d->write_fn(d->user, b, z);
}
const char *ast_machine_regbank_name(AstMachineRegBank b) {
  switch (b) {
  case AST_MACHINE_BANK_GPR:
    return "gpr";
  case AST_MACHINE_BANK_FP:
    return "fp";
  case AST_MACHINE_BANK_VECTOR:
    return "vector";
  case AST_MACHINE_BANK_MASK:
    return "mask";
  case AST_MACHINE_BANK_SPECIAL:
    return "special";
  default:
    return "invalid";
  }
}
const char *ast_machine_opcode_name(AstMachineOpcode op) {
#define C(x)                                                                   \
  case x:                                                                      \
    return #x
  switch (op) {
    C(AST_MACHINE_G_CONSTANT);
    C(AST_MACHINE_G_UNDEF);
    C(AST_MACHINE_G_COPY);
    C(AST_MACHINE_G_STACK_ADDR);
    C(AST_MACHINE_G_LOAD);
    C(AST_MACHINE_G_STORE);
    C(AST_MACHINE_G_GLOBAL_ADDR);
    C(AST_MACHINE_G_FUNCTION_ADDR);
    C(AST_MACHINE_G_GENERIC_CONST);
    C(AST_MACHINE_G_UNARY);
    C(AST_MACHINE_G_BINARY);
    C(AST_MACHINE_G_CAST);
    C(AST_MACHINE_G_SELECT);
    C(AST_MACHINE_G_PACK);
    C(AST_MACHINE_G_CALL_DIRECT);
    C(AST_MACHINE_G_CALL_INDIRECT);
    C(AST_MACHINE_G_INDEX);
    C(AST_MACHINE_G_MEMBER);
    C(AST_MACHINE_G_ADDRESS_OF);
    C(AST_MACHINE_G_ATOMIC);
    C(AST_MACHINE_G_FENCE);
    C(AST_MACHINE_G_INTRINSIC);
    C(AST_MACHINE_G_BIND);
    C(AST_MACHINE_G_NATIVE);
    C(AST_MACHINE_G_NATIVE_RESULT);
    C(AST_MACHINE_G_BR);
    C(AST_MACHINE_G_COND_BR);
    C(AST_MACHINE_G_RET);
    C(AST_MACHINE_G_UNREACHABLE);
    C(AST_MACHINE_X86_UNDEF);
    C(AST_MACHINE_X86_MOV);
    C(AST_MACHINE_X86_MOV_IMM);
    C(AST_MACHINE_X86_LEA);
    C(AST_MACHINE_X86_LOAD);
    C(AST_MACHINE_X86_STORE);
    C(AST_MACHINE_X86_NEG);
    C(AST_MACHINE_X86_NOT);
    C(AST_MACHINE_X86_ADD);
    C(AST_MACHINE_X86_SUB);
    C(AST_MACHINE_X86_MUL);
    C(AST_MACHINE_X86_UMUL_WIDE);
    C(AST_MACHINE_X86_SDIV);
    C(AST_MACHINE_X86_UDIV);
    C(AST_MACHINE_X86_SREM);
    C(AST_MACHINE_X86_UREM);
    C(AST_MACHINE_X86_AND);
    C(AST_MACHINE_X86_OR);
    C(AST_MACHINE_X86_XOR);
    C(AST_MACHINE_X86_SHL);
    C(AST_MACHINE_X86_ASHR);
    C(AST_MACHINE_X86_LSHR);
    C(AST_MACHINE_X86_ICMP_EQ);
    C(AST_MACHINE_X86_ICMP_NE);
    C(AST_MACHINE_X86_ICMP_LT);
    C(AST_MACHINE_X86_ICMP_LE);
    C(AST_MACHINE_X86_ICMP_GT);
    C(AST_MACHINE_X86_ICMP_GE);
    C(AST_MACHINE_X86_FADD);
    C(AST_MACHINE_X86_FSUB);
    C(AST_MACHINE_X86_FMUL);
    C(AST_MACHINE_X86_FDIV);
    C(AST_MACHINE_X86_FREM);
    C(AST_MACHINE_X86_FCMP_EQ);
    C(AST_MACHINE_X86_FCMP_NE);
    C(AST_MACHINE_X86_FCMP_LT);
    C(AST_MACHINE_X86_FCMP_LE);
    C(AST_MACHINE_X86_FCMP_GT);
    C(AST_MACHINE_X86_FCMP_GE);
    C(AST_MACHINE_X86_VEC_ADD);
    C(AST_MACHINE_X86_VEC_SUB);
    C(AST_MACHINE_X86_VEC_MUL);
    C(AST_MACHINE_X86_VEC_AND);
    C(AST_MACHINE_X86_VEC_OR);
    C(AST_MACHINE_X86_VEC_XOR);
    C(AST_MACHINE_X86_VEC_CMP);
    C(AST_MACHINE_X86_CAST);
    C(AST_MACHINE_X86_SELECT);
    C(AST_MACHINE_X86_CALL);
    C(AST_MACHINE_X86_CALL_INDIRECT);
    C(AST_MACHINE_X86_INDEX);
    C(AST_MACHINE_X86_MEMBER);
    C(AST_MACHINE_X86_ATOMIC);
    C(AST_MACHINE_X86_FENCE);
    C(AST_MACHINE_X86_INTRINSIC);
    C(AST_MACHINE_X86_BIND);
    C(AST_MACHINE_X86_NATIVE);
    C(AST_MACHINE_X86_NATIVE_RESULT);
    C(AST_MACHINE_X86_BR);
    C(AST_MACHINE_X86_COND_BR);
    C(AST_MACHINE_X86_RET);
    C(AST_MACHINE_X86_TRAP);
    C(AST_MACHINE_X86_CDQ);
    C(AST_MACHINE_X86_CQO);
    C(AST_MACHINE_X86_MOVSX);
    C(AST_MACHINE_X86_MOVZX);
    C(AST_MACHINE_X86_NOP);
    C(AST_MACHINE_X86_CLI);
    C(AST_MACHINE_X86_STI);
    C(AST_MACHINE_X86_HLT);
    C(AST_MACHINE_X86_CLD);
    C(AST_MACHINE_X86_LOCKED_ATOMIC);
    C(AST_MACHINE_X86_IN);
    C(AST_MACHINE_X86_OUT);
    C(AST_MACHINE_X86_LIDT);
    C(AST_MACHINE_X86_LGDT);
    C(AST_MACHINE_X86_PUSH);
    C(AST_MACHINE_X86_POP);
    C(AST_MACHINE_X86_JMP);
    C(AST_MACHINE_X86_JMP_INDIRECT);
    C(AST_MACHINE_X86_RAW_BYTES);
    C(AST_MACHINE_X86_ICMP_BR);
    C(AST_MACHINE_X86_POPCNT);
    C(AST_MACHINE_X86_BSF);
    C(AST_MACHINE_X86_BSR);
    C(AST_MACHINE_X86_BSWAP);
    C(AST_MACHINE_X86_ROL);
    C(AST_MACHINE_X86_ROR);
    C(AST_MACHINE_X86_FMOV_BITS);
    C(AST_MACHINE_X86_SINT_TO_FP);
    C(AST_MACHINE_X86_FP_TO_SINT);
    C(AST_MACHINE_X86_FP_CONVERT);
    C(AST_MACHINE_X86_UINT64_TO_FP);
    C(AST_MACHINE_X86_FP_TO_UINT64);
    C(AST_MACHINE_X86_SYSTEM);
    C(AST_MACHINE_X86_VECTOR);
#define AST_MACHINE_OPCODE(name) C(AST_MACHINE_##name);
#include "asterasm/detail/aarch64_opcodes.inc"
#include "asterasm/detail/riscv64_opcodes.inc"
#undef AST_MACHINE_OPCODE
  default:
    return "AST_MACHINE_OP_INVALID";
  }
#undef C
}
void ast_machine_dump_program(const AstMachineProgram *p,
                              void (*write_fn)(void *, const char *, size_t),
                              void *user) {
  if (!p || !write_fn)
    return;
  Dump d = {write_fn, user};
  pf(&d, "machine stage=%u functions=%zu insts=%zu vregs=%zu\n",
     (unsigned)p->stage, p->function_count, p->stats.instruction_count,
     p->stats.vreg_count);
  for (size_t fi = 0; fi < p->function_count; ++fi) {
    const MachineFunction *fn = &p->functions[fi];
    pf(&d, "fn %.*s.%.*s vregs=%u blocks=%u\n",
       (int)fn->info.module_name.span.length, fn->info.module_name.text,
       (int)fn->info.name.span.length, fn->info.name.text, fn->info.vreg_count,
       fn->info.block_count);
    for (uint32_t v = 0; v < fn->info.vreg_count; ++v) {
      const AstMachineVRegInfo *x = &fn->vregs[v];
      pf(&d, "  %%m%u src=%%r%u part=%u/%u bank=%s class=%s", v, x->source_vreg,
         x->part_index + 1u, x->part_count, ast_machine_regbank_name(x->bank),
         ast_target_reg_class_name(x->reg_class));
      if (x->fixed_register[0])
        pf(&d, " fixed=%s", x->fixed_register);
      if (x->flags & AST_MACHINE_VREG_ABI_DEFERRED)
        out(&d, " abi-deferred");
      out(&d, "\n");
    }
    for (uint32_t b = 0; b < fn->info.block_count; ++b) {
      pf(&d, "  bb%u:\n", b);
      const MachineBlock *mb = &fn->blocks[b];
      for (uint32_t j = 0; j < mb->info.instruction_count; ++j) {
        const MachineInstruction *mi = &fn->instructions[mb->instructions[j]];
        pf(&d, "    %u %s", mi->info.id,
           ast_machine_opcode_name(mi->info.opcode));
        if (mi->info.memory_order != AST_MEMORY_ORDER_NONE) {
          pf(&d, " order[%s", ast_memory_order_name(mi->info.memory_order));
          if (mi->info.failure_memory_order != AST_MEMORY_ORDER_NONE)
            pf(&d, ",fail=%s",
               ast_memory_order_name(mi->info.failure_memory_order));
          out(&d, "]");
        }
        for (uint32_t oi = 0; oi < mi->info.operand_count; ++oi) {
          const AstMachineOperand *o = &mi->info.operands[oi];
          if (o->kind == AST_MACHINE_OPERAND_VREG)
            pf(&d, " %%m%u", o->as.vreg);
          else if (o->kind == AST_MACHINE_OPERAND_BLOCK)
            pf(&d, " bb%u", o->as.block);
          else if (o->kind == AST_MACHINE_OPERAND_STACK_SLOT)
            pf(&d, " stack%u", o->as.stack_slot);
          else if (o->kind == AST_MACHINE_OPERAND_IMMEDIATE)
            pf(&d, " $%lld", (long long)o->as.immediate);
          else if (o->kind == AST_MACHINE_OPERAND_SYMBOL) {
            if (o->module.text && o->module.span.length)
              pf(&d, " @%.*s.%.*s", (int)o->module.span.length, o->module.text,
                 (int)o->as.symbol.span.length, o->as.symbol.text);
            else
              pf(&d, " @%.*s", (int)o->as.symbol.span.length,
                 o->as.symbol.text);
          }
        }
        out(&d, "\n");
      }
    }
  }
}
