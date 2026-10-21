#include "../src/isa/aarch64_data_contract.h"
#include "../src/isa/aarch64_scalable.h"
#include "../src/isa/aarch64_scalar_contract.h"
#include "../src/isa/riscv64_branch_contract.h"
#include "../src/isa/riscv64_memory_contract.h"
#include "../src/isa/riscv64_scalar_contract.h"
#include "../src/isa/system_contract.h"
#include "asterasm/asterasm.h"
#include "encoder_internal.h"
#include "physical_internal.h"
#include "test_process.h"

typedef struct Fixture {
  AstFrontend *frontend;
  AstSema *sema;
  AstHirProgram *hir;
  AstMirProgram *mir;
  AstTargetProgram *target;
  AstAbiProgram *abi;
  AstMachineProgram *machine;
  AstRegAllocProgram *allocation;
} Fixture;
static unsigned checks, failures;
static void require(int ok, const char *message) {
  ++checks;
  if (!ok) {
    fprintf(stderr, "native processor: %s\n", message);
    ++failures;
  }
}
static void close_fixture(Fixture *f) {
  ast_regalloc_destroy(f->allocation);
  ast_machine_destroy(f->machine);
  ast_abi_destroy(f->abi);
  ast_target_destroy(f->target);
  ast_mir_destroy(f->mir);
  ast_hir_destroy(f->hir);
  ast_sema_destroy(f->sema);
  ast_frontend_destroy(f->frontend);
}
static int open_fixture(Fixture *f, AstTargetArch arch, int windows,
                        const char *directory, uint32_t padding) {
  char path[1024];
  memset(f, 0, sizeof(*f));
  (void)snprintf(path, sizeof(path), "%s/tests/regalloc_cases/%s/%s.aa",
                 ASTERASM_SOURCE_DIR, directory, ast_target_arch_name(arch));
  const char *entry = path;
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 1u;
  f->frontend = ast_frontend_create(&fc);
  if (!f->frontend || ast_frontend_load(f->frontend, &entry, 1u))
    return 0;
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 1u;
  f->sema = ast_sema_create(f->frontend, &sc);
  if (!f->sema || ast_sema_check(f->sema))
    return 0;
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 1u;
  f->hir = ast_hir_build(f->frontend, f->sema, &hc);
  if (!f->hir || !ast_hir_verify(f->hir))
    return 0;
  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 1u;
  f->mir = ast_mir_build(f->frontend, f->hir, &mc);
  if (!f->mir || !ast_mir_verify(f->mir))
    return 0;
  AstTargetConfig tc = ast_target_config_default(arch);
  tc.features |=
      arch == AST_TARGET_ARCH_X86_64 ? AST_TARGET_FEATURE_X86_XSAVE
      : arch == AST_TARGET_ARCH_RISCV64
          ? AST_TARGET_FEATURE_RV_ZICSR | AST_TARGET_FEATURE_RV_ZIFENCEI |
                AST_TARGET_FEATURE_RV_H | AST_TARGET_FEATURE_RV_SVINVAL
          : 0u;
  if (!strcmp(directory, "scalar"))
    tc.features |= AST_TARGET_FEATURE_RV_M | AST_TARGET_FEATURE_RV_ZBA |
                   AST_TARGET_FEATURE_RV_ZBB | AST_TARGET_FEATURE_RV_ZBC |
                   AST_TARGET_FEATURE_RV_ZBS;
  if (!strcmp(directory, "scalable"))
    tc.features |= AST_TARGET_FEATURE_A64_SVE2 | AST_TARGET_FEATURE_A64_SME;
  f->target = ast_target_legalize(f->mir, &tc);
  if (!f->target || !ast_target_verify(f->target))
    return 0;
  AstAbiKind kind = arch == AST_TARGET_ARCH_X86_64
                        ? (windows ? AST_ABI_WINDOWS_X64 : AST_ABI_SYSV_AMD64)
                    : arch == AST_TARGET_ARCH_AARCH64 ? AST_ABI_AAPCS64
                                                      : AST_ABI_RISCV_LP64;
  AstAbiConfig ac = ast_abi_config_default(kind);
  f->abi = ast_abi_lower(f->target, &ac);
  if (!f->abi || !ast_abi_verify(f->abi))
    return 0;
  f->machine = ast_backend_lower(f->abi, NULL);
  if (!f->machine)
    return 0;
  f->allocation = ast_regalloc_build(f->machine, NULL);
  if (f->allocation && padding)
    for (uint32_t fn = 0; fn < f->allocation->function_count; ++fn)
      if (frame_add_object(&f->allocation->functions[fn],
                           AST_FRAME_OBJECT_LOCAL, padding,
                           16u) == AST_MACHINE_INVALID_ID)
        return 0;
  return f->allocation && ast_regalloc_finalize(f->allocation) &&
         ast_physical_verify(f->allocation);
}
static int encode(AstTargetConfig target, const AstPhysicalInstructionInfo *i) {
  AstPhysicalBlockInfo block = {0u, 1u, i};
  AstEncoderOutput out;
  int ok = encoder_blocks(target, &block, 1u, 0u, &out);
  ast_encoder_output_destroy(&out);
  return ok;
}
static void mutations(Fixture *f) {
  AstRegAllocProgram *p = f->allocation;
  for (uint32_t fi = 0; fi < p->function_count; ++fi) {
    PhysicalBlock *block = &p->functions[fi].blocks[0];
    for (uint32_t j = 0; j < block->count; ++j) {
      AstPhysicalInstructionInfo *i = &block->instructions[j];
      if (i->opcode != system_opcode(p->target.arch))
        continue;
      AstPhysicalInstructionInfo saved = *i;
      uint32_t tag = i->operand_count - 1u;
      const SystemForm *form = system_physical_form(i);
      require(form != NULL && encode(p->target, i),
              "original single-instruction encoding");
      i->operands[tag].immediate_bits = SYSTEM_COUNT;
      require(!ast_physical_verify(p) && !encode(p->target, i),
              "out-of-range schema tag");
      *i = saved;
      i->operand_count = AST_PHYSICAL_MAX_OPERANDS + 1u;
      require(!ast_physical_verify(p) && !encode(p->target, i),
              "operand count overflow");
      *i = saved;
      i->operands[tag].bit_width = 64u;
      require(!ast_physical_verify(p) && !encode(p->target, i),
              "malformed schema tag");
      *i = saved;
      i->raw_bytes = (const uint8_t *)"x";
      i->raw_byte_count = 1u;
      require(!ast_physical_verify(p) && !encode(p->target, i),
              "opaque instruction payload");
      *i = saved;
      for (uint32_t k = 0; k < tag; ++k) {
        if (i->operands[k].kind == AST_PHYSICAL_OPERAND_REGISTER) {
          i->operands[k].register_unit ^= 1u;
          require(!ast_physical_verify(p) && !encode(p->target, i),
                  "mutated register identity");
          *i = saved;
          i->operands[k].bit_width = 8u;
          require(!ast_physical_verify(p) && !encode(p->target, i),
                  "mutated register width");
          *i = saved;
        } else {
          i->operands[k].immediate_bits ^= 1u;
          require(!ast_physical_verify(p),
                  "source/physical selector or immediate mismatch");
          *i = saved;
        }
      }
      if (form) {
        for (uint32_t other = 0; other < SYSTEM_COUNT; ++other) {
          if (&system_forms[other] == form ||
              system_forms[other].arch != form->arch ||
              system_forms[other].shape != form->shape)
            continue;
          i->operands[tag].immediate_bits = other;
          require(!ast_physical_verify(p),
                  "same-shape instruction substitution");
          *i = saved;
        }
        if (form->feature) {
          uint64_t features = p->target.features;
          p->target.features &= ~form->feature;
          require(!ast_physical_verify(p) && !encode(p->target, i),
                  "removed target feature");
          p->target.features = features;
        }
      }
      require(ast_physical_verify(p), "restored source proof");
    }
  }
}
static void binding_mutations(Fixture *f) {
  AstRegAllocProgram *p = f->allocation;
  for (uint32_t fn = 0; fn < p->function_count; ++fn) {
    PhysicalContext context = {0};
    context.program = p;
    context.function = fn;
    require(physical_verify_native(p, fn), "original managed bridge proof");
    for (uint32_t b = 0; b < p->functions[fn].block_count; ++b) {
      PhysicalBlock *block = &p->functions[fn].blocks[b];
      for (uint32_t k = 0; k < block->count; ++k) {
        AstPhysicalInstructionInfo *i = &block->instructions[k];
        if (i->source_instruction == AST_MACHINE_INVALID_ID)
          continue;
        AstMirOp source = p->machine->functions[fn]
                              .instructions[i->source_instruction]
                              .info.source_op;
        if (source != AST_MIR_OP_NATIVE && source != AST_MIR_OP_NATIVE_RESULT)
          continue;
        AstPhysicalInstructionInfo original = *i;
        for (uint32_t j = 0; j < i->operand_count; ++j) {
          AstPhysicalOperand *o = &i->operands[j];
          if (o->kind == AST_PHYSICAL_OPERAND_REGISTER &&
              o->register_unit != 320u && o->register_unit != 66u &&
              o->register_unit != 64u) {
            *o = physical_register(&context, o->bank,
                                   (o->register_unit % 64u) ^ 1u, o->bit_width);
          } else if (o->kind == AST_PHYSICAL_OPERAND_MEMORY)
            o->displacement += 8;
          else if (o->kind == AST_PHYSICAL_OPERAND_IMMEDIATE)
            o->immediate_bits ^= 1u;
          else
            continue;
          if (encode(p->target, i)) {
            int rejected = !ast_physical_verify(p);
            if (!rejected)
              fprintf(stderr,
                      "accepted mutation arch%u function%u instruction%u %s "
                      "operand%u\n",
                      (unsigned)p->target.arch, fn, k,
                      ast_machine_opcode_name(i->opcode), j);
            require(rejected,
                    "valid instruction with wrong bridge value/location");
          }
          *i = original;
        }
        require(ast_physical_verify(p), "restored managed bridge proof");
      }
    }
  }
}
static void scalar_mutations(Fixture *f) {
  AstRegAllocProgram *p = f->allocation;
  uint8_t covered[RV_SCALAR_COUNT] = {0}, branches[8] = {0};
  for (uint32_t fn = 0; fn < p->function_count; ++fn) {
    PhysicalContext context = {0};
    context.program = p;
    for (uint32_t b = 0; b < p->functions[fn].block_count; ++b) {
      PhysicalBlock *block = &p->functions[fn].blocks[b];
      for (uint32_t k = 0; k < block->count; ++k) {
        AstPhysicalInstructionInfo *i = &block->instructions[k];
        int branch = rv_native_branch_funct3(i->opcode);
        if (branch >= 0) {
          branches[branch] = 1u;
          AstPhysicalInstructionInfo original = *i;
          require(encode(p->target, i), "original branch encoding");
          for (uint32_t j = 0; j < 2u; ++j) {
            i->operands[j] = physical_register(
                &context, AST_MACHINE_BANK_GPR,
                (i->operands[j].register_unit % 64u) ^ 1u, 64u);
            require(encode(p->target, i) && !ast_physical_verify(p),
                    "valid branch with substituted comparison register");
            *i = original;
          }
          i->operands[2].immediate_bits += 4u;
          require(encode(p->target, i) && !ast_physical_verify(p),
                  "valid branch to substituted target");
          *i = original;
          i->opcode = branch == 0 ? AST_MACHINE_RISCV_NATIVE_BNE
                                  : AST_MACHINE_RISCV_NATIVE_BEQ;
          require(encode(p->target, i) && !ast_physical_verify(p),
                  "valid branch with substituted condition");
          *i = original;
          require(ast_physical_verify(p), "restored branch proof");
          continue;
        }
        const RvScalarForm *form = rv_scalar_form(i->opcode);
        if (!form || i->source_instruction == AST_MACHINE_INVALID_ID ||
            p->machine->functions[fn]
                    .instructions[i->source_instruction]
                    .info.source_op != AST_MIR_OP_NATIVE)
          continue;
        covered[form - rv_scalar_forms] = 1u;
        AstPhysicalInstructionInfo original = *i;
        require(encode(p->target, i), "original scalar encoding");
        for (uint32_t j = 0; j < i->operand_count; ++j) {
          AstPhysicalOperand *o = &i->operands[j];
          if (o->kind == AST_PHYSICAL_OPERAND_REGISTER) {
            *o = physical_register(&context, AST_MACHINE_BANK_GPR,
                                   (o->register_unit % 64u) ^ 1u, 64u);
            require(!ast_physical_verify(p), "source register substitution");
          } else {
            o->immediate_bits ^= 1u;
            require(!ast_physical_verify(p), "source immediate substitution");
          }
          *i = original;
        }
        i->opcode = i->opcode == AST_MACHINE_RISCV_ADD ? AST_MACHINE_RISCV_SUB
                                                       : AST_MACHINE_RISCV_ADD;
        require(!ast_physical_verify(p), "scalar opcode substitution");
        *i = original;
        i->raw_bytes = (const uint8_t *)"x";
        i->raw_byte_count = 1u;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "scalar opaque payload");
        *i = original;
        if (form->feature) {
          uint64_t features = p->target.features;
          p->target.features &= ~form->feature;
          require(!ast_physical_verify(p) && !encode(p->target, i),
                  "scalar feature removed");
          p->target.features = features;
        }
        if (form->shape != RV_SCALAR_R && form->shape != RV_SCALAR_N) {
          AstPhysicalOperand *immediate = &i->operands[i->operand_count - 1u];
          immediate->immediate_bits = form->shape == RV_SCALAR_I     ? 2048u
                                      : form->shape == RV_SCALAR_S32 ? 32u
                                      : form->shape == RV_SCALAR_S64
                                          ? 64u
                                          : UINT64_C(0x100000);
          require(!ast_physical_verify(p) && !encode(p->target, i),
                  "scalar immediate upper bound");
          *i = original;
        }
        require(ast_physical_verify(p), "restored scalar proof");
      }
    }
  }
  for (uint32_t j = 0; j < RV_SCALAR_COUNT; ++j)
    require(covered[j], "every scalar schema mutation-tested");
  for (uint32_t j = 0; j < 8u; ++j)
    if (j < 2u || j > 3u)
      require(branches[j], "every branch condition mutation-tested");
}
static void operand_mutations(Fixture *f) {
  AstRegAllocProgram *p = f->allocation;
  unsigned registers = 0u, immediates = 0u, addresses = 0u, symbols = 0u,
           aliases = 0u;
  for (uint32_t fn = 0; fn < p->function_count; ++fn) {
    PhysicalContext context = {0};
    context.program = p;
    context.function = fn;
    for (uint32_t b = 0; b < p->functions[fn].block_count; ++b) {
      PhysicalBlock *block = &p->functions[fn].blocks[b];
      for (uint32_t k = 0; k < block->count; ++k) {
        AstPhysicalInstructionInfo *i = &block->instructions[k];
        if (i->source_instruction == AST_MACHINE_INVALID_ID ||
            p->machine->functions[fn]
                    .instructions[i->source_instruction]
                    .info.source_op != AST_MIR_OP_NATIVE)
          continue;
        AstPhysicalInstructionInfo original = *i;
        for (uint32_t j = 0; j < i->operand_count; ++j) {
          AstPhysicalOperand *o = &i->operands[j];
          if (o->kind == AST_PHYSICAL_OPERAND_REGISTER) {
            *o = physical_register(&context, o->bank,
                                   (o->register_unit % 64u) ^ 1u, o->bit_width);
            if (encode(p->target, i)) {
              require(!physical_verify_native(p, fn),
                      "encodable source register substitution");
              ++registers;
            }
            *i = original;
            if (i->operands[j].bit_width == 8u &&
                !strcmp(i->operands[j].register_name, "ah")) {
              i->operands[j] = physical_named_register(&context, "al", 8u);
              require(encode(p->target, i) && !physical_verify_native(p, fn),
                      "high/low byte alias substitution");
              ++aliases;
              *i = original;
            }
          } else if (o->kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
            o->immediate_bits ^= 1u;
            if (encode(p->target, i)) {
              require(!physical_verify_native(p, fn),
                      "encodable source immediate substitution");
              ++immediates;
            }
            *i = original;
          } else if (o->kind == AST_PHYSICAL_OPERAND_SYMBOL) {
            o->symbol.text = "substituted";
            o->symbol.span.length = 11u;
            require(encode(p->target, i) && !physical_verify_native(p, fn),
                    "encodable symbol target substitution");
            ++symbols;
            *i = original;
            o->module.text = "different";
            o->module.span.length = 9u;
            require(encode(p->target, i) && !physical_verify_native(p, fn),
                    "encodable defining-module substitution");
            *i = original;
          } else if (o->kind == AST_PHYSICAL_OPERAND_MEMORY) {
            o->displacement += 1;
            require(encode(p->target, i) && !physical_verify_native(p, fn),
                    "encodable memory displacement substitution");
            ++addresses;
            *i = original;
            if (o->scale) {
              o->scale = o->scale == 2u ? 4u : 2u;
              require(encode(p->target, i) && !physical_verify_native(p, fn),
                      "encodable memory scale substitution");
              *i = original;
              AstPhysicalOperand index =
                  physical_register(&context, AST_MACHINE_BANK_GPR, 8u, 64u);
              o->index_register_unit = index.register_unit;
              memcpy(o->index_register_name, index.register_name,
                     sizeof(o->index_register_name));
              require(encode(p->target, i) && !physical_verify_native(p, fn),
                      "encodable memory index substitution");
              *i = original;
            }
          }
        }
        require(ast_physical_verify(p), "restored x86 source operand proof");
      }
    }
  }
  require(registers && immediates && addresses && symbols && aliases,
          "all x86 source operand classes mutation-tested");
}
static void memory_mutations(Fixture *f) {
  AstRegAllocProgram *p = f->allocation;
  uint8_t covered[11] = {0}, indirect[2] = {0};
  for (uint32_t fn = 0; fn < p->function_count; ++fn) {
    PhysicalContext context = {0};
    context.program = p;
    context.function = fn;
    for (uint32_t b = 0; b < p->functions[fn].block_count; ++b) {
      PhysicalBlock *block = &p->functions[fn].blocks[b];
      for (uint32_t k = 0; k < block->count; ++k) {
        AstPhysicalInstructionInfo *i = &block->instructions[k];
        if (i->opcode == AST_MACHINE_RISCV_NATIVE_JALR) {
          AstPhysicalInstructionInfo original = *i;
          indirect[i->operands[0].register_unit - 64u] = 1u;
          i->operands[0] = physical_register(
              &context, AST_MACHINE_BANK_GPR,
              i->operands[0].register_unit == 64u ? 1u : 0u, 64u);
          require(encode(p->target, i) && !ast_physical_verify(p),
                  "encodable indirect call/tail substitution");
          *i = original;
          i->operands[1] =
              physical_register(&context, AST_MACHINE_BANK_GPR, 9u, 64u);
          require(encode(p->target, i) && !ast_physical_verify(p),
                  "encodable indirect target register substitution");
          *i = original;
          i->operands[2].immediate_bits ^= 1u;
          require(encode(p->target, i) && !ast_physical_verify(p),
                  "encodable indirect offset substitution");
          *i = original;
          require(ast_physical_verify(p), "restored indirect source proof");
          continue;
        }
        const RvMemoryForm *form = rv_memory_form(i->opcode);
        if (!form || i->source_instruction == AST_MACHINE_INVALID_ID ||
            p->machine->functions[fn]
                    .instructions[i->source_instruction]
                    .info.source_op != AST_MIR_OP_NATIVE)
          continue;
        covered[form - rv_memory_forms] = 1u;
        AstPhysicalInstructionInfo original = *i;
        i->operands[1].displacement ^= 1;
        require(encode(p->target, i) && !ast_physical_verify(p),
                "encodable memory offset substitution");
        *i = original;
        AstPhysicalOperand base = physical_register(
            &context, AST_MACHINE_BANK_GPR,
            i->operands[1].register_unit == 95u ? 30u : 31u, 64u);
        i->operands[1].register_unit = base.register_unit;
        memcpy(i->operands[1].register_name, base.register_name,
               sizeof(base.register_name));
        require(encode(p->target, i) && !ast_physical_verify(p),
                "encodable memory base substitution");
        *i = original;
        i->operands[0] =
            physical_register(&context, AST_MACHINE_BANK_GPR,
                              (i->operands[0].register_unit % 64u) ^ 1u, 64u);
        require(encode(p->target, i) && !ast_physical_verify(p),
                "encodable memory data register substitution");
        *i = original;
        if (form->load) {
          i->opcode = i->opcode == AST_MACHINE_RISCV_LBU
                          ? AST_MACHINE_RISCV_LB
                          : AST_MACHINE_RISCV_LBU;
          i->operands[1].bit_width = 8u;
        } else {
          i->opcode = i->opcode == AST_MACHINE_RISCV_SB ? AST_MACHINE_RISCV_SH
                                                        : AST_MACHINE_RISCV_SB;
          i->operands[1].bit_width =
              i->opcode == AST_MACHINE_RISCV_SH ? 16u : 8u;
        }
        require(encode(p->target, i) && !ast_physical_verify(p),
                "encodable memory width/sign/opcode substitution");
        *i = original;
        i->operands[1].scale = 2u;
        i->operands[1].index_register_unit = 95u;
        memcpy(i->operands[1].index_register_name, "t6", 3u);
        require(!encode(p->target, i) && !ast_physical_verify(p),
                "RV memory cannot silently discard an index");
        *i = original;
        require(ast_physical_verify(p), "restored memory source proof");
      }
    }
  }
  for (uint32_t j = 0; j < 11u; ++j)
    require(covered[j], "every integer memory form mutation-tested");
  require(indirect[0] && indirect[1],
          "indirect call and tail forms mutation-tested");
}
static int emit_frame(const char *arch_name, const char *path,
                      uint32_t padding) {
  AstTargetArch arch = !strcmp(arch_name, "aarch64") ? AST_TARGET_ARCH_AARCH64
                                                     : AST_TARGET_ARCH_RISCV64;
  Fixture fixture;
  AstEncoderModuleOutput encoded = {0};
  int ok = open_fixture(&fixture, arch, 0, "native_bindings", padding);
  if (ok)
    ok = ast_encoder_module(fixture.allocation, &encoded) &&
         encoded.function_count && !encoded.functions[0].fixup_count;
  if (!ok)
    fprintf(stderr, "native frame encoding: %s function%llu\n",
            encoded.message ? encoded.message : "fixture failure",
            (unsigned long long)encoded.failed_function);
  if (ok) {
    FILE *file = ast_fopen(path, "wb");
    if (!file)
      ok = 0;
    else {
      ok = fwrite(encoded.functions[0].bytes, 1u, encoded.functions[0].size,
                  file) == encoded.functions[0].size;
      if (fclose(file))
        ok = 0;
    }
  }
  ast_encoder_module_output_destroy(&encoded);
  close_fixture(&fixture);
  return ok;
}
static void a64_scalar_mutations(Fixture *f) {
  AstRegAllocProgram *p = f->allocation;
  uint8_t covered[A64_SCALAR_COUNT] = {0};
  for (uint32_t fn = 0; fn < p->function_count; ++fn)
    for (uint32_t b = 0; b < p->functions[fn].block_count; ++b) {
      PhysicalBlock *block = &p->functions[fn].blocks[b];
      for (uint32_t k = 0; k < block->count; ++k) {
        AstPhysicalInstructionInfo *i = &block->instructions[k];
        const A64ScalarForm *form = a64_scalar_form(i);
        if (!form)
          continue;
        covered[form - a64_scalar_forms] = 1u;
        AstPhysicalInstructionInfo saved = *i;
        uint32_t tag = i->operand_count - 1u;
        require(encode(p->target, i), "A64 scalar original encoding");
        i->operands[tag].immediate_bits = A64_SCALAR_COUNT;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "A64 scalar schema bounds");
        *i = saved;
        i->operands[tag].bit_width = 64u;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "A64 scalar schema width");
        *i = saved;
        i->operand_count = AST_PHYSICAL_MAX_OPERANDS + 1u;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "A64 scalar operand overflow");
        *i = saved;
        i->result_count = 0u;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "A64 scalar result shape");
        *i = saved;
        i->raw_bytes = (const uint8_t *)"x";
        i->raw_byte_count = 1u;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "A64 scalar raw payload");
        *i = saved;
        for (uint32_t j = 0; j < tag; ++j) {
          AstPhysicalOperand *o = &i->operands[j];
          if (o->kind == AST_PHYSICAL_OPERAND_REGISTER) {
            o->register_unit ^= 1u;
            require(!ast_physical_verify(p) && !encode(p->target, i),
                    "A64 scalar register name/unit mismatch");
            *i = saved;
            o->bank = AST_MACHINE_BANK_GPR;
            o->register_unit = o->register_unit == 74u ? 75u : 74u;
            (void)snprintf(o->register_name, sizeof(o->register_name), "%c%u",
                           o->bit_width == 32u ? 'w' : 'x',
                           o->register_unit - 64u);
            require(encode(p->target, i) && !ast_physical_verify(p),
                    "A64 scalar encodable register substitution");
            *i = saved;
          } else {
            uint64_t v = o->immediate_bits;
            o->immediate_bits = form->shape == A64_SCALAR_WIDE && j == 2u
                                    ? (v + 16u) % i->operands[0].bit_width
                                    : v ^ 1u;
            require(encode(p->target, i) && !ast_physical_verify(p),
                    "A64 scalar encodable immediate substitution");
            *i = saved;
          }
        }
        for (uint32_t other = 0; other < A64_SCALAR_COUNT; ++other) {
          if (other == (uint32_t)(form - a64_scalar_forms) ||
              a64_scalar_forms[other].shape != form->shape)
            continue;
          i->operands[tag].immediate_bits = other;
          if (encode(p->target, i)) {
            require(!ast_physical_verify(p),
                    "A64 scalar encodable instruction substitution");
            *i = saved;
            break;
          }
          *i = saved;
        }
      }
    }
  for (uint32_t j = 0; j < A64_SCALAR_COUNT; ++j)
    require(covered[j], "A64 scalar catalog source coverage");
  require(ast_physical_verify(p), "A64 scalar fixture after restoration");
}
static void data_mutations(Fixture *f) {
  AstRegAllocProgram *p = f->allocation;
  uint8_t covered[A64_DATA_COUNT] = {0};
  for (uint32_t fn = 0; fn < p->function_count; ++fn)
    for (uint32_t b = 0; b < p->functions[fn].block_count; ++b) {
      PhysicalBlock *block = &p->functions[fn].blocks[b];
      for (uint32_t k = 0; k < block->count; ++k) {
        AstPhysicalInstructionInfo *i = &block->instructions[k];
        const A64DataForm *form = a64_data_form(i);
        if (!form)
          continue;
        covered[form - a64_data_forms] = 1u;
        AstPhysicalInstructionInfo saved = *i;
        uint32_t tag = i->operand_count - 1u;
        require(encode(p->target, i), "data original encoding");
        i->operands[tag].immediate_bits = A64_DATA_COUNT;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "data schema bounds");
        *i = saved;
        i->operands[tag].bit_width = 64u;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "data schema width");
        *i = saved;
        i->operand_count = AST_PHYSICAL_MAX_OPERANDS + 1u;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "data operand count");
        *i = saved;
        i->result_count ^= 1u;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "data result count");
        *i = saved;
        i->raw_bytes = (const uint8_t *)"x";
        i->raw_byte_count = 1u;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "data opaque payload");
        *i = saved;
        for (uint32_t j = 0; j < tag; ++j) {
          AstPhysicalOperand *o = &i->operands[j];
          if (o->kind == AST_PHYSICAL_OPERAND_REGISTER) {
            o->register_unit ^= 1u;
            require(!ast_physical_verify(p) && !encode(p->target, i),
                    "data register identity mismatch");
            *i = saved;
            o->bank = AST_MACHINE_BANK_GPR;
            o->register_unit = o->register_unit == 74u ? 75u : 74u;
            (void)snprintf(o->register_name, sizeof(o->register_name), "%c%u",
                           o->bit_width == 32u ? 'w' : 'x',
                           o->register_unit - 64u);
            require(encode(p->target, i) && !ast_physical_verify(p),
                    "data encodable register substitution");
            *i = saved;
          } else {
            o->immediate_bits = form->shape == A64_DATA_LOGIC && j == 2u
                                    ? a64_data_rotate(o->immediate_bits, 1u,
                                                      i->operands[0].bit_width)
                                    : o->immediate_bits ^ 1u;
            require(!ast_physical_verify(p), "data source immediate mismatch");
            *i = saved;
          }
        }
        for (uint32_t other = 0; other < A64_DATA_COUNT; ++other) {
          if (other == (uint32_t)(form - a64_data_forms) ||
              a64_data_forms[other].shape != form->shape)
            continue;
          i->operands[tag].immediate_bits = other;
          if (encode(p->target, i)) {
            require(!ast_physical_verify(p),
                    "data encodable opcode substitution");
            *i = saved;
            break;
          }
          *i = saved;
        }
      }
    }
  for (uint32_t j = 0; j < A64_DATA_COUNT; ++j)
    require(covered[j], "data source catalog coverage");
  require(ast_physical_verify(p), "data restored fixture");
}
static void scalable_mutations(Fixture *f) {
  AstRegAllocProgram *p = f->allocation;
  unsigned count = 0u;
  for (uint32_t fn = 0; fn < p->function_count; ++fn)
    for (uint32_t b = 0; b < p->functions[fn].block_count; ++b) {
      PhysicalBlock *block = &p->functions[fn].blocks[b];
      for (uint32_t k = 0; k < block->count; ++k) {
        AstPhysicalInstructionInfo *i = &block->instructions[k];
        const A64ScalableForm *form = a64_scalable_form(i);
        if (!form)
          continue;
        ++count;
        AstPhysicalInstructionInfo saved = *i;
        uint32_t tag = i->operand_count - 1u;
        require(encode(p->target, i), "scalable original encoding");
        i->operands[tag].immediate_bits = a64_scalable_form_count;
        require(!encode(p->target, i) && !ast_physical_verify(p),
                "scalable tag bounds");
        *i = saved;
        i->operands[tag].bit_width = 64u;
        require(!encode(p->target, i) && !ast_physical_verify(p),
                "scalable tag type");
        *i = saved;
        i->operand_count = AST_PHYSICAL_MAX_OPERANDS + 1u;
        require(!encode(p->target, i) && !ast_physical_verify(p),
                "scalable operand bounds");
        *i = saved;
        i->result_count ^= 1u;
        require(!encode(p->target, i) && !ast_physical_verify(p),
                "scalable result shape");
        *i = saved;
        i->raw_bytes = (const uint8_t *)"x";
        i->raw_byte_count = 1u;
        require(!encode(p->target, i) && !ast_physical_verify(p),
                "scalable opaque payload");
        *i = saved;
        for (uint32_t j = 0; j < tag; ++j) {
          const A64ScalableParameter *param =
              &a64_scalable_parameters[form->parameter_offset + j];
          const A64ScalableType *type = &a64_scalable_types[param->type];
          if (i->operands[j].kind == AST_PHYSICAL_OPERAND_REGISTER) {
            i->operands[j].register_unit ^= 1u;
            require(!encode(p->target, i) && !ast_physical_verify(p),
                    "scalable register alias identity");
            *i = saved;
            if (param->tied < 0 && type->last > type->first) {
              uint64_t index = 0u;
              (void)a64_scalable_operand(type, &i->operands[j], &index);
              (void)a64_scalable_register(
                  type, index == type->last ? type->first : type->last,
                  &i->operands[j]);
              if (a64_scalable_contract(i))
                require(encode(p->target, i) && !ast_physical_verify(p),
                        "scalable encodable source substitution");
              *i = saved;
            }
          } else {
            i->operands[j].immediate_bits ^= 1u;
            require(!ast_physical_verify(p),
                    "scalable source immediate substitution");
            *i = saved;
          }
        }
        uint64_t features = p->target.features;
        p->target.features = 0u;
        require(!encode(p->target, i) && !ast_physical_verify(p),
                "scalable removed feature");
        p->target.features = features;
      }
    }
  require(count >= 10u, "scalable source fixture families");
  require(ast_physical_verify(p), "scalable restored fixture");
}
static int emit_address(const char *offset, const char *base,
                        const char *path) {
  AstRegAllocProgram program = {0};
  PhysicalContext context = {0};
  program.target = ast_target_config_default(AST_TARGET_ARCH_AARCH64);
  context.program = &program;
  AstPhysicalInstructionInfo instructions[2] = {{0}};
  instructions[0].opcode = AST_MACHINE_A64_LEA;
  instructions[0].result_count = 1u;
  instructions[0].operand_count = 2u;
  instructions[0].operands[0] = physical_named_register(&context, "x0", 64u);
  instructions[0].operands[1] = physical_named_register(&context, base, 64u);
  instructions[0].operands[1].kind = AST_PHYSICAL_OPERAND_MEMORY;
  instructions[0].operands[1].displacement = strtoll(offset, NULL, 10);
  instructions[1].opcode = AST_MACHINE_A64_RET;
  AstPhysicalBlockInfo block = {0u, 2u, instructions};
  AstEncoderOutput encoded;
  int ok = encoder_blocks(program.target, &block, 1u, 0u, &encoded);
  if (ok) {
    FILE *file = ast_fopen(path, "wb");
    if (!file)
      ok = 0;
    else {
      ok = fwrite(encoded.bytes, 1u, encoded.size, file) == encoded.size;
      if (fclose(file))
        ok = 0;
    }
  }
  ast_encoder_output_destroy(&encoded);
  return ok;
}
int main(int argc, char **argv) {
  ast_test_process_setup();
  if (argc == 5 && !strcmp(argv[1], "--emit-address"))
    return emit_address(argv[2], argv[3], argv[4]) ? 0 : 1;
  if ((argc == 4 || argc == 5) && !strcmp(argv[1], "--emit-frame"))
    return emit_frame(argv[2], argv[3],
                      argc == 5 ? (uint32_t)strtoul(argv[4], NULL, 10) : 65536u)
               ? 0
               : 1;
  int bindings = argc == 2 && !strcmp(argv[1], "--bindings");
  int scalar = argc == 2 && !strcmp(argv[1], "--scalar");
  int operands = argc == 2 && !strcmp(argv[1], "--operands");
  int memory = argc == 2 && !strcmp(argv[1], "--memory");
  int a64_scalar = argc == 2 && !strcmp(argv[1], "--a64-scalar");
  int data = argc == 2 && !strcmp(argv[1], "--data");
  int scalable = argc == 2 && !strcmp(argv[1], "--scalable");
  for (unsigned arch = scalar || memory ? AST_TARGET_ARCH_RISCV64
                       : bindings       ? AST_TARGET_ARCH_AARCH64
                                        : AST_TARGET_ARCH_X86_64;
       arch <= AST_TARGET_ARCH_RISCV64; ++arch)
    for (int windows = 0; windows < (arch == AST_TARGET_ARCH_X86_64 ? 2 : 1);
         ++windows) {
      if (operands && arch != AST_TARGET_ARCH_X86_64)
        continue;
      if ((a64_scalar || data || scalable) && arch != AST_TARGET_ARCH_AARCH64)
        continue;
      Fixture f;
      int ok = open_fixture(&f, (AstTargetArch)arch, windows,
                            scalable     ? "scalable"
                            : data       ? "data_processing"
                            : a64_scalar ? "aarch64_scalar"
                            : memory     ? "operand_memory"
                            : operands   ? "operand_proof"
                            : scalar     ? "scalar"
                            : bindings   ? "native_bindings"
                                         : "processor",
                            0u);
      require(ok, "source-to-physical fixture");
      if (ok) {
        if (scalable)
          scalable_mutations(&f);
        else if (data)
          data_mutations(&f);
        else if (a64_scalar)
          a64_scalar_mutations(&f);
        else if (memory)
          memory_mutations(&f);
        else if (operands)
          operand_mutations(&f);
        else if (scalar)
          scalar_mutations(&f);
        else if (bindings)
          binding_mutations(&f);
        else
          mutations(&f);
      }
      close_fixture(&f);
      if (bindings) {
        ok = open_fixture(&f, (AstTargetArch)arch, windows, "native_bindings",
                          65536u);
        require(ok, "large-frame managed bridge fixture");
        if (ok)
          binding_mutations(&f);
        close_fixture(&f);
      }
    }
  printf("%u native CPU contract/source/encoder checks; %u failures\n", checks,
         failures);
  return failures ? 1 : 0;
}
