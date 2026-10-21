#include "../../abi/abi_lookup.h"
#include "../../isa/riscv64_branch_contract.h"
#include "machine_internal.h"

static void machine_function_release(MachineFunction *fn) {
  if (!fn)
    return;
  if (fn->blocks) {
    for (uint32_t i = 0; i < fn->info.block_count; ++i) {
      free(fn->blocks[i].instructions);
      free(fn->blocks[i].arguments);
    }
  }
  free(fn->blocks);
  free(fn->instructions);
  free(fn->vregs);
  free(fn->operands);
  free(fn->source_first);
  free(fn->source_parts);
  free(fn->vreg_alias);
  memset(fn, 0, sizeof(*fn));
}

AstMachineConfig ast_machine_config_default(void) {
  AstMachineConfig c;
  c.verify_sources = 1u;
  c.verify_after_each_stage = 1u;
  c.enable_peephole = 1u;
  return c;
}

AstMachineRegBank machine_bank_for_class(AstTargetRegClass rc) {
  switch (rc) {
  case AST_TARGET_REGCLASS_GPR8:
  case AST_TARGET_REGCLASS_GPR16:
  case AST_TARGET_REGCLASS_GPR32:
  case AST_TARGET_REGCLASS_GPR64:
  case AST_TARGET_REGCLASS_GPR_PAIR:
    return AST_MACHINE_BANK_GPR;
  case AST_TARGET_REGCLASS_FP16:
  case AST_TARGET_REGCLASS_FP32:
  case AST_TARGET_REGCLASS_FP64:
  case AST_TARGET_REGCLASS_FP128:
    return AST_MACHINE_BANK_FP;
  case AST_TARGET_REGCLASS_VEC64:
  case AST_TARGET_REGCLASS_VEC128:
  case AST_TARGET_REGCLASS_VEC256:
  case AST_TARGET_REGCLASS_VEC512:
  case AST_TARGET_REGCLASS_SCALABLE_VECTOR:
    return AST_MACHINE_BANK_VECTOR;
  case AST_TARGET_REGCLASS_MASK:
  case AST_TARGET_REGCLASS_PREDICATE:
    return AST_MACHINE_BANK_MASK;
  case AST_TARGET_REGCLASS_SPECIAL:
    return AST_MACHINE_BANK_SPECIAL;
  default:
    return AST_MACHINE_BANK_INVALID;
  }
}

int machine_register_bank(const AstTargetConfig *tc, const char *name,
                          AstMachineRegBank *out) {
  AstTargetRegisterInfo ri;
  if (out)
    *out = AST_MACHINE_BANK_INVALID;
  if (!tc || !name || !out || !ast_target_parse_register(tc, name, &ri))
    return 0;
  *out = machine_bank_for_class(ri.reg_class);
  return *out != AST_MACHINE_BANK_INVALID;
}

int machine_register_compatible(const AstTargetConfig *tc, const char *name,
                                AstMachineRegBank expected) {
  AstTargetRegisterInfo ri;
  if (!tc || !name || !ast_target_parse_register(tc, name, &ri))
    return 0;
  AstMachineRegBank actual = machine_bank_for_class(ri.reg_class);
  if (actual == expected)
    return 1;
  /* x86 XMM and AArch64 SIMD scalar views share their register file with
     vector values. The register parser classifies both as vector views. */
  return (tc->arch == AST_TARGET_ARCH_X86_64 ||
          tc->arch == AST_TARGET_ARCH_AARCH64) &&
         expected == AST_MACHINE_BANK_FP && actual == AST_MACHINE_BANK_VECTOR;
}

