#ifndef ASTERASM_SOURCE_H
#define ASTERASM_SOURCE_H

#include "asterasm/common.h"
#include "asterasm/lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstMappedSource {
  AstSourceFile source;
  void *internal;
} AstMappedSource;

ASTER_API int ast_source_map_file(const char *path, AstMappedSource *out);
ASTER_API void ast_source_unmap(AstMappedSource *source);
ASTER_API char *ast_path_canonicalize(const char *path);
ASTER_API char *ast_path_module_candidate(const char *root,
                                          const char *module_name,
                                          const char *extension);
ASTER_API int ast_path_exists_regular(const char *path);

#ifdef __cplusplus
}
#endif

#endif
