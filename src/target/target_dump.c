#include "target_internal.h"

#include <stdarg.h>

typedef struct TargetDump {
  void (*write_fn)(void *user, const char *text, size_t length);
  void *user;
} TargetDump;

static void dump_out(TargetDump *d, const char *fmt, ...)
    ASTER_PRINTF_LIKE(2, 3);

static void dump_out(TargetDump *d, const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0)
    return;
  size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1u;
  d->write_fn(d->user, buf, len);
}

static void dump_name(TargetDump *d, AstName n) {
  if (n.text && n.span.length)
    d->write_fn(d->user, n.text, n.span.length);
  else
    dump_out(d, "<anon>");
}

void ast_target_dump_program(const AstTargetProgram *p,
                             void (*write_fn)(void *user, const char *text,
                                              size_t length),
                             void *user) {
  if (!p || !write_fn)
    return;
  TargetDump d = {write_fn, user};
  dump_out(&d, "target.program arch=%s os=%u ptr=%u features=0x%llx {\n",
           ast_target_arch_name(p->config.arch), (unsigned)p->config.os,
           p->config.pointer_bits, (unsigned long long)p->config.features);
  for (size_t fi = 0u; fi < p->function_count; ++fi) {
    const TargetFunction *f = &p->functions[fi];
    dump_out(&d, "  target.func @");
    dump_name(&d, f->info.module_name);
    dump_out(&d, ".");
    dump_name(&d, f->info.name);
    dump_out(
        &d,
        " [vregs=%u insts=%u legal=%u custom=%u split=%u unsupported=%u] {\n",
        f->info.vreg_count, f->info.instruction_count, f->info.legal_count,
        f->info.custom_count, f->info.split_count, f->info.unsupported_count);
    for (uint32_t vi = 0u; vi < f->info.vreg_count; ++vi) {
      const AstTargetValueInfo *v = &f->values[vi];
      dump_out(&d, "    %%r%u -> %s <%s> width=%u parts=%u", v->vreg,
               ast_target_legalize_action_name(v->action),
               ast_target_reg_class_name(v->reg_class), v->legal_bit_width,
               v->part_count);
      if (v->required_features)
        dump_out(&d, " requires=0x%llx",
                 (unsigned long long)v->required_features);
      dump_out(&d, "\n");
    }
    for (uint32_t ii = 0u; ii < f->info.instruction_count; ++ii) {
      const AstTargetInstructionInfo *in = &f->instructions[ii];
      dump_out(&d, "    i%u op=%u -> %s <%s>", in->id, (unsigned)in->op,
               ast_target_legalize_action_name(in->action),
               ast_target_reg_class_name(in->result_class));
      if (in->required_features)
        dump_out(&d, " requires=0x%llx",
                 (unsigned long long)in->required_features);
      if (in->native_effects_refined)
        dump_out(&d, " refined=0x%x", in->narrowed_effects);
      dump_out(&d, "\n");
    }
    dump_out(&d, "  }\n");
  }
  dump_out(&d, "}\n");
}