AstMachineOpcode machine_generic_opcode(AstMirOp op) {
  switch (op) {
  case AST_MIR_OP_CONSTANT:
    return AST_MACHINE_G_CONSTANT;
  case AST_MIR_OP_UNDEF:
    return AST_MACHINE_G_UNDEF;
  case AST_MIR_OP_COPY:
    return AST_MACHINE_G_COPY;
  case AST_MIR_OP_STACK_ADDR:
    return AST_MACHINE_G_STACK_ADDR;
  case AST_MIR_OP_STACK_LOAD:
  case AST_MIR_OP_GLOBAL_LOAD:
  case AST_MIR_OP_LOAD:
    return AST_MACHINE_G_LOAD;
  case AST_MIR_OP_STACK_STORE:
  case AST_MIR_OP_GLOBAL_STORE:
  case AST_MIR_OP_STORE:
    return AST_MACHINE_G_STORE;
  case AST_MIR_OP_GLOBAL_ADDR:
    return AST_MACHINE_G_GLOBAL_ADDR;
  case AST_MIR_OP_FUNCTION_ADDR:
    return AST_MACHINE_G_FUNCTION_ADDR;
  case AST_MIR_OP_GENERIC_CONST:
    return AST_MACHINE_G_GENERIC_CONST;
  case AST_MIR_OP_UNARY:
    return AST_MACHINE_G_UNARY;
  case AST_MIR_OP_BINARY:
    return AST_MACHINE_G_BINARY;
  case AST_MIR_OP_CAST:
    return AST_MACHINE_G_CAST;
  case AST_MIR_OP_SELECT:
    return AST_MACHINE_G_SELECT;
  case AST_MIR_OP_PACK:
    return AST_MACHINE_G_PACK;
  case AST_MIR_OP_CALL_DIRECT:
    return AST_MACHINE_G_CALL_DIRECT;
  case AST_MIR_OP_CALL_INDIRECT:
    return AST_MACHINE_G_CALL_INDIRECT;
  case AST_MIR_OP_INDEX:
    return AST_MACHINE_G_INDEX;
  case AST_MIR_OP_MEMBER:
    return AST_MACHINE_G_MEMBER;
  case AST_MIR_OP_ADDRESS_OF:
    return AST_MACHINE_G_ADDRESS_OF;
  case AST_MIR_OP_ATOMIC:
    return AST_MACHINE_G_ATOMIC;
  case AST_MIR_OP_FENCE:
    return AST_MACHINE_G_FENCE;
  case AST_MIR_OP_INTRINSIC:
    return AST_MACHINE_G_INTRINSIC;
  case AST_MIR_OP_BIND:
    return AST_MACHINE_G_BIND;
  case AST_MIR_OP_NATIVE:
    return AST_MACHINE_G_NATIVE;
  case AST_MIR_OP_NATIVE_RESULT:
    return AST_MACHINE_G_NATIVE_RESULT;
  default:
    return AST_MACHINE_OP_INVALID;
  }
}

int machine_opcode_is_generic(AstMachineOpcode op) {
  return op >= AST_MACHINE_G_CONSTANT && op <= AST_MACHINE_G_UNREACHABLE;
}

int machine_opcode_is_selected(AstTargetArch arch, AstMachineOpcode op) {
  switch (arch) {
  case AST_TARGET_ARCH_X86_64:
    return op >= AST_MACHINE_X86_UNDEF && op <= AST_MACHINE_X86_FP_TO_UINT64;
  case AST_TARGET_ARCH_AARCH64:
    return (op >= AST_MACHINE_A64_UNDEF && op <= AST_MACHINE_A64_TRAP) ||
           op == AST_MACHINE_A64_NATIVE_CBZ ||
           op == AST_MACHINE_A64_NATIVE_CBNZ;
  case AST_TARGET_ARCH_RISCV64:
    return (op >= AST_MACHINE_RISCV_UNDEF && op <= AST_MACHINE_RISCV_EBREAK) ||
           rv_native_branch_funct3(op) >= 0;
  default:
    return 0;
  }
}

int machine_opcode_terminator_kind(AstMachineOpcode op) {
  switch (op) {
  case AST_MACHINE_G_BR:
  case AST_MACHINE_X86_BR:
  case AST_MACHINE_A64_BR:
  case AST_MACHINE_RISCV_J:
    return AST_MIR_TERM_BR;
  case AST_MACHINE_G_COND_BR:
  case AST_MACHINE_X86_COND_BR:
  case AST_MACHINE_X86_ICMP_BR:
  case AST_MACHINE_A64_COND_BR:
  case AST_MACHINE_RISCV_COND_BR:
    return AST_MIR_TERM_COND_BR;
  case AST_MACHINE_G_RET:
  case AST_MACHINE_X86_RET:
  case AST_MACHINE_A64_RET:
  case AST_MACHINE_RISCV_RET:
    return AST_MIR_TERM_RETURN;
  case AST_MACHINE_G_UNREACHABLE:
  case AST_MACHINE_X86_TRAP:
  case AST_MACHINE_A64_TRAP:
  case AST_MACHINE_RISCV_EBREAK:
    return AST_MIR_TERM_UNREACHABLE;
  default:
    return 0;
  }
}

static int reserve_operands(MachineFunction *fn, uint32_t extra) {
  if (extra > UINT32_MAX - fn->operand_count)
    return 0;
  uint32_t need = fn->operand_count + extra;
  if (need <= fn->operand_capacity)
    return 1;
  uint32_t cap = fn->operand_capacity ? fn->operand_capacity : 64u;
  while (cap < need) {
    if (cap > UINT32_MAX / 2u) {
      cap = need;
      break;
    }
    cap *= 2u;
  }
  AstMachineOperand *next =
      (AstMachineOperand *)realloc(fn->operands, (size_t)cap * sizeof(*next));
  if (!next)
    return 0;
  fn->operands = next;
  fn->operand_capacity = cap;
  return 1;
}

