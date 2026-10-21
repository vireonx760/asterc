#include "mir_internal.h"

/* Fail-closed structural edge rewrite: only an original, HIR-mapped literal
 * bool instruction can discharge the conditional.  No SCCP speculation or
 * use of a rewritten constant as its own proof is permitted. */
static int proven_literal(const AstMirFunction *fn, AstMirBlockId block,
                          const AstMirTerminatorInfo *term,
                          uint8_t *take_true) {
  if (!fn || !take_true || block >= fn->block_count ||
      term->kind != AST_MIR_TERM_COND_BR ||
      term->condition_or_value >= fn->vreg_count || !fn->source_hir ||
      block >= fn->source_hir->block_count ||
      fn->source_hir->blocks[block].term.kind != AST_HIR_TERM_COND_BR)
    return 0;
  const AstHirTerminator *hir_term = &fn->source_hir->blocks[block].term;
  AstMirVReg reg = term->condition_or_value;
  const AstMirVRegInfo *info = &fn->vregs[reg].info;
  if (info->kind != AST_MIR_VREG_INSTRUCTION_RESULT ||
      ast_sema_type_kind(info->type) != AST_SEMA_TYPE_BOOL ||
      info->definition_index >= fn->instruction_count ||
      info->block >= fn->block_count ||
      hir_term->condition_or_value >= fn->source_hir->value_count)
    return 0;
  const AstHirValue *hv = &fn->source_hir->values[hir_term->condition_or_value];
  if (hv->kind != AST_HIR_VALUE_INSTRUCTION_RESULT ||
      hv->definition_index >= fn->source_hir->instruction_count)
    return 0;
  const AstMirInstruction *inst = &fn->instructions[info->definition_index];
  if (inst->info.op != AST_MIR_OP_CONSTANT || inst->info.result != reg ||
      inst->info.flags || inst->info.effects || inst->info.operand_count ||
      inst->info.constant.kind != AST_SEMA_CONST_BOOL ||
      inst->info.constant.lo > 1u || inst->info.constant.is_negative ||
      inst->info.constant.hi ||
      inst->info.source_hir_instruction != hv->definition_index ||
      (fn->gvn_certificates &&
       fn->gvn_certificates[info->definition_index].valid))
    return 0;
  const AstHirInstruction *hi =
      &fn->source_hir->instructions[hv->definition_index];
  if (hi->op != AST_HIR_OP_CONSTANT ||
      hi->result != hir_term->condition_or_value ||
      hi->constant.kind != AST_SEMA_CONST_BOOL ||
      hi->constant.lo != inst->info.constant.lo)
    return 0;
  /* Prove SSA dominance of the literal's definition. */
  if (info->block == block) {
    int found = 0;
    const AstMirBlock *bb = &fn->blocks[block];
    for (uint32_t i = 0; i < bb->instruction_count; ++i)
      if (bb->instructions[i] == info->definition_index) {
        found = 1;
        break;
      }
    if (!found)
      return 0;
  } else {
    AstMirBlockId cursor = block;
    int found = 0;
    for (uint32_t i = 0; i < fn->block_count; ++i) {
      if (cursor == info->block) {
        found = 1;
        break;
      }
      if (cursor == AST_MIR_INVALID_ID || cursor >= fn->block_count)
        break;
      cursor = fn->blocks[cursor].immediate_dominator;
    }
    if (!found)
      return 0;
  }
  *take_true = (uint8_t)inst->info.constant.lo;
  return 1;
}

int mir_branch_verify_certificates(AstMirProgram *program,
                                   const AstMirFunction *fn) {
  if (!fn || !fn->branch_certificates)
    return 1;
  for (uint32_t bi = 0; bi < fn->block_count; ++bi) {
    const MirBranchCertificate *cert = &fn->branch_certificates[bi];
    if (!cert->valid)
      continue;
    const AstMirTerminatorInfo *old = &cert->original.info;
    const AstMirTerminatorInfo *now = &fn->blocks[bi].term.info;
    uint8_t chosen = 0;
    if (cert->valid != 1u || old->true_args != cert->original.true_args ||
        old->false_args != cert->original.false_args ||
        (old->true_arg_count &&
         (!cert->saved_true || !old->true_args ||
          memcmp(old->true_args, cert->saved_true,
                 (size_t)old->true_arg_count * sizeof(AstMirVReg)) != 0)) ||
        (old->false_arg_count &&
         (!cert->saved_false || !old->false_args ||
          memcmp(old->false_args, cert->saved_false,
                 (size_t)old->false_arg_count * sizeof(AstMirVReg)) != 0)) ||
        !proven_literal(fn, bi, old, &chosen) || chosen != cert->take_true ||
        now->kind != AST_MIR_TERM_BR ||
        now->condition_or_value != AST_MIR_INVALID_ID ||
        now->target_true != (chosen ? old->target_true : old->target_false) ||
        now->true_arg_count !=
            (chosen ? old->true_arg_count : old->false_arg_count) ||
        now->target_false != AST_MIR_INVALID_ID || now->false_arg_count != 0u ||
        now->memory != old->memory ||
        now->true_args !=
            (chosen ? cert->original.true_args : cert->original.false_args)) {
      mir_program_diag(
          program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
          fn->blocks[bi].span,
          "CFG branch certificate failed independent literal replay");
      return 0;
    }
  }
  return 1;
}

