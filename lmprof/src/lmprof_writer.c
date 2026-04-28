/*
** $Id: lmprof_writer.c $
** Native file writer helpers for lmprof reports.
** See Copyright Notice in lmprof_lib.h
*/
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <stdlib.h>
#endif

#include <dmsdk/dlib/log.h>

#include "lmprof_writer.h"

#if !defined(PATH_MAX)
#define PATH_MAX 4096
#endif

static int writer_ascii_equal_ci(char a, char b) {
  if (a >= 'A' && a <= 'Z')
    a = (char)(a + ('a' - 'A'));
  if (b >= 'A' && b <= 'Z')
    b = (char)(b + ('a' - 'A'));
  return a == b;
}

static int writer_path_ends_with(const char *path, const char *ext) {
  size_t i;
  const size_t path_len = path == NULL ? 0 : strlen(path);
  const size_t ext_len = strlen(ext);

  if (path_len < ext_len)
    return 0;

  path += path_len - ext_len;
  for (i = 0; i < ext_len; ++i) {
    if (!writer_ascii_equal_ci(path[i], ext[i]))
      return 0;
  }
  return 1;
}

static const char *writer_output_format(const char *path) {
  if (writer_path_ends_with(path, ".json"))
    return "JSON";
  if (writer_path_ends_with(path, ".tracy"))
    return "Tracy";
  if (writer_path_ends_with(path, ".perfetto-trace"))
    return "Perfetto";
  return "file";
}

static const char *writer_absolute_path(const char *path, char *buffer, size_t buffer_size) {
#if defined(_WIN32)
  if (_fullpath(buffer, path, buffer_size) != NULL)
    return buffer;
#else
  if (realpath(path, buffer) != NULL)
    return buffer;
#endif
  return path;
}

size_t lmprof_writer_default_output_paths(const char **paths, size_t path_count) {
  static const char *const default_output[] = {
    LMPROF_WRITER_DEFAULT_OUTPUT
  };
  const size_t output_count = sizeof(default_output) / sizeof(default_output[0]);
  size_t i;

  if (paths == NULL || path_count < output_count)
    return 0;

  for (i = 0; i < output_count; ++i)
    paths[i] = default_output[i];
  return output_count;
}

int lmprof_writer_write(const lmprof_WriterOutput *outputs, size_t output_count, lmprof_WriterCallback write, void *ctx) {
  size_t i;

  if (outputs == NULL || write == NULL || output_count == 0)
    return LMPROF_WRITER_FAILURE;

  for (i = 0; i < output_count; ++i) {
    int result;
    FILE *file;
    const char *mode;
    const lmprof_WriterOutput *output = &outputs[i];

    if (output->path == NULL)
      return LMPROF_WRITER_FAILURE;

    mode = output->binary ? "wb" : "w";
    file = fopen(output->path, mode);
    if (file == NULL) {
      dmLogError("lmprof: cannot open file '%s' (%s)", output->path, strerror(errno));
      return LMPROF_WRITER_FAILURE;
    }

    {
      char fullpath[PATH_MAX];
      const char *path = writer_absolute_path(output->path, fullpath, sizeof(fullpath));
      dmLogInfo("lmprof: Start writing %s profile: %s", writer_output_format(output->path), path);
      result = write(file, output->path, ctx);
      if (fclose(file) != 0)
        result = LMPROF_WRITER_FAILURE;
      if (result == LMPROF_WRITER_OK) {
        dmLogInfo("lmprof: Writing finished: %s", path);
      }
      else {
        dmLogError("lmprof: Writing failed: %s", path);
      }
    }
    if (result != LMPROF_WRITER_OK)
      return result;
  }

  return LMPROF_WRITER_OK;
}