static int append_operand(MachineFunction *fn, const AstMachineOperand *op) {
  if (!reserve_operands(fn, 1u))
    return 0;
  fn->operands[fn->operand_count++] = *op;
  return 1;
}

static int append_source_value(MachineFunction *fn, AstMirVReg source) {
  if (source >= fn->source_vreg_count)
    return 0;
  uint32_t first = fn->source_first[source], parts = fn->source_parts[source];
  if (first == AST_MACHINE_INVALID_ID || parts == 0u)
    return 0;
  for (uint32_t i = 0; i < parts; ++i) {
    AstMachineOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = AST_MACHINE_OPERAND_VREG;
    op.as.vreg = first + i;
    if (!append_operand(fn, &op))
      return 0;
  }
  return 1;
}

static int append_block_operand(MachineFunction *fn, AstMirBlockId block) {
  AstMachineOperand op;
  memset(&op, 0, sizeof(op));
  op.kind = AST_MACHINE_OPERAND_BLOCK;
  op.as.block = block;
  return append_operand(fn, &op);
}

static uint32_t bit_width_for_part(const AstTargetValueInfo *tv,
                                   const AstMirVRegInfo *mv) {
  if (tv && tv->legal_bit_width)
    return tv->legal_bit_width;
  if (mv && mv->bit_width)
    return mv->bit_width;
  if (mv && mv->type) {
    uint32_t b = ast_sema_type_bit_width(mv->type);
    if (b)
      return b;
    uint64_t s = ast_sema_type_size(mv->type);
    if (s != UINT64_MAX && s <= UINT32_MAX / 8u)
      return (uint32_t)s * 8u;
  }
  return 0u;
}

static int build_vregs(AstMachineProgram *p, MachineFunction *fn,
                       const AstMirFunction *mf, uint32_t function_index,
                       const AstMirFunctionInfo *mi) {
  fn->source_vreg_count = mi->vreg_count;
  size_t mapn = mi->vreg_count ? (size_t)mi->vreg_count : 1u;
  uint32_t *source_first = (uint32_t *)malloc(mapn * sizeof(uint32_t));
  uint32_t *source_parts = (uint32_t *)calloc(mapn, sizeof(uint32_t));
  if (!source_first || !source_parts) {
    free(source_first);
    free(source_parts);
    return 0;
  }
  fn->source_first = source_first;
  fn->source_parts = source_parts;
  for (uint32_t i = 0; i < mi->vreg_count; ++i)
    fn->source_first[i] = AST_MACHINE_INVALID_ID;

  uint64_t total = 0u;
  for (uint32_t vr = 0; vr < mi->vreg_count; ++vr) {
    AstTargetValueInfo tv;
    if (!ast_target_value_info(p->target, function_index, vr, &tv))
      return 0;
    uint32_t parts = tv.part_count ? tv.part_count : 1u;
    if (total + parts > UINT32_MAX)
      return 0;
    total += parts;
  }
  fn->info.vreg_count = (uint32_t)total;
  fn->vregs = (AstMachineVRegInfo *)calloc(total ? (size_t)total : 1u,
                                           sizeof(*fn->vregs));
  if (!fn->vregs)
    return 0;

  uint32_t cursor = 0u;
  for (uint32_t vr = 0; vr < mi->vreg_count; ++vr) {
    AstTargetValueInfo tv;
    AstMirVRegInfo mv;
    if (!ast_target_value_info(p->target, function_index, vr, &tv) ||
        !ast_mir_function_vreg(mf, vr, &mv))
      return 0;
    uint32_t parts = tv.part_count ? tv.part_count : 1u;
    fn->source_first[vr] = cursor;
    fn->source_parts[vr] = parts;
    if (parts > 1u)
      p->stats.split_value_count++;
    for (uint32_t pi = 0; pi < parts; ++pi) {
      AstMachineVRegInfo *v = &fn->vregs[cursor];
      v->id = cursor;
      v->source_vreg = vr;
      v->type = mv.type;
      v->reg_class = tv.reg_class;
      v->bit_width = bit_width_for_part(&tv, &mv);
      v->part_index = pi;
      v->part_count = parts;
      if (mv.flags & AST_MIR_VREG_FUNCTION_ARGUMENT)
        v->flags |= AST_MACHINE_VREG_FUNCTION_ARGUMENT;
      if (parts > 1u)
        v->flags |= AST_MACHINE_VREG_SPLIT_PART;
      cursor++;
    }
  }
  return 1;
}

