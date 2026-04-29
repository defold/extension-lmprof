/*
** $Id: lmprof_writer.h $
** Native file writer helpers for lmprof reports.
** See Copyright Notice in lmprof_lib.h
*/
#ifndef lmprof_writer_h
#define lmprof_writer_h

#include <stddef.h>
#include <stdio.h>

#define LMPROF_WRITER_DEFAULT_OUTPUT "mem.perfetto-trace"
#define LMPROF_WRITER_JSON_OUTPUT "mem.json"
#define LMPROF_WRITER_TRACY_OUTPUT "mem.tracy"
#define LMPROF_WRITER_OK 0
#define LMPROF_WRITER_FAILURE -1

typedef struct lmprof_WriterOutput {
  const char *path;
  int binary;
} lmprof_WriterOutput;

typedef int (*lmprof_WriterCallback)(FILE *file, const char *path, void *ctx);

size_t lmprof_writer_default_output_paths(const char **paths, size_t path_count);
int lmprof_writer_write(const lmprof_WriterOutput *outputs, size_t output_count, lmprof_WriterCallback write, void *ctx);

#endif
