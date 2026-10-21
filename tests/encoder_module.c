#include "asterasm/asterasm.h"
#include "test_process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ASTERASM_HAS_OBJECT_WRITER
#include "asterasm/object_writer.h"
#endif

/* Test transport only: compile a source fixture and emit named, relocated
   function bytes. The independent CPU emulator owns execution and expectations.
 */
typedef struct Module {
  AstMachineProgram *machine;
  size_t count;
  uint64_t *addresses;
} Module;
static int resolve(void *user, AstName name, uint64_t *address) {
  Module *m = user;
  for (size_t f = 0; f < m->count; ++f) {
    AstMachineFunctionInfo info;
    if (!ast_machine_function_info(m->machine, f, &info))
      return 0;
    if (info.name.span.length == name.span.length &&
        !memcmp(info.name.text, name.text, name.span.length)) {
      *address = m->addresses[f];
      return 1;
    }
  }
  return 0;
}
static void dump(void *user, const char *text, size_t length) {
  (void)fwrite(text, 1, length, user);
}
static int check_cast_contracts(AstRegAllocProgram *p, size_t functions) {
  for (size_t f = 0; f < functions; ++f)
    for (size_t b = 0; b < ast_physical_block_count(p, f); ++b) {
      AstPhysicalBlockInfo block;
      if (b > UINT32_MAX || !ast_physical_block(p, f, (uint32_t)b, &block))
        return 0;
      for (uint32_t n = 0; n < block.instruction_count; ++n) {
        /* The fixture owns mutable storage; mutate then restore through
           the public view to test the verifier independently of lowering. */
        AstPhysicalInstructionInfo *i =
            (AstPhysicalInstructionInfo *)&block.instructions[n];
        if (i->opcode < AST_MACHINE_X86_FMOV_BITS ||
            i->opcode > AST_MACHINE_X86_FP_TO_UINT64)
          continue;
        AstPhysicalInstructionInfo saved = *i;
        if (i->opcode == AST_MACHINE_X86_UINT64_TO_FP)
          i->operands[1] = i->operands[2];
        else if (i->opcode == AST_MACHINE_X86_FP_TO_UINT64)
          i->operands[2] = i->operands[3];
        else
          i->operands[1].bank = AST_MACHINE_BANK_VECTOR;
        int rejected = !ast_physical_verify(p);
        *i = saved;
        if (!rejected) {
          fputs("cast verifier accepted a corrupt register/scratch contract\n",
                stderr);
          return 0;
        }
      }
    }
  return ast_physical_verify(p);
}
#ifdef ASTERASM_HAS_OBJECT_WRITER
static int write_file(Module *m, AstEncoderOutput *functions, unsigned mode,
                      unsigned format, const char *path) {
  if (!m->count || m->count > UINT32_MAX)
    return 0;
  size_t size = (size_t)(m->addresses[m->count - 1] - m->addresses[0]) +
                functions[m->count - 1].size;
  size_t reloc_count = 0;
  for (size_t f = 0; f < m->count; ++f) {
    if (functions[f].fixup_count > SIZE_MAX - reloc_count)
      return 0;
    reloc_count += functions[f].fixup_count;
  }
  int ok = 0;
  AstObjectOutput output = {0};
  uint8_t *text = calloc(size ? size : 1u, 1);
  AstObjectSymbol *symbols = calloc(m->count, sizeof(*symbols));
  AstObjectRelocation *relocs =
      calloc(reloc_count ? reloc_count : 1u, sizeof(*relocs));
  if (!text || !symbols || !relocs)
    goto done;
  size_t r = 0;
  for (size_t f = 0; f < m->count; ++f) {
    size_t offset = (size_t)(m->addresses[f] - m->addresses[0]);
    memcpy(text + offset, functions[f].bytes, functions[f].size);
    AstMachineFunctionInfo info;
    (void)ast_machine_function_info(m->machine, f, &info);
    symbols[f].name = (AstObjectBytes){(const uint8_t *)info.name.text,
                                       info.name.span.length};
    symbols[f].value = offset;
    symbols[f].size = functions[f].size;
    symbols[f].kind = AST_OBJECT_SYMBOL_CODE;
    symbols[f].binding = AST_OBJECT_GLOBAL;
    for (size_t n = 0; n < functions[f].fixup_count; ++n) {
      const AstEncoderFixup *fixup = &functions[f].fixups[n];
      uint64_t address;
      if (!resolve(m, fixup->symbol, &address))
        goto done;
      size_t target = 0;
      while (target < m->count && m->addresses[target] != address)
        ++target;
      if (target == m->count)
        goto done;
      relocs[r].offset = offset + fixup->offset;
      relocs[r].symbol = (uint32_t)target;
      relocs[r].addend = fixup->addend;
      switch (fixup->kind) {
      case AST_ENCODER_FIXUP_PC_REL32:
        relocs[r].kind = AST_OBJECT_PCREL32;
        break;
      case AST_ENCODER_FIXUP_X86_CALL_REL32:
        relocs[r].kind = AST_OBJECT_X86_CALL32;
        break;
      case AST_ENCODER_FIXUP_ABS64:
        relocs[r].kind = AST_OBJECT_ABS64;
        break;
      case AST_ENCODER_FIXUP_A64_CALL26:
        relocs[r].kind = AST_OBJECT_A64_CALL26;
        break;
      case AST_ENCODER_FIXUP_A64_ADR21:
        relocs[r].kind = AST_OBJECT_A64_ADR21;
        break;
      case AST_ENCODER_FIXUP_A64_ADRP_ADD:
        relocs[r].kind = AST_OBJECT_A64_ADRP_ADD;
        break;
      case AST_ENCODER_FIXUP_RISCV_CALL:
        relocs[r].kind = AST_OBJECT_RV_CALL;
        break;
      case AST_ENCODER_FIXUP_RISCV_PCREL_ADDRESS:
        relocs[r].kind = AST_OBJECT_RV_ADDRESS;
        break;
      default:
        goto done;
      }
      ++r;
    }
  }
  AstObjectSection section = {{(const uint8_t *)".text", 5},
                              {text, size},
                              size,
                              16,
                              AST_OBJECT_TEXT,
                              0};
  AstObjectModule object = {0};
  object.abi_version = AST_OBJECT_ABI_VERSION;
  object.format = format;
  object.architecture = mode < 2    ? AST_OBJECT_X86_64
                        : mode == 2 ? AST_OBJECT_AARCH64
                                    : AST_OBJECT_RISCV64;
  object.sections = &section;
  object.section_count = 1;
  object.symbols = symbols;
  object.symbol_count = m->count;
  object.relocations = relocs;
  object.relocation_count = reloc_count;
  object.base_address = m->addresses[0];
  object.image_limit = size;
  object.entry_symbol = 0;
  object.riscv_float_abi = mode >= 3 ? mode - 3 : 0;
  if (ast_object_write(&object, &output) != AST_OBJECT_OK) {
    fprintf(stderr, "object writer: %s\n", output.message);
    goto done;
  }
  FILE *file = ast_fopen(path, "wb");
  if (!file)
    goto done;
  ok = fwrite(output.data, 1, (size_t)output.size, file) == output.size;
  if (fclose(file))
    ok = 0;
done:
  ast_object_output_free(&output);
  free(text);
  free(symbols);
  free(relocs);
  return ok;
}
#endif
int main(int argc, char **argv) {
  ast_test_process_setup();
  if (argc != 5 && argc != 7)
    return 2;
  unsigned mode = (unsigned)strtoul(argv[1], NULL, 10);
  if (mode > 5)
    return 2;
  int ok = 0;
  AstFrontend *frontend = NULL;
  AstSema *sema = NULL;
  AstHirProgram *hir = NULL;
  AstMirProgram *mir = NULL;
  AstTargetProgram *target = NULL;
  AstAbiProgram *abi = NULL;
  AstRegAllocProgram *allocation = NULL;
  Module m = {0};
  AstEncoderOutput *outputs = NULL;
  AstEncoderModuleOutput encoded_module = {0};
  char root[4096], path[4096];
  int root_len = snprintf(root, sizeof(root), "%s/tests/regalloc_cases/%s",
                          ASTERASM_SOURCE_DIR, argv[4]);
  if (root_len < 0 || (size_t)root_len >= sizeof(root))
    goto done;
  int path_len = snprintf(path, sizeof(path), "%s/main.aa", root);
  if (path_len < 0 || (size_t)path_len >= sizeof(path))
    goto done;
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 1;
  frontend = ast_frontend_create(&fc);
  const char *entries[] = {path};
  if (!frontend || ast_frontend_add_import_root(frontend, root) ||
      ast_frontend_load(frontend, entries, 1))
    goto done;
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 1;
  sema = ast_sema_create(frontend, &sc);
  if (!sema || ast_sema_check(sema)) {
    size_t count = 0;
    const AstSemaDiagnostic *diags = ast_sema_diagnostics(sema, &count);
    for (size_t d = 0; d < count; ++d)
      fprintf(stderr, "sema: %s\n", diags[d].message);
    goto done;
  }
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 1;
  hir = ast_hir_build(frontend, sema, &hc);
  if (!hir || !ast_hir_verify(hir))
    goto done;
  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 1;
  mc.promote_locals = (uint8_t)atoi(argv[3]);
  mir = ast_mir_build(frontend, hir, &mc);
  if (!mir || !ast_mir_verify(mir))
    goto done;
  AstTargetConfig tc =
      ast_target_config_default(mode < 2    ? AST_TARGET_ARCH_X86_64
                                : mode == 2 ? AST_TARGET_ARCH_AARCH64
                                            : AST_TARGET_ARCH_RISCV64);
  tc.os = mode == 1 ? AST_TARGET_OS_WINDOWS : AST_TARGET_OS_LINUX;
  if (mode >= 3)
    tc.features |= AST_TARGET_FEATURE_RV_M | AST_TARGET_FEATURE_RV_F |
                   AST_TARGET_FEATURE_RV_D;
  target = ast_target_legalize(mir, &tc);
  if (!target || !ast_target_verify(target))
    goto done;
  static const AstAbiKind abis[] = {AST_ABI_SYSV_AMD64,  AST_ABI_WINDOWS_X64,
                                    AST_ABI_AAPCS64,     AST_ABI_RISCV_LP64,
                                    AST_ABI_RISCV_LP64F, AST_ABI_RISCV_LP64D};
  AstAbiConfig ac = ast_abi_config_default(abis[mode]);
  abi = ast_abi_lower(target, &ac);
  if (!abi || !ast_abi_verify(abi))
    goto done;
  m.machine = ast_backend_lower(abi, NULL);
  if (!m.machine || !ast_machine_verify(m.machine))
    goto done;
  AstRegAllocConfig rc = ast_regalloc_config_default();
  rc.register_limit = (uint32_t)strtoul(argv[2], NULL, 10);
  allocation = ast_regalloc_build(m.machine, &rc);
  if (!allocation || !ast_regalloc_verify(allocation) ||
      !ast_regalloc_finalize(allocation))
    goto done;
  m.count = ast_machine_function_count(m.machine);
  if (mode < 2 && !check_cast_contracts(allocation, m.count))
    goto done;
  m.addresses = calloc(m.count, sizeof(*m.addresses));
  if (!m.addresses || !ast_encoder_module(allocation, &encoded_module)) {
    if (encoded_module.message)
      fprintf(stderr, "encoder function %zu: %s\n",
              encoded_module.failed_function, encoded_module.message);
    goto done;
  }
  outputs = encoded_module.functions;
  if (encoded_module.function_count != m.count)
    goto done;
  uint64_t next = UINT64_C(0x100000);
  for (size_t f = 0; f < m.count; ++f) {
    m.addresses[f] = next;
    next = (next + outputs[f].size + 15u) & ~UINT64_C(15);
  }
  if (argc == 7) {
#ifdef ASTERASM_HAS_OBJECT_WRITER
    ok = write_file(&m, outputs, mode, (unsigned)strtoul(argv[5], NULL, 10),
                    argv[6]);
#else
    fputs("Object writer was not enabled in this build\n", stderr);
#endif
    goto done;
  }
  for (size_t f = 0; f < m.count; ++f) {
    if (!ast_encoder_apply_fixups(&outputs[f], m.addresses[f], resolve, &m)) {
      fprintf(stderr, "%s\n", outputs[f].message);
      goto done;
    }
    AstMachineFunctionInfo info;
    (void)ast_machine_function_info(m.machine, f, &info);
    printf("%.*s\t%llu\t", (int)info.name.span.length, info.name.text,
           (unsigned long long)m.addresses[f]);
    for (size_t b = 0; b < outputs[f].size; ++b)
      printf("%02x", outputs[f].bytes[b]);
    putchar('\n');
  }
  ok = 1;
done:
  if (!ok && allocation)
    ast_regalloc_dump(allocation, dump, stderr);
  if (!ok)
    fputs("source-to-byte fixture failed\n", stderr);
  ast_encoder_module_output_destroy(&encoded_module);
  free(m.addresses);
  ast_regalloc_destroy(allocation);
  ast_machine_destroy(m.machine);
  ast_abi_destroy(abi);
  ast_target_destroy(target);
  ast_mir_destroy(mir);
  ast_hir_destroy(hir);
  ast_sema_destroy(sema);
  ast_frontend_destroy(frontend);
  return ok ? 0 : 1;
}