static int abi_value_register_compatible(const AstMachineProgram *p,
                                         const MachineFunction *fn,
                                         const AstAbiValueInfo *av,
                                         uint32_t first, uint32_t parts) {
  if (!p || !fn || !av || first == AST_MACHINE_INVALID_ID || parts == 0u ||
      first >= fn->info.vreg_count || parts > fn->info.vreg_count - first)
    return 0;
  if (av->location.kind != AST_ABI_LOCATION_REGISTER &&
      av->location.kind != AST_ABI_LOCATION_REGISTER_LIST)
    return 0;
  if (av->location.register_count != parts)
    return 0;
  AstTargetConfig tc = ast_target_config(p->target);
  for (uint32_t i = 0; i < parts; ++i) {
    AstMachineRegBank expected =
        machine_bank_for_class(fn->vregs[first + i].reg_class);
    if (expected == AST_MACHINE_BANK_INVALID ||
        !machine_register_compatible(&tc, av->location.registers[i], expected))
      return 0;
  }
  return 1;
}

static int set_vreg_def_constraint(AstMachineProgram *p, MachineFunction *fn,
                                   const AstAbiValueInfo *av) {
  if (!av || av->source_vreg == AST_MIR_INVALID_ID ||
      av->source_vreg >= fn->source_vreg_count)
    return 1;
  uint32_t first = fn->source_first[av->source_vreg],
           parts = fn->source_parts[av->source_vreg];
  if (!abi_value_register_compatible(p, fn, av, first, parts)) {
    if (first != AST_MACHINE_INVALID_ID) {
      for (uint32_t i = 0; i < parts; ++i)
        fn->vregs[first + i].flags |= AST_MACHINE_VREG_ABI_DEFERRED;
      p->stats.deferred_abi_value_count++;
    }
    return 1;
  }
  for (uint32_t i = 0; i < parts; ++i) {
    AstMachineVRegInfo *v = &fn->vregs[first + i];
    size_t n = strlen(av->location.registers[i]);
    if (n >= sizeof(v->fixed_register))
      return 0;
    if (!(v->flags & AST_MACHINE_VREG_FIXED_REGISTER))
      p->stats.fixed_register_count++;
    memcpy(v->fixed_register, av->location.registers[i], n + 1u);
    v->flags |= AST_MACHINE_VREG_FIXED_REGISTER;
  }
  return 1;
}

static int set_operand_constraint(AstMachineProgram *p, MachineFunction *fn,
                                  uint32_t operand_index, const char *reg) {
  if (!p || !fn || operand_index >= fn->operand_count || !reg)
    return 0;
  AstMachineOperand *o = &fn->operands[operand_index];
  if (o->kind != AST_MACHINE_OPERAND_VREG || o->as.vreg >= fn->info.vreg_count)
    return 0;
  AstMachineRegBank expected =
      machine_bank_for_class(fn->vregs[o->as.vreg].reg_class);
  AstTargetConfig tc = ast_target_config(p->target);
  if (!machine_register_compatible(&tc, reg, expected))
    return 0;
  size_t n = strlen(reg);
  if (n >= sizeof(o->fixed_register))
    return 0;
  memcpy(o->fixed_register, reg, n + 1u);
  o->flags |= AST_MACHINE_OPERAND_FIXED_REGISTER;
  p->stats.fixed_operand_count++;
  return 1;
}

static int find_abi_call(const AstMachineProgram *p, uint32_t function_index,
                         AstMirInstId iid, size_t *call_index,
                         AstAbiCallInfo *out) {
  return p && abi_find_call(p->abi, function_index, iid, call_index, out);
}

