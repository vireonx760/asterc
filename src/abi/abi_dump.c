#include "abi_internal.h"

#include <stdarg.h>

#define ABI_DUMP_BUF 1024u

typedef struct AbiDump {
  void (*write_fn)(void *user, const char *text, size_t length);
  void *user;
} AbiDump;

static void dump_out(AbiDump *d, const char *fmt, ...) ASTER_PRINTF_LIKE(2, 3);

static void dump_out(AbiDump *d, const char *fmt, ...) {
  char buf[ABI_DUMP_BUF];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0)
    return;
  size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1u;
  d->write_fn(d->user, buf, len);
}

static void dump_name(AbiDump *d, AstName n) {
  if (n.text && n.span.length)
    d->write_fn(d->user, n.text, n.span.length);
  else
    dump_out(d, "<anon>");
}

static void dump_loc(AbiDump *d, const AstAbiLocation *l) {
  dump_out(d, "%s(", ast_abi_location_kind_name(l->kind));
  for (uint32_t i = 0u; i < l->register_count; ++i) {
    if (i)
      dump_out(d, ",");
    dump_out(d, "%s", l->registers[i]);
  }
  if (l->kind == AST_ABI_LOCATION_STACK ||
      l->kind == AST_ABI_LOCATION_SPLIT_REG_STACK ||
      (l->kind == AST_ABI_LOCATION_INDIRECT && l->register_count == 0u)) {
    dump_out(d, "%s+%llu", l->register_count ? "," : "",
             (unsigned long long)l->stack_offset);
  }
  dump_out(d, ";%lluB;a%u)", (unsigned long long)l->stack_size,
           l->stack_alignment);
}

void ast_abi_dump_program(const AstAbiProgram *p,
                          void (*write_fn)(void *user, const char *text,
                                           size_t length),
                          void *user) {
  if (!p || !write_fn)
    return;
  AbiDump d = {write_fn, user};
  dump_out(&d, "abi.program abi=%s {\n", ast_abi_kind_name(p->config.kind));
  for (size_t fi = 0u; fi < p->function_count; ++fi) {
    const AbiFunction *f = &p->functions[fi];
    dump_out(&d, "  abi.func @");
    dump_name(&d, f->info.module_name);
    dump_out(&d, ".");
    dump_name(&d, f->info.name);
    dump_out(&d,
             " args=%u hidden=%u stack=%llu align=%u shadow=%u redzone=%u {\n",
             f->info.argument_count, f->info.hidden_argument_count,
             (unsigned long long)f->info.outgoing_stack_bytes,
             f->info.stack_alignment, f->info.shadow_space_bytes,
             f->info.red_zone_bytes);
    for (uint32_t ai = 0u; ai < f->total_argument_count; ++ai) {
      const AstAbiValueInfo *v = &f->arguments[ai];
      dump_out(&d, "    arg%u src=%%r%u flags=0x%x ext=%u -> ", ai,
               v->source_vreg, v->flags, (unsigned)v->extension);
      dump_loc(&d, &v->location);
      dump_out(&d, "\n");
    }
    dump_out(&d, "    result flags=0x%x ext=%u -> ", f->info.result.flags,
             (unsigned)f->info.result.extension);
    dump_loc(&d, &f->info.result.location);
    dump_out(&d, "\n  }\n");
  }
  for (size_t ci = 0u; ci < p->call_count; ++ci) {
    const AbiCall *c = &p->calls[ci];
    dump_out(&d, "  call f%u:i%u direct=%u args=%u stack=%llu shadow=%u {\n",
             c->info.caller_function_index, c->info.instruction,
             (unsigned)c->info.is_direct, c->info.argument_count,
             (unsigned long long)c->info.outgoing_stack_bytes,
             c->info.shadow_space_bytes);
    for (uint32_t ai = 0u; ai < c->total_argument_count; ++ai) {
      dump_out(&d, "    arg%u src=%%r%u -> ", ai, c->arguments[ai].source_vreg);
      dump_loc(&d, &c->arguments[ai].location);
      dump_out(&d, "\n");
    }
    dump_out(&d, "  }\n");
  }
  dump_out(&d, "}\n");
}
