#include "../src/isa/vector_contract.h"
#include "asterasm/asterasm.h"
#include "encoder_internal.h"
#include "physical_internal.h"
#include "test_process.h"

typedef struct Fixture {
  AstFrontend *front;
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
    ++failures;
    fprintf(stderr, "native vector: %s\n", message);
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
  ast_frontend_destroy(f->front);
}
static int open_fixture(Fixture *f, AstTargetArch arch, int windows) {
  char path[1024];
  memset(f, 0, sizeof(*f));
  (void)snprintf(path, sizeof(path), "%s/tests/regalloc_cases/vector/%s.aa",
                 ASTERASM_SOURCE_DIR, ast_target_arch_name(arch));
  const char *entry = path;
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 1u;
  f->front = ast_frontend_create(&fc);
  if (!f->front || ast_frontend_load(f->front, &entry, 1u))
    return 0;
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 1u;
  f->sema = ast_sema_create(f->front, &sc);
  if (!f->sema || ast_sema_check(f->sema))
    return 0;
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 1u;
  f->hir = ast_hir_build(f->front, f->sema, &hc);
  if (!f->hir || !ast_hir_verify(f->hir))
    return 0;
  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 1u;
  f->mir = ast_mir_build(f->front, f->hir, &mc);
  if (!f->mir || !ast_mir_verify(f->mir))
    return 0;
  AstTargetConfig tc = ast_target_config_default(arch);
  tc.features |= arch == AST_TARGET_ARCH_X86_64
                     ? AST_TARGET_FEATURE_X86_AVX2 |
                           AST_TARGET_FEATURE_X86_SSE41 |
                           AST_TARGET_FEATURE_X86_AVX512F |
                           AST_TARGET_FEATURE_X86_AVX512BW |
                           AST_TARGET_FEATURE_X86_AVX512DQ |
                           AST_TARGET_FEATURE_X86_AVX512VL
                 : arch == AST_TARGET_ARCH_RISCV64 ? AST_TARGET_FEATURE_RV_V
                                                   : 0u;
  f->target = ast_target_legalize(f->mir, &tc);
  if (!f->target || !ast_target_verify(f->target))
    return 0;
  AstAbiConfig ac = ast_abi_config_default(
      arch == AST_TARGET_ARCH_X86_64
          ? (windows ? AST_ABI_WINDOWS_X64 : AST_ABI_SYSV_AMD64)
      : arch == AST_TARGET_ARCH_AARCH64 ? AST_ABI_AAPCS64
                                        : AST_ABI_RISCV_LP64);
  f->abi = ast_abi_lower(f->target, &ac);
  if (!f->abi || !ast_abi_verify(f->abi))
    return 0;
  f->machine = ast_backend_lower(f->abi, NULL);
  if (!f->machine)
    return 0;
  f->allocation = ast_regalloc_build(f->machine, NULL);
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
  for (uint32_t fn = 0; fn < p->function_count; ++fn)
    for (uint32_t b = 0; b < p->functions[fn].block_count; ++b) {
      PhysicalBlock *block = &p->functions[fn].blocks[b];
      for (uint32_t j = 0; j < block->count; ++j) {
        AstPhysicalInstructionInfo *i = &block->instructions[j], saved = *i;
        const VectorForm *form = vector_physical_form(i);
        if (!form) {
          if (p->target.arch == AST_TARGET_ARCH_X86_64 &&
              i->opcode == AST_MACHINE_X86_STORE &&
              i->operands[0].bit_width == 512u) {
            i->operands[0].bit_width = i->operands[1].bit_width = 128u;
            (void)machine_physreg_name(&p->target, AST_MACHINE_BANK_VECTOR,
                                       i->operands[0].register_unit - 128u,
                                       128u, i->operands[0].register_name);
            require(!ast_physical_verify(p),
                    "narrowed ZMM save must fail independent boundary proof");
            if (i->operands[0].register_unit < 144u)
              require(encode(p->target, i),
                      "narrowed low-register save remains encoding-valid");
            *i = saved;
          }
          continue;
        }
        require(ast_physical_verify(p) && encode(p->target, i),
                "original source and independent encoder proof");
        uint32_t tag = i->operand_count - 1u;
        i->result_count = AST_PHYSICAL_MAX_OPERANDS;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "malformed result count");
        *i = saved;
        i->operand_count = AST_PHYSICAL_MAX_OPERANDS + 1u;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "operand overflow");
        *i = saved;
        i->operands[tag].immediate_bits = VECTOR_COUNT;
        require(!ast_physical_verify(p) && !encode(p->target, i),
                "invalid schema tag");
        *i = saved;
        for (uint32_t k = 0; k < VECTOR_COUNT; ++k)
          if (vector_forms[k].arch == form->arch &&
              vector_forms[k].shape == form->shape &&
              vector_forms[k].encoding != form->encoding) {
            i->operands[tag].immediate_bits = k;
            require(!ast_physical_verify(p),
                    "encoding-valid semantic substitution must fail source "
                    "reconstruction");
            *i = saved;
            break;
          }
        for (uint32_t k = 0; k < tag; ++k) {
          if (i->operands[k].kind == AST_PHYSICAL_OPERAND_REGISTER) {
            i->operands[k].bit_width += 1u;
            require(!ast_physical_verify(p) && !encode(p->target, i),
                    "invalid register width");
            *i = saved;
            i->operands[k].register_unit ^= 1u;
            require(!ast_physical_verify(p) && !encode(p->target, i),
                    "register identity mismatch");
            *i = saved;
          } else if (i->operands[k].kind == AST_PHYSICAL_OPERAND_MEMORY) {
            i->operands[k].displacement += 64;
            require(!ast_physical_verify(p),
                    "valid memory form with changed source displacement");
            *i = saved;
          }
        }
        if (form->feature && form->feature != AST_TARGET_FEATURE_X86_SSE2) {
          uint64_t features = p->target.features;
          p->target.features = 0u;
          int source_ok = ast_physical_verify(p),
              encoder_ok = encode(p->target, i);
          if (source_ok || encoder_ok)
            fprintf(stderr, "feature probe %s %s: source=%d encoder=%d\n",
                    ast_target_arch_name(form->arch), form->name, source_ok,
                    encoder_ok);
          require(!source_ok && !encoder_ok, "missing instruction feature");
          p->target.features = features;
        }
      }
    }
}
int main(int argc, char **argv) {
  ast_test_process_setup();
  if (argc == 2 && !strcmp(argv[1], "--inventory")) {
    printf("%u\n", (unsigned)VECTOR_COUNT);
    for (uint32_t j = 0; j < VECTOR_COUNT; ++j)
      printf("%u\t%s\t%s\t%u\n", j, ast_target_arch_name(vector_forms[j].arch),
             vector_forms[j].name, (unsigned)vector_forms[j].shape);
    return 0;
  }
  static const AstTargetArch arches[] = {
      AST_TARGET_ARCH_X86_64, AST_TARGET_ARCH_X86_64, AST_TARGET_ARCH_AARCH64,
      AST_TARGET_ARCH_RISCV64};
  for (uint32_t a = 0; a < 4u; ++a) {
    Fixture f;
    int ok = open_fixture(&f, arches[a], a == 1u);
    require(ok, "fixture pipeline");
    if (ok)
      mutations(&f);
    close_fixture(&f);
  }
  printf("%u native SIMD source/physical/encoder checks; %u failures\n", checks,
         failures);
  return failures ? 1 : 0;
}