static int apply_call_constraints(AstMachineProgram *p, MachineFunction *fn,
                                  uint32_t function_index,
                                  const AstMirInstructionInfo *mi,
                                  MachineInstruction *out) {
  if (!mi ||
      (mi->op != AST_MIR_OP_CALL_DIRECT && mi->op != AST_MIR_OP_CALL_INDIRECT))
    return 1;
  size_t call_index;
  AstAbiCallInfo ci;
  if (!find_abi_call(p, function_index, mi->id, &call_index, &ci))
    return 0;
  uint32_t hidden = 0u;
  AstAbiValueInfo first_av;
  if (ast_abi_call_argument(p->abi, call_index, 0u, &first_av) &&
      first_av.source_vreg == AST_MIR_INVALID_ID &&
      (first_av.flags & AST_ABI_VALUE_HIDDEN))
    hidden = 1u;

  uint32_t source_first = mi->op == AST_MIR_OP_CALL_INDIRECT ? 1u : 0u;
  uint32_t pool_cursor = out->operand_offset;
  if (source_first) {
    AstMirVReg callee = mi->operands[0];
    if (callee >= fn->source_vreg_count)
      return 0;
    pool_cursor += fn->source_parts[callee];
  }
  for (uint32_t ai = 0; ai < ci.argument_count; ++ai) {
    AstAbiValueInfo av;
    if (!ast_abi_call_argument(p->abi, call_index, hidden + ai, &av))
      return 0;
    if (source_first + ai >= mi->operand_count ||
        av.source_vreg != mi->operands[source_first + ai] ||
        av.source_vreg >= fn->source_vreg_count)
      return 0;
    uint32_t parts = fn->source_parts[av.source_vreg];
    uint32_t first = fn->source_first[av.source_vreg];
    if (abi_value_register_compatible(p, fn, &av, first, parts)) {
      for (uint32_t pi = 0; pi < parts; ++pi)
        if (!set_operand_constraint(p, fn, pool_cursor + pi,
                                    av.location.registers[pi]))
          return 0;
    }
    pool_cursor += parts;
  }
  if (ci.result.source_vreg != AST_MIR_INVALID_ID &&
      !set_vreg_def_constraint(p, fn, &ci.result))
    return 0;
  return 1;
}

static int apply_return_constraints(AstMachineProgram *p, MachineFunction *fn,
                                    uint32_t function_index) {
  AstAbiFunctionInfo fi;
  if (!ast_abi_function_info(p->abi, function_index, &fi))
    return 0;
  for (uint32_t i = 0; i < fn->info.instruction_count; ++i) {
    MachineInstruction *mi = &fn->instructions[i];
    if (mi->info.opcode != AST_MACHINE_G_RET || mi->info.operand_count == 0u)
      continue;
    if (fi.result.location.kind != AST_ABI_LOCATION_REGISTER &&
        fi.result.location.kind != AST_ABI_LOCATION_REGISTER_LIST)
      continue;
    if (fi.result.location.register_count != mi->info.operand_count)
      continue;
    int compatible = 1;
    for (uint32_t oi = 0; oi < mi->info.operand_count; ++oi) {
      AstMachineOperand *o = &fn->operands[mi->operand_offset + oi];
      if (o->kind != AST_MACHINE_OPERAND_VREG ||
          o->as.vreg >= fn->info.vreg_count) {
        compatible = 0;
        break;
      }
      AstMachineRegBank expected =
          machine_bank_for_class(fn->vregs[o->as.vreg].reg_class);
      AstTargetConfig tc = ast_target_config(p->target);
      if (!machine_register_compatible(&tc, fi.result.location.registers[oi],
                                       expected)) {
        compatible = 0;
        break;
      }
    }
    if (compatible)
      for (uint32_t oi = 0; oi < mi->info.operand_count; ++oi)
        if (!set_operand_constraint(p, fn, mi->operand_offset + oi,
                                    fi.result.location.registers[oi]))
          return 0;
  }
  return 1;
}

static int set_fixed_if_compatible(AstMachineProgram *p, MachineFunction *fn,
                                   const AstAbiValueInfo *av) {
  return set_vreg_def_constraint(p, fn, av);
}

static int apply_function_abi(AstMachineProgram *p, MachineFunction *fn,
                              uint32_t function_index) {
  AstAbiFunctionInfo fi;
  if (!ast_abi_function_info(p->abi, function_index, &fi))
    return 0;
  uint32_t total = fi.argument_count + fi.hidden_argument_count;
  for (uint32_t i = 0; i < total; ++i) {
    AstAbiValueInfo av;
    if (!ast_abi_function_argument(p->abi, function_index, i, &av))
      return 0;
    if (!(av.flags & AST_ABI_VALUE_HIDDEN) &&
        !set_fixed_if_compatible(p, fn, &av))
      return 0;
  }
  return 1;
}

