#include "../core/parallel.h"
#include "asterasm/parser.h"

#include <stdlib.h>
#include <string.h>

typedef struct AstParseParallelJob {
  const AstLexedFile *lexed;
  AstParseResult *parsed;
  AstParseConfig config;
  uint32_t first_file_id;
} AstParseParallelJob;

static void ast_parse_parallel_worker(void *context, size_t index) {
  AstParseParallelJob *job = (AstParseParallelJob *)context;
  ast_parse_file(&job->lexed[index], job->first_file_id + (uint32_t)index,
                 &job->config, &job->parsed[index]);
}

int ast_parse_files_parallel(const AstLexedFile *lexed_files, size_t count,
                             uint32_t first_file_id,
                             const AstParseConfig *config, size_t thread_count,
                             AstParseBatch *out) {
  if (!out || (!lexed_files && count))
    return -1;
  memset(out, 0, sizeof(*out));
  if (!count)
    return 0;
  if (count > UINT32_MAX || first_file_id > UINT32_MAX - (uint32_t)(count - 1u))
    return -1;
  AstParseResult *parsed = (AstParseResult *)calloc(count, sizeof(*parsed));
  if (!parsed)
    return -1;
  AstParseParallelJob job;
  job.lexed = lexed_files;
  job.parsed = parsed;
  job.config = config ? *config : ast_parse_config_default();
  job.first_file_id = first_file_id;
  ast_parallel_for(count, thread_count, ast_parse_parallel_worker, &job);
  out->files = parsed;
  out->count = count;
  for (size_t i = 0; i < count; ++i) {
    if (!parsed[i].module && parsed[i].error_count == 0u) {
      ast_parse_batch_destroy(out);
      return -1;
    }
  }
  return 0;
}

void ast_parse_batch_destroy(AstParseBatch *batch) {
  if (!batch)
    return;
  if (batch->files) {
    for (size_t i = 0; i < batch->count; ++i)
      ast_parse_result_destroy(&batch->files[i]);
    free(batch->files);
  }
  memset(batch, 0, sizeof(*batch));
}