static int refresh(AstMirFunction *fn) {
  return mir_analyze_cfg(fn) && mir_link_memory_ssa(fn) &&
         mir_analyze_liveness(fn);
}

static void rollback(AstMirProgram *p) {
  for (size_t i = 0; i < p->function_count; ++i) {
    AstMirFunction *fn = p->functions[i];
    if (!fn->branch_certificates)
      continue;
    for (uint32_t b = 0; b < fn->block_count; ++b) {
      MirBranchCertificate *c = &fn->branch_certificates[b];
      if (c->valid)
        fn->blocks[b].term = c->original;
    }
    for (uint32_t b = 0; b < fn->block_count; ++b) {
      MirBranchCertificate *c = &fn->branch_certificates[b];
      if (c->valid) {
        if (c->saved_true)
          memcpy(c->original.true_args, c->saved_true,
                 (size_t)c->original.info.true_arg_count * sizeof(AstMirVReg));
        if (c->saved_false)
          memcpy(c->original.false_args, c->saved_false,
                 (size_t)c->original.info.false_arg_count * sizeof(AstMirVReg));
      }
      free(c->saved_true);
      free(c->saved_false);
    }
    free(fn->branch_certificates);
    fn->branch_certificates = NULL;
    (void)refresh(fn);
  }
}

int ast_mir_optimize_const_branches(AstMirProgram *p,
                                    AstMirOptimizeStats *stats) {
  AstMirOptimizeStats c = {0};
  if (!p || !ast_mir_verify_mode(p, AST_MIR_VERIFY_PRE_OPT))
    return 0;
  c.verifier_runs++;
  for (size_t fi = 0; fi < p->function_count; ++fi) {
    AstMirFunction *fn = p->functions[fi];
    if (fn->gvn_certificates || fn->branch_certificates)
      return 0;
  }
  int success = 1;
  for (size_t fi = 0; fi < p->function_count && success; ++fi) {
    AstMirFunction *fn = p->functions[fi];
    for (uint32_t b = 0; b < fn->block_count; ++b) {
      AstMirTerminator *t = &fn->blocks[b].term;
      if (t->info.kind != AST_MIR_TERM_COND_BR)
        continue;
      c.examined++;
      uint8_t take_true = 0;
      if (!proven_literal(fn, b, &t->info, &take_true))
        continue;
      if (!fn->branch_certificates) {
        fn->branch_certificates = (MirBranchCertificate *)calloc(
            fn->block_count, sizeof(*fn->branch_certificates));
        if (!fn->branch_certificates) {
          success = 0;
          break;
        }
      }
      MirBranchCertificate *cert = &fn->branch_certificates[b];
      cert->original = *t;
      if (t->info.true_arg_count) {
        if (!t->true_args) {
          success = 0;
          break;
        }
        cert->saved_true = (AstMirVReg *)mir_malloc_array(
            t->info.true_arg_count, sizeof(AstMirVReg));
        if (!cert->saved_true) {
          success = 0;
          break;
        }
        memcpy(cert->saved_true, t->true_args,
               (size_t)t->info.true_arg_count * sizeof(AstMirVReg));
      }
      if (t->info.false_arg_count) {
        if (!t->false_args) {
          success = 0;
          break;
        }
        cert->saved_false = (AstMirVReg *)mir_malloc_array(
            t->info.false_arg_count, sizeof(AstMirVReg));
        if (!cert->saved_false) {
          success = 0;
          break;
        }
        memcpy(cert->saved_false, t->false_args,
               (size_t)t->info.false_arg_count * sizeof(AstMirVReg));
      }
      cert->take_true = take_true;
      cert->valid = 1u;
      if (!take_true) {
        t->true_args = cert->original.false_args;
        t->true_arg_capacity = cert->original.false_arg_capacity;
        t->false_args = cert->original.true_args;
        t->false_arg_capacity = cert->original.true_arg_capacity;
      }
      t->info.kind = AST_MIR_TERM_BR;
      t->info.condition_or_value = AST_MIR_INVALID_ID;
      t->info.target_true = take_true ? cert->original.info.target_true
                                      : cert->original.info.target_false;
      t->info.target_false = AST_MIR_INVALID_ID;
      t->info.true_arg_count = take_true ? cert->original.info.true_arg_count
                                         : cert->original.info.false_arg_count;
      t->info.false_arg_count = 0u;
      t->info.true_args = t->true_args;
      t->info.false_args = t->false_args;
      c.rewritten++;
      c.branch_folded++;
    }
    if (fn->branch_certificates && !refresh(fn))
      success = 0;
  }
  if (success)
    success = ast_mir_verify_mode(p, AST_MIR_VERIFY_POST_OPT);
  if (!success) {
    rollback(p);
    (void)ast_mir_verify(p);
    return 0;
  }
  c.verifier_runs++;
  if (stats)
    *stats = c;
  return 1;
}