static int build_instruction(AstMachineProgram *p, MachineFunction *fn,
                             const AstMirFunction *mf, uint32_t function_index,
                             AstMirInstId iid, MachineInstruction *out) {
  AstMirInstructionInfo mi;
  AstTargetInstructionInfo ti;
  if (!ast_mir_function_instruction(mf, iid, &mi) ||
      !ast_target_instruction_info(p->target, function_index, iid, &ti))
    return 0;
  memset(out, 0, sizeof(*out));
  out->info.id = iid;
  out->info.opcode = machine_generic_opcode(mi.op);
  out->info.source_op = mi.op;
  out->info.source_instruction = iid;
  out->info.token_op = mi.token_op;
  out->info.intrinsic = mi.intrinsic;
  out->info.memory_order = mi.memory_order;
  out->info.failure_memory_order = mi.failure_memory_order;
  out->info.span = mi.span;
  out->info.effects = ti.narrowed_effects;
  out->info.flags = mi.flags;
  out->operand_offset = fn->operand_count;
  if (mi.result != AST_MIR_INVALID_ID) {
    if (mi.result >= fn->source_vreg_count)
      return 0;
    out->info.result_first = fn->source_first[mi.result];
    out->info.result_count = fn->source_parts[mi.result];
  } else
    out->info.result_first = AST_MACHINE_INVALID_ID;

  for (uint32_t oi = 0; oi < mi.operand_count; ++oi)
    if (!append_source_value(fn, mi.operands[oi]))
      return 0;

  if (mi.op == AST_MIR_OP_STACK_ADDR || mi.op == AST_MIR_OP_STACK_LOAD ||
      mi.op == AST_MIR_OP_STACK_STORE) {
    AstMachineOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = AST_MACHINE_OPERAND_STACK_SLOT;
    op.as.stack_slot = mi.stack_slot;
    if (!append_operand(fn, &op))
      return 0;
  }
  if ((mi.op == AST_MIR_OP_CALL_DIRECT || mi.op == AST_MIR_OP_GLOBAL_ADDR ||
       mi.op == AST_MIR_OP_GLOBAL_LOAD || mi.op == AST_MIR_OP_GLOBAL_STORE ||
       mi.op == AST_MIR_OP_FUNCTION_ADDR ||
       mi.op == AST_MIR_OP_GENERIC_CONST) &&
      mi.symbol.span.length) {
    AstMachineOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = AST_MACHINE_OPERAND_SYMBOL;
    op.as.symbol = mi.symbol;
    op.module = mi.module;
    if (!append_operand(fn, &op))
      return 0;
  }
  if (mi.op == AST_MIR_OP_CONSTANT && mi.constant.kind != AST_SEMA_CONST_NONE) {
    AstMachineOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = AST_MACHINE_OPERAND_IMMEDIATE;
    /* Form two's-complement bits in unsigned arithmetic; negating the
       signed INT64_MIN cast would be undefined behavior. */
    uint64_t bits =
        mi.constant.is_negative ? UINT64_C(0) - mi.constant.lo : mi.constant.lo;
    op.as.immediate =
        bits <= INT64_MAX ? (int64_t)bits : -(int64_t)(UINT64_MAX - bits) - 1;
    if (!append_operand(fn, &op))
      return 0;
  }
  if (!apply_call_constraints(p, fn, function_index, &mi, out))
    return 0;
  out->info.operand_count = fn->operand_count - out->operand_offset;
  return out->info.opcode != AST_MACHINE_OP_INVALID;
}

static int append_terminator(MachineFunction *fn, const AstMirBlockInfo *bi,
                             uint32_t *cursor) {
  MachineInstruction *out = &fn->instructions[*cursor];
  memset(out, 0, sizeof(*out));
  out->info.id = *cursor;
  out->info.source_instruction = AST_MIR_INVALID_ID;
  out->info.source_op = AST_MIR_OP_INVALID;
  out->info.span = bi->terminator.span;
  out->info.result_first = AST_MACHINE_INVALID_ID;
  out->operand_offset = fn->operand_count;
  switch (bi->terminator.kind) {
  case AST_MIR_TERM_BR:
    out->info.opcode = AST_MACHINE_G_BR;
    if (!append_block_operand(fn, bi->terminator.target_true))
      return 0;
    for (uint32_t a = 0; a < bi->terminator.true_arg_count; ++a)
      if (!append_source_value(fn, bi->terminator.true_args[a]))
        return 0;
    break;
  case AST_MIR_TERM_COND_BR:
    out->info.opcode = AST_MACHINE_G_COND_BR;
    if (!append_source_value(fn, bi->terminator.condition_or_value) ||
        !append_block_operand(fn, bi->terminator.target_true))
      return 0;
    for (uint32_t a = 0; a < bi->terminator.true_arg_count; ++a)
      if (!append_source_value(fn, bi->terminator.true_args[a]))
        return 0;
    if (!append_block_operand(fn, bi->terminator.target_false))
      return 0;
    for (uint32_t a = 0; a < bi->terminator.false_arg_count; ++a)
      if (!append_source_value(fn, bi->terminator.false_args[a]))
        return 0;
    break;
  case AST_MIR_TERM_RETURN:
    out->info.opcode = AST_MACHINE_G_RET;
    if (bi->terminator.condition_or_value != AST_MIR_INVALID_ID &&
        !append_source_value(fn, bi->terminator.condition_or_value))
      return 0;
    break;
  case AST_MIR_TERM_UNREACHABLE:
    out->info.opcode = AST_MACHINE_G_UNREACHABLE;
    break;
  default:
    return 0;
  }
  out->info.operand_count = fn->operand_count - out->operand_offset;
  (*cursor)++;
  return 1;
}

