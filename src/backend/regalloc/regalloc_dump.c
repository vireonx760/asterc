#include "allocation_internal.h"

static void dumpf(void (*write_fn)(void *, const char *, size_t), void *user,
                  const char *fmt, ...) ASTER_PRINTF_LIKE(3, 4);
static void dumpf(void (*write_fn)(void *, const char *, size_t), void *user,
                  const char *fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  if (n > 0)
    write_fn(user, buffer,
             (size_t)n < sizeof(buffer) ? (size_t)n : sizeof(buffer) - 1u);
}
void ast_regalloc_dump(const AstRegAllocProgram *p,
                       void (*write_fn)(void *, const char *, size_t),
                       void *user) {
  if (!p || !write_fn)
    return;
  dumpf(write_fn, user, "regalloc functions=%zu physical=%u errors=%zu\n",
        p->function_count, (unsigned)p->finalized, p->stats.error_count);
  if (p->stats.error_count) {
    dumpf(write_fn, user, "error %u fn=%u instruction=%u: %s\n",
          (unsigned)p->diagnostic.code, p->diagnostic.function_index,
          p->diagnostic.instruction, p->diagnostic.message);
    return;
  }
  for (uint32_t f = 0; f < p->function_count; ++f) {
    const MachineFunction *mf = &p->machine->functions[f];
    const AllocationFunction *af = &p->functions[f];
    dumpf(write_fn, user, "fn %.*s frame=%llu redzone=%u\n",
          (int)mf->info.name.span.length, mf->info.name.text,
          (unsigned long long)af->frame.stack_adjustment,
          (unsigned)af->frame.uses_red_zone);
    for (uint32_t v = 0; v < mf->info.vreg_count; ++v) {
      const AstRegAllocLocation *loc = &af->locations[v];
      dumpf(write_fn, user, "  %%m%u: kind=%u reg=%s slot=%u\n", v,
            (unsigned)loc->kind, loc->register_name, loc->frame_object);
    }
    for (uint32_t b = 0; b < af->block_count; ++b) {
      dumpf(write_fn, user, "  bb%u:\n", b);
      for (uint32_t i = 0; i < af->blocks[b].count; ++i) {
        const AstPhysicalInstructionInfo *inst = &af->blocks[b].instructions[i];
        dumpf(write_fn, user, "    %s", ast_machine_opcode_name(inst->opcode));
        for (uint32_t o = 0; o < inst->operand_count; ++o) {
          const AstPhysicalOperand *op = &inst->operands[o];
          switch (op->kind) {
          case AST_PHYSICAL_OPERAND_REGISTER:
            dumpf(write_fn, user, " %s:%u", op->register_name, op->bit_width);
            break;
          case AST_PHYSICAL_OPERAND_MEMORY:
            dumpf(write_fn, user, " [%s%+lld]:%u", op->register_name,
                  (long long)op->displacement, op->bit_width);
            break;
          case AST_PHYSICAL_OPERAND_IMMEDIATE:
            dumpf(write_fn, user, " 0x%llx",
                  (unsigned long long)op->immediate_bits);
            break;
          case AST_PHYSICAL_OPERAND_BLOCK:
            dumpf(write_fn, user, " bb%u", op->block);
            break;
          case AST_PHYSICAL_OPERAND_SYMBOL:
            if (op->module.text && op->module.span.length)
              dumpf(write_fn, user, " @%.*s.%.*s", (int)op->module.span.length,
                    op->module.text, (int)op->symbol.span.length,
                    op->symbol.text);
            else
              dumpf(write_fn, user, " @%.*s", (int)op->symbol.span.length,
                    op->symbol.text);
            break;
          default:
            dumpf(write_fn, user, " invalid");
            break;
          }
        }
        dumpf(write_fn, user, "\n");
      }
    }
  }
}