void machine_refresh_operand_views(MachineFunction *fn) {
  if (!fn || !fn->instructions)
    return;
  for (uint32_t i = 0; i < fn->info.instruction_count; ++i) {
    MachineInstruction *mi = &fn->instructions[i];
    mi->info.operands =
        mi->info.operand_count ? fn->operands + mi->operand_offset : NULL;
  }
}

int machine_build_function(AstMachineProgram *p, uint32_t function_index) {
  const AstMirFunction *mf = ast_mir_function_at(p->mir, function_index);
  AstMirFunctionInfo mi;
  if (!mf || !ast_mir_function_info(mf, &mi))
    return 0;
  MachineFunction *fn = &p->functions[function_index];
  fn->info.module_name = mi.module_name;
  fn->info.name = mi.name;
  fn->info.function_index = function_index;
  fn->info.entry_block = mi.entry_block;
  fn->info.block_count = mi.block_count;
  if (!build_vregs(p, fn, mf, function_index, &mi) ||
      !apply_function_abi(p, fn, function_index))
    goto fail;

  uint64_t total_inst = (uint64_t)mi.instruction_count + mi.block_count;
  if (total_inst > UINT32_MAX)
    goto fail;
  fn->info.instruction_count = (uint32_t)total_inst;
  fn->instructions = (MachineInstruction *)calloc(
      total_inst ? (size_t)total_inst : 1u, sizeof(*fn->instructions));
  fn->blocks = (MachineBlock *)calloc(
      mi.block_count ? (size_t)mi.block_count : 1u, sizeof(*fn->blocks));
  if (!fn->instructions || !fn->blocks)
    goto fail;

  uint32_t cursor = 0u;
  for (uint32_t b = 0; b < mi.block_count; ++b) {
    AstMirBlockInfo bi;
    if (!ast_mir_function_block(mf, b, &bi))
      goto fail;
    MachineBlock *mb = &fn->blocks[b];
    mb->info.id = b;
    mb->info.reachable = bi.reachable;
    mb->info.loop_depth = bi.loop_depth;
    for (uint32_t a = 0; a < bi.argument_count; ++a) {
      if (bi.arguments[a] >= fn->source_vreg_count ||
          fn->source_parts[bi.arguments[a]] >
              UINT32_MAX - mb->info.argument_count)
        goto fail;
      mb->info.argument_count += fn->source_parts[bi.arguments[a]];
    }
    if (mb->info.argument_count) {
      mb->arguments = (AstMachineVReg *)calloc(mb->info.argument_count,
                                               sizeof(*mb->arguments));
      if (!mb->arguments)
        goto fail;
      uint32_t ac = 0u;
      for (uint32_t a = 0; a < bi.argument_count; ++a)
        for (uint32_t part = 0; part < fn->source_parts[bi.arguments[a]];
             ++part)
          mb->arguments[ac++] = fn->source_first[bi.arguments[a]] + part;
      mb->info.arguments = mb->arguments;
    }
    uint32_t count = bi.instruction_count + 1u;
    mb->instructions = (AstMachineInstId *)calloc(count ? count : 1u,
                                                  sizeof(*mb->instructions));
    if (!mb->instructions)
      goto fail;
    mb->info.instruction_count = count;
    for (uint32_t j = 0; j < bi.instruction_count; ++j) {
      if (!build_instruction(p, fn, mf, function_index, bi.instructions[j],
                             &fn->instructions[cursor]))
        goto fail;
      fn->instructions[cursor].info.id = cursor;
      mb->instructions[j] = cursor++;
    }
    if (!append_terminator(fn, &bi, &cursor))
      goto fail;
    mb->instructions[count - 1u] = cursor - 1u;
    mb->info.instructions = mb->instructions;
  }
  if (cursor != fn->info.instruction_count)
    goto fail;
  if (!apply_return_constraints(p, fn, function_index))
    goto fail;
  machine_refresh_operand_views(fn);
  p->stats.block_count += fn->info.block_count;
  p->stats.instruction_count += fn->info.instruction_count;
  p->stats.vreg_count += fn->info.vreg_count;
  return 1;
fail:
  machine_function_release(fn);
  return 0;
}

typedef struct MachineBuildContext {
  AstMachineProgram *program;
  AstMachineStats *results;
} MachineBuildContext;
static int machine_build_task(void *opaque, size_t index) {
  MachineBuildContext *ctx = opaque;
  AstMachineProgram local = *ctx->program;
  memset(&local.stats, 0, sizeof(local.stats));
  int ok = machine_build_function(&local, (uint32_t)index);
  ctx->results[index] = local.stats;
  return ok;
}

AstMachineProgram *ast_machine_build(const AstAbiProgram *abi,
                                     const AstMachineConfig *config) {
  if (!abi)
    return NULL;
  AstMachineConfig c = config ? *config : ast_machine_config_default();
  const AstTargetProgram *target = ast_abi_source_target(abi);
  const AstMirProgram *mir = target ? ast_target_source_mir(target) : NULL;
  if (!target || !mir)
    return NULL;
  if (c.verify_sources && (!ast_abi_verify(abi) || !ast_target_verify(target)))
    return NULL;
  size_t n = ast_mir_function_count(mir);
  if (n > UINT32_MAX)
    return NULL;
  AstMachineProgram *p = (AstMachineProgram *)calloc(1u, sizeof(*p));
  if (!p)
    return NULL;
  p->abi = abi;
  p->target = target;
  p->mir = mir;
  p->config = c;
  p->stage = AST_MACHINE_STAGE_GENERIC;
  p->function_count = n;
  p->stats.function_count = n;
  p->functions = (MachineFunction *)calloc(n ? n : 1u, sizeof(*p->functions));
  if (!p->functions) {
    free(p);
    return NULL;
  }
  AstMachineStats *results = calloc(n ? n : 1u, sizeof(*results));
  if (!results) {
    ast_machine_destroy(p);
    return NULL;
  }
  MachineBuildContext ctx = {p, results};
  int ok = ast_parallel_check(n, 0u, machine_build_task, &ctx, NULL);
  for (size_t i = 0; i < n; ++i) {
    p->stats.block_count += results[i].block_count;
    p->stats.instruction_count += results[i].instruction_count;
    p->stats.vreg_count += results[i].vreg_count;
    p->stats.fixed_register_count += results[i].fixed_register_count;
    p->stats.fixed_operand_count += results[i].fixed_operand_count;
    p->stats.split_value_count += results[i].split_value_count;
    p->stats.deferred_abi_value_count += results[i].deferred_abi_value_count;
  }
  free(results);
  if (!ok) {
    ast_machine_destroy(p);
    return NULL;
  }
  if (c.enable_peephole && !machine_run_peephole(p)) {
    ast_machine_destroy(p);
    return NULL;
  }
  if (c.verify_after_each_stage && !ast_machine_verify(p)) {
    ast_machine_destroy(p);
    return NULL;
  }
  return p;
}

void ast_machine_destroy(AstMachineProgram *p) {
  if (!p)
    return;
  if (p->functions)
    for (size_t i = 0; i < p->function_count; ++i)
      machine_function_release(&p->functions[i]);
  free(p->functions);
  free(p);
}

AstMachineStage ast_machine_stage(const AstMachineProgram *p) {
  return p ? p->stage : AST_MACHINE_STAGE_INVALID;
}
AstMachineStats ast_machine_stats(const AstMachineProgram *p) {
  AstMachineStats z;
  memset(&z, 0, sizeof(z));
  return p ? p->stats : z;
}
const AstAbiProgram *ast_machine_source_abi(const AstMachineProgram *p) {
  return p ? p->abi : NULL;
}
size_t ast_machine_function_count(const AstMachineProgram *p) {
  return p ? p->function_count : 0u;
}
int ast_machine_function_info(const AstMachineProgram *p, size_t fi,
                              AstMachineFunctionInfo *out) {
  if (!p || !out || fi >= p->function_count)
    return 0;
  *out = p->functions[fi].info;
  return 1;
}
int ast_machine_function_block(const AstMachineProgram *p, size_t fi,
                               AstMachineBlockId b, AstMachineBlockInfo *out) {
  if (!p || !out || fi >= p->function_count ||
      b >= p->functions[fi].info.block_count)
    return 0;
  *out = p->functions[fi].blocks[b].info;
  return 1;
}
int ast_machine_function_instruction(const AstMachineProgram *p, size_t fi,
                                     AstMachineInstId i,
                                     AstMachineInstructionInfo *out) {
  if (!p || !out || fi >= p->function_count ||
      i >= p->functions[fi].info.instruction_count)
    return 0;
  *out = p->functions[fi].instructions[i].info;
  return 1;
}
int ast_machine_function_vreg(const AstMachineProgram *p, size_t fi,
                              AstMachineVReg v, AstMachineVRegInfo *out) {
  if (!p || !out || fi >= p->function_count ||
      v >= p->functions[fi].info.vreg_count)
    return 0;
  *out = p->functions[fi].vregs[v];
  return 1;
}
