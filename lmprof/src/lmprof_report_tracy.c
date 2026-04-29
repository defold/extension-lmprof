/*
** $Id: lmprof_report_tracy.c $
** Tracy native capture output.
** See Copyright Notice in lmprof_lib.h
*/
#define LUA_LIB

#include <stdlib.h>
#include <time.h>

#include "lmprof_report_internal.h"
#include "dlib/lz4.h"

/*
** {==================================================================
** Tracy native capture file output
** ===================================================================
**
** Tracy's .tracy format is the profiler worker's native save format. This
** writer emits the subset needed by lmprof traces: CPU zones, frames, thread
** names, and user plots. Unsupported sections are serialized as empty blocks.
*/

#define TRACY_FILE_BLOCK_SIZE 65536
#define TRACY_FILE_COMPRESS_BOUND (TRACY_FILE_BLOCK_SIZE + (TRACY_FILE_BLOCK_SIZE / 255) + 16)

#define TRACY_FILE_MAJOR 0
#define TRACY_FILE_MINOR 13
#define TRACY_FILE_PATCH 1

#define TRACY_STRINGREF_INACTIVE 0
#define TRACY_STRINGREF_IDX_ACTIVE 3
#define TRACY_PLOT_TYPE_USER 0
#define TRACY_PLOT_FORMAT_NUMBER 0

typedef struct TracyFileWriter {
  FILE *file;
  unsigned char buffer[TRACY_FILE_BLOCK_SIZE];
  size_t offset;
  int failed;
} TracyFileWriter;

typedef struct TracyString {
  char *value;
  size_t len;
  uint64_t ptr;
} TracyString;

typedef struct TracySourceLocation {
  char *name;
  char *file;
  uint32_t line;
  uint32_t name_idx;
  uint32_t file_idx;
  int16_t id;
  uint64_t count;
} TracySourceLocation;

typedef struct TracyZone {
  uint64_t tid;
  int16_t srcloc;
  int64_t start;
  int64_t end;
  size_t order;
  uint32_t extra;
  struct TracyZone **children;
  size_t child_count;
  size_t child_capacity;
} TracyZone;

typedef struct TracyThread {
  uint64_t tid;
  uint32_t name_idx;
  uint64_t name_ptr;
  TracyZone **roots;
  size_t root_count;
  size_t root_capacity;
  uint64_t zone_count;
} TracyThread;

typedef struct TracyFrameSet {
  char *name;
  uint32_t name_idx;
  uint64_t name_ptr;
  int64_t *starts;
  size_t count;
  size_t capacity;
} TracyFrameSet;

typedef struct TracyPlotPoint {
  int64_t time;
  double value;
} TracyPlotPoint;

typedef struct TracyPlot {
  char *name;
  uint32_t name_idx;
  uint64_t name_ptr;
  uint8_t format;
  TracyPlotPoint *points;
  size_t count;
  size_t capacity;
  double min;
  double max;
  double sum;
} TracyPlot;

typedef struct TracyExport {
  TracyString *strings;
  size_t string_count;
  size_t string_capacity;

  TracySourceLocation *sources;
  size_t source_count;
  size_t source_capacity;

  TracyThread *threads;
  size_t thread_count;
  size_t thread_capacity;

  TracyZone **zones;
  size_t zone_count;
  size_t zone_capacity;
  uint64_t child_vector_count;

  TracyFrameSet *frames;
  size_t frame_count;
  size_t frame_capacity;

  TracyPlot *plots;
  size_t plot_count;
  size_t plot_capacity;

  int64_t last_time;
} TracyExport;

static char *tracy_strdup_local(const char *str) {
  const char *value = CHROME_OPT_NAME(str, "");
  const size_t len = strlen(value);
  char *copy = l_pcast(char *, malloc(len + 1));
  if (copy == l_nullptr)
    return l_nullptr;
  memcpy(copy, value, len + 1);
  return copy;
}

static int tracy_reserve_array(void **data, size_t *capacity, size_t count, size_t item_size) {
  void *next;
  size_t next_capacity;
  if (count <= *capacity)
    return LUA_OK;

  next_capacity = (*capacity == 0) ? 16 : (*capacity * 2);
  while (next_capacity < count)
    next_capacity *= 2;

  next = realloc(*data, next_capacity * item_size);
  if (next == l_nullptr)
    return LMPROF_REPORT_FAILURE;

  *data = next;
  *capacity = next_capacity;
  return LUA_OK;
}

static int tracy_export_add_string(TracyExport *E, const char *value, uint32_t *idx, uint64_t *ptr) {
  size_t i;
  const char *str = CHROME_OPT_NAME(value, "");

  for (i = 0; i < E->string_count; ++i) {
    if (strcmp(E->strings[i].value, str) == 0) {
      if (idx != l_nullptr) *idx = l_cast(uint32_t, i);
      if (ptr != l_nullptr) *ptr = E->strings[i].ptr;
      return LUA_OK;
    }
  }

  if (tracy_reserve_array(l_pcast(void **, &E->strings), &E->string_capacity, E->string_count + 1, sizeof(TracyString)) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  E->strings[E->string_count].value = tracy_strdup_local(str);
  if (E->strings[E->string_count].value == l_nullptr)
    return LMPROF_REPORT_FAILURE;

  E->strings[E->string_count].len = strlen(str);
  E->strings[E->string_count].ptr = l_cast(uint64_t, E->string_count + 1);
  if (idx != l_nullptr) *idx = l_cast(uint32_t, E->string_count);
  if (ptr != l_nullptr) *ptr = E->strings[E->string_count].ptr;
  E->string_count++;
  return LUA_OK;
}

static TracyThread *tracy_export_get_thread(TracyExport *E, uint64_t tid) {
  size_t i;
  for (i = 0; i < E->thread_count; ++i) {
    if (E->threads[i].tid == tid)
      return &E->threads[i];
  }

  if (tracy_reserve_array(l_pcast(void **, &E->threads), &E->thread_capacity, E->thread_count + 1, sizeof(TracyThread)) != LUA_OK)
    return l_nullptr;

  memset(&E->threads[E->thread_count], 0, sizeof(TracyThread));
  E->threads[E->thread_count].tid = tid;
  return &E->threads[E->thread_count++];
}

static int tracy_export_set_thread_name(TracyExport *E, uint64_t tid, const char *name) {
  TracyThread *thread = tracy_export_get_thread(E, tid);
  if (thread == l_nullptr)
    return LMPROF_REPORT_FAILURE;
  return tracy_export_add_string(E, name, &thread->name_idx, &thread->name_ptr);
}

static TracySourceLocation *tracy_export_get_source(TracyExport *E, const char *name, const char *file, uint32_t line) {
  size_t i;
  const char *source_name = CHROME_OPT_NAME(name, LMPROF_RECORD_NAME_UNKNOWN);
  const char *source_file = CHROME_OPT_NAME(file, "");

  for (i = 0; i < E->source_count; ++i) {
    TracySourceLocation *source = &E->sources[i];
    if (source->line == line && strcmp(source->name, source_name) == 0 && strcmp(source->file, source_file) == 0)
      return source;
  }

  if (E->source_count >= 32767)
    return l_nullptr;
  if (tracy_reserve_array(l_pcast(void **, &E->sources), &E->source_capacity, E->source_count + 1, sizeof(TracySourceLocation)) != LUA_OK)
    return l_nullptr;

  memset(&E->sources[E->source_count], 0, sizeof(TracySourceLocation));
  E->sources[E->source_count].name = tracy_strdup_local(source_name);
  E->sources[E->source_count].file = tracy_strdup_local(source_file);
  if (E->sources[E->source_count].name == l_nullptr || E->sources[E->source_count].file == l_nullptr)
    return l_nullptr;
  if (tracy_export_add_string(E, source_name, &E->sources[E->source_count].name_idx, l_nullptr) != LUA_OK
      || tracy_export_add_string(E, source_file, &E->sources[E->source_count].file_idx, l_nullptr) != LUA_OK)
    return l_nullptr;

  E->sources[E->source_count].line = line;
  E->sources[E->source_count].id = l_cast(int16_t, -l_cast(int, E->source_count + 1));
  return &E->sources[E->source_count++];
}

static TracyZone *tracy_export_add_zone(TracyExport *E, uint64_t tid, const char *name, const char *file, uint32_t line, int64_t start, int64_t end) {
  TracySourceLocation *source;
  TracyZone *zone;
  TracyThread *thread;

  if (end < start)
    end = start;

  source = tracy_export_get_source(E, name, file, line);
  if (source == l_nullptr)
    return l_nullptr;

  thread = tracy_export_get_thread(E, tid);
  if (thread == l_nullptr)
    return l_nullptr;

  if (tracy_reserve_array(l_pcast(void **, &E->zones), &E->zone_capacity, E->zone_count + 1, sizeof(TracyZone *)) != LUA_OK)
    return l_nullptr;

  zone = l_pcast(TracyZone *, calloc(1, sizeof(TracyZone)));
  if (zone == l_nullptr)
    return l_nullptr;

  zone->srcloc = source->id;
  zone->tid = tid;
  zone->start = start;
  zone->end = end;
  zone->order = E->zone_count;
  zone->extra = 0;
  E->zones[E->zone_count++] = zone;
  source->count++;
  thread->zone_count++;
  if (E->last_time < end)
    E->last_time = end;
  return zone;
}

static int tracy_zone_add_child(TracyZone *parent, TracyZone *child) {
  if (tracy_reserve_array(l_pcast(void **, &parent->children), &parent->child_capacity, parent->child_count + 1, sizeof(TracyZone *)) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  parent->children[parent->child_count++] = child;
  return LUA_OK;
}

static int tracy_thread_add_root(TracyThread *thread, TracyZone *zone) {
  if (tracy_reserve_array(l_pcast(void **, &thread->roots), &thread->root_capacity, thread->root_count + 1, sizeof(TracyZone *)) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  thread->roots[thread->root_count++] = zone;
  return LUA_OK;
}

static TracyFrameSet *tracy_export_get_frame_set(TracyExport *E, const char *name) {
  size_t i;
  const char *frame_name = CHROME_OPT_NAME(name, "Frame");

  for (i = 0; i < E->frame_count; ++i) {
    if (strcmp(E->frames[i].name, frame_name) == 0)
      return &E->frames[i];
  }

  if (tracy_reserve_array(l_pcast(void **, &E->frames), &E->frame_capacity, E->frame_count + 1, sizeof(TracyFrameSet)) != LUA_OK)
    return l_nullptr;

  memset(&E->frames[E->frame_count], 0, sizeof(TracyFrameSet));
  E->frames[E->frame_count].name = tracy_strdup_local(frame_name);
  if (E->frames[E->frame_count].name == l_nullptr)
    return l_nullptr;
  if (tracy_export_add_string(E, frame_name, &E->frames[E->frame_count].name_idx, &E->frames[E->frame_count].name_ptr) != LUA_OK) {
    free(E->frames[E->frame_count].name);
    E->frames[E->frame_count].name = l_nullptr;
    return l_nullptr;
  }

  return &E->frames[E->frame_count++];
}

static int tracy_frame_add(TracyExport *E, const char *name, int64_t time) {
  TracyFrameSet *frames = tracy_export_get_frame_set(E, name);
  if (frames == l_nullptr)
    return LMPROF_REPORT_FAILURE;
  if (tracy_reserve_array(l_pcast(void **, &frames->starts), &frames->capacity, frames->count + 1, sizeof(int64_t)) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  frames->starts[frames->count++] = time;
  if (E->last_time < time)
    E->last_time = time;
  return LUA_OK;
}

static TracyPlot *tracy_export_get_plot(TracyExport *E, const char *name, uint8_t format) {
  size_t i;
  const char *plot_name = CHROME_OPT_NAME(name, "Plot");
  for (i = 0; i < E->plot_count; ++i) {
    if (strcmp(E->plots[i].name, plot_name) == 0)
      return &E->plots[i];
  }

  if (tracy_reserve_array(l_pcast(void **, &E->plots), &E->plot_capacity, E->plot_count + 1, sizeof(TracyPlot)) != LUA_OK)
    return l_nullptr;

  memset(&E->plots[E->plot_count], 0, sizeof(TracyPlot));
  E->plots[E->plot_count].name = tracy_strdup_local(plot_name);
  if (E->plots[E->plot_count].name == l_nullptr)
    return l_nullptr;
  if (tracy_export_add_string(E, plot_name, &E->plots[E->plot_count].name_idx, &E->plots[E->plot_count].name_ptr) != LUA_OK)
    return l_nullptr;
  E->plots[E->plot_count].format = format;
  return &E->plots[E->plot_count++];
}

static int tracy_plot_add_point(TracyExport *E, const char *name, uint8_t format, int64_t time, double value) {
  TracyPlot *plot = tracy_export_get_plot(E, name, format);
  if (plot == l_nullptr)
    return LMPROF_REPORT_FAILURE;
  if (tracy_reserve_array(l_pcast(void **, &plot->points), &plot->capacity, plot->count + 1, sizeof(TracyPlotPoint)) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  plot->points[plot->count].time = time;
  plot->points[plot->count].value = value;
  if (plot->count == 0) {
    plot->min = value;
    plot->max = value;
    plot->sum = value;
  }
  else {
    if (plot->min > value) plot->min = value;
    if (plot->max < value) plot->max = value;
    plot->sum += value;
  }
  plot->count++;
  if (E->last_time < time)
    E->last_time = time;
  return LUA_OK;
}

static int tracy_zone_compare(const void *lhs, const void *rhs) {
  const TracyZone *a = *l_pcast(TracyZone * const *, lhs);
  const TracyZone *b = *l_pcast(TracyZone * const *, rhs);
  if (a->start < b->start) return -1;
  if (a->start > b->start) return 1;
  if (a->end > b->end) return -1;
  if (a->end < b->end) return 1;
  if (a->order < b->order) return -1;
  if (a->order > b->order) return 1;
  return 0;
}

static int tracy_build_thread_trees(TracyExport *E) {
  size_t t;
  for (t = 0; t < E->thread_count; ++t) {
    TracyThread *thread = &E->threads[t];
    TracyZone **thread_zones = l_nullptr;
    TracyZone **stack = l_nullptr;
    size_t count = 0, capacity = 0, stack_count = 0, stack_capacity = 0;
    size_t i;

    for (i = 0; i < E->zone_count; ++i) {
      TracyZone *zone = E->zones[i];
      if (zone->tid != thread->tid)
        continue;
      if (tracy_reserve_array(l_pcast(void **, &thread_zones), &capacity, count + 1, sizeof(TracyZone *)) != LUA_OK) {
        free(thread_zones);
        free(stack);
        return LMPROF_REPORT_FAILURE;
      }
      thread_zones[count++] = zone;
    }

    if (count > 1)
      qsort(thread_zones, count, sizeof(TracyZone *), tracy_zone_compare);

    for (i = 0; i < count; ++i) {
      TracyZone *zone = thread_zones[i];
      while (stack_count > 0 && stack[stack_count - 1]->end <= zone->start)
        stack_count--;

      if (stack_count > 0 && zone->end <= stack[stack_count - 1]->end) {
        TracyZone *parent = stack[stack_count - 1];
        if (parent->child_count == 0)
          E->child_vector_count++;
        if (tracy_zone_add_child(parent, zone) != LUA_OK) {
          free(thread_zones);
          free(stack);
          return LMPROF_REPORT_FAILURE;
        }
      }
      else if (tracy_thread_add_root(thread, zone) != LUA_OK) {
        free(thread_zones);
        free(stack);
        return LMPROF_REPORT_FAILURE;
      }

      if (tracy_reserve_array(l_pcast(void **, &stack), &stack_capacity, stack_count + 1, sizeof(TracyZone *)) != LUA_OK) {
        free(thread_zones);
        free(stack);
        return LMPROF_REPORT_FAILURE;
      }
      stack[stack_count++] = zone;
    }
    free(thread_zones);
    free(stack);
  }
  return LUA_OK;
}

static size_t tracy_lz4_literals(unsigned char *dst, const unsigned char *src, size_t len) {
  size_t pos = 0;
  size_t lit_len = len;

  if (lit_len < 15) {
    dst[pos++] = l_cast(unsigned char, lit_len << 4);
  }
  else {
    dst[pos++] = 0xF0;
    lit_len -= 15;
    while (lit_len >= 255) {
      dst[pos++] = 255;
      lit_len -= 255;
    }
    dst[pos++] = l_cast(unsigned char, lit_len);
  }

  if (len != 0) {
    memcpy(dst + pos, src, len);
    pos += len;
  }
  return pos;
}

static size_t tracy_lz4_compress_block(unsigned char *dst, const unsigned char *src, size_t len) {
  int compressed_size = 0;
  if (len <= UINT32_MAX
      && dmLZ4::CompressBuffer(src, l_cast(uint32_t, len), dst, &compressed_size) == dmLZ4::RESULT_OK
      && compressed_size > 0
      && l_cast(size_t, compressed_size) <= TRACY_FILE_COMPRESS_BOUND) {
    return l_cast(size_t, compressed_size);
  }
  return tracy_lz4_literals(dst, src, len);
}

static int tracy_file_flush(TracyFileWriter *W) {
  unsigned char compressed[TRACY_FILE_COMPRESS_BOUND];
  uint32_t size;
  if (W->failed || W->offset == 0)
    return W->failed ? LMPROF_REPORT_FAILURE : LUA_OK;

  size = l_cast(uint32_t, tracy_lz4_compress_block(compressed, W->buffer, W->offset));
  if (fwrite(&size, 1, sizeof(size), W->file) != sizeof(size)
      || fwrite(compressed, 1, size, W->file) != size) {
    W->failed = 1;
    return LMPROF_REPORT_FAILURE;
  }

  W->offset = 0;
  return LUA_OK;
}

static int tracy_file_open(TracyFileWriter *W, FILE *file) {
  static const unsigned char header[4] = { 't', 'r', 253, 'P' };
  unsigned char compression = 0; /* Tracy FileCompression::Fast / LZ4 stream */
  unsigned char streams = 1;
  memset(W, 0, sizeof(*W));
  W->file = file;
  if (fwrite(header, 1, sizeof(header), file) != sizeof(header)
      || fwrite(&compression, 1, sizeof(compression), file) != sizeof(compression)
      || fwrite(&streams, 1, sizeof(streams), file) != sizeof(streams)) {
    W->failed = 1;
    return LMPROF_REPORT_FAILURE;
  }
  return LUA_OK;
}

static int tracy_file_write(TracyFileWriter *W, const void *data, size_t size) {
  const unsigned char *src = l_pcast(const unsigned char *, data);
  while (size != 0) {
    const size_t available = TRACY_FILE_BLOCK_SIZE - W->offset;
    const size_t chunk = size < available ? size : available;
    memcpy(W->buffer + W->offset, src, chunk);
    W->offset += chunk;
    src += chunk;
    size -= chunk;
    if (W->offset == TRACY_FILE_BLOCK_SIZE && tracy_file_flush(W) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
  }
  return LUA_OK;
}

static int tracy_write_u8(TracyFileWriter *W, uint8_t value) {
  return tracy_file_write(W, &value, sizeof(value));
}

static int tracy_write_i16(TracyFileWriter *W, int16_t value) {
  return tracy_file_write(W, &value, sizeof(value));
}

static int tracy_write_u32(TracyFileWriter *W, uint32_t value) {
  return tracy_file_write(W, &value, sizeof(value));
}

static int tracy_write_i32(TracyFileWriter *W, int32_t value) {
  return tracy_file_write(W, &value, sizeof(value));
}

static int tracy_write_u64(TracyFileWriter *W, uint64_t value) {
  return tracy_file_write(W, &value, sizeof(value));
}

static int tracy_write_i64(TracyFileWriter *W, int64_t value) {
  return tracy_file_write(W, &value, sizeof(value));
}

static int tracy_write_double(TracyFileWriter *W, double value) {
  return tracy_file_write(W, &value, sizeof(value));
}

static int tracy_write_string_ref_idx(TracyFileWriter *W, uint32_t idx) {
  const uint64_t value = idx;
  const uint8_t flags = TRACY_STRINGREF_IDX_ACTIVE;
  return tracy_write_u64(W, value) == LUA_OK && tracy_write_u8(W, flags) == LUA_OK ? LUA_OK : LMPROF_REPORT_FAILURE;
}

static int tracy_write_string_ref_inactive(TracyFileWriter *W) {
  const uint64_t value = 0;
  const uint8_t flags = TRACY_STRINGREF_INACTIVE;
  return tracy_write_u64(W, value) == LUA_OK && tracy_write_u8(W, flags) == LUA_OK ? LUA_OK : LMPROF_REPORT_FAILURE;
}

static int tracy_write_string_idx_inactive(TracyFileWriter *W) {
  const unsigned char bytes[3] = { 0, 0, 0 };
  return tracy_file_write(W, bytes, sizeof(bytes));
}

static int tracy_write_time_offset(TracyFileWriter *W, int64_t *ref, int64_t time) {
  const int64_t offset = time - *ref;
  *ref += offset;
  return tracy_write_i64(W, offset);
}

static int tracy_write_source_location(TracyFileWriter *W, const TracySourceLocation *source) {
  return tracy_write_string_ref_inactive(W) == LUA_OK
      && tracy_write_string_ref_idx(W, source->name_idx) == LUA_OK
      && tracy_write_string_ref_idx(W, source->file_idx) == LUA_OK
      && tracy_write_u32(W, source->line) == LUA_OK
      && tracy_write_u32(W, 0) == LUA_OK
      ? LUA_OK : LMPROF_REPORT_FAILURE;
}

static int tracy_write_zone_list(TracyFileWriter *W, TracyZone **zones, size_t count, int64_t *ref_time) {
  size_t i;
  if (count > UINT32_MAX)
    return LMPROF_REPORT_FAILURE;
  if (tracy_write_u32(W, l_cast(uint32_t, count)) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  for (i = 0; i < count; ++i) {
    TracyZone *zone = zones[i];
    if (tracy_write_i16(W, zone->srcloc) != LUA_OK
        || tracy_write_time_offset(W, ref_time, zone->start) != LUA_OK
        || tracy_write_u32(W, zone->extra) != LUA_OK)
      return LMPROF_REPORT_FAILURE;

    if (zone->child_count == 0) {
      if (tracy_write_u32(W, 0) != LUA_OK)
        return LMPROF_REPORT_FAILURE;
    }
    else if (tracy_write_zone_list(W, zone->children, zone->child_count, ref_time) != LUA_OK) {
      return LMPROF_REPORT_FAILURE;
    }

    if (tracy_write_time_offset(W, ref_time, zone->end) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
  }
  return LUA_OK;
}

static int tracy_write_zero_zone_extra(TracyFileWriter *W) {
  return tracy_write_string_idx_inactive(W) == LUA_OK
      && tracy_write_string_idx_inactive(W) == LUA_OK
      && tracy_write_string_idx_inactive(W) == LUA_OK
      && tracy_file_write(W, "\0\0\0", 3) == LUA_OK
      ? LUA_OK : LMPROF_REPORT_FAILURE;
}

static uint64_t tracy_pseudo_tid(lua_Integer pid, lua_Integer tid) {
  return (l_cast(uint64_t, l_cast(uint32_t, pid)) << 32) | l_cast(uint64_t, l_cast(uint32_t, tid));
}

static uint64_t tracy_event_tid(const TraceEvent *event) {
  return tracy_pseudo_tid(event->call.proc.pid, event->call.proc.tid);
}

static uint64_t tracy_main_tid(const lmprof_State *st) {
  return tracy_pseudo_tid(st->thread.mainproc.pid, st->thread.mainproc.tid);
}

static int64_t tracy_rebase_time_ns(uint64_t time_ns, uint64_t base_ns) {
  return l_cast(int64_t, time_ns > base_ns ? time_ns - base_ns : 0);
}

static int tracy_export_set_chrome_thread_name(TracyExport *E, lua_Integer pid, lua_Integer tid, const char *name) {
  char buffer[128];
  const char *thread_name = CHROME_OPT_NAME(name, CHROME_META_TICK);
  const uint64_t pseudo_tid = tracy_pseudo_tid(pid, tid);
  const int len = snprintf(buffer, sizeof(buffer), "(PID %" PRIu64 " TID %" PRIu64 ") %s",
      l_cast(uint64_t, l_cast(uint32_t, pid)),
      l_cast(uint64_t, l_cast(uint32_t, tid)),
      thread_name);
  if (len < 0)
    return LMPROF_REPORT_FAILURE;
  buffer[sizeof(buffer) - 1] = '\0';
  return tracy_export_set_thread_name(E, pseudo_tid, buffer);
}

static int tracy_write_capture(TracyFileWriter *W, TracyExport *E, lmprof_State *st, const char *file) {
  static const unsigned char file_header[8] = { 't', 'r', 'a', 'c', 'y', TRACY_FILE_MAJOR, TRACY_FILE_MINOR, TRACY_FILE_PATCH };
  const char *capture_name = CHROME_OPT_NAME(file, "lmprof.tracy");
  const char *program_name = CHROME_OPT_NAME(st->i.name, LMPROF);
  const char cpu_manufacturer[12] = { 0 };
  const uint64_t max64 = ~(uint64_t)0;
  size_t i;

  if (E->frame_count == 0) {
    if (tracy_frame_add(E, "Frame", 0) != LUA_OK
        || tracy_frame_add(E, "Frame", 0) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
  }

  for (i = 0; i < E->thread_count; ++i) {
    TracyThread *thread = &E->threads[i];
    if (thread->name_ptr == 0) {
      const lua_Integer pid = l_cast(lua_Integer, thread->tid >> 32);
      const lua_Integer tid = l_cast(lua_Integer, thread->tid & 0xffffffffu);
      const char *name = thread->tid == tracy_main_tid(st) ? CHROME_NAME_CR_RENDERER : CHROME_META_TICK;
      if (tracy_export_set_chrome_thread_name(E, pid, tid, name) != LUA_OK)
        return LMPROF_REPORT_FAILURE;
    }
  }
  if (tracy_build_thread_trees(E) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  if (tracy_file_write(W, file_header, sizeof(file_header)) != LUA_OK
      || tracy_write_u64(W, 1) != LUA_OK
      || tracy_write_double(W, 1.0) != LUA_OK
      || tracy_write_i64(W, E->last_time) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, l_cast(uint64_t, st->thread.mainproc.pid)) != LUA_OK
      || tracy_write_i64(W, 0) != LUA_OK
      || tracy_write_u8(W, 0) != LUA_OK
      || tracy_write_u32(W, 0) != LUA_OK
      || tracy_file_write(W, cpu_manufacturer, sizeof(cpu_manufacturer)) != LUA_OK
      || tracy_write_u8(W, 0) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  if (tracy_write_u64(W, strlen(capture_name)) != LUA_OK
      || tracy_file_write(W, capture_name, strlen(capture_name)) != LUA_OK
      || tracy_write_u64(W, strlen(program_name)) != LUA_OK
      || tracy_file_write(W, program_name, strlen(program_name)) != LUA_OK
      || tracy_write_u64(W, l_cast(uint64_t, time(l_nullptr))) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  /* CPU topology and crash event. */
  if (tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_i64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u32(W, 0) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  /* Frame sets. */
  if (tracy_write_u64(W, E->frame_count) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  for (i = 0; i < E->frame_count; ++i) {
    TracyFrameSet *frames = &E->frames[i];
    int64_t ref = 0;
    size_t j;
    if (tracy_write_u64(W, frames->name_ptr) != LUA_OK
        || tracy_write_u8(W, 1) != LUA_OK
        || tracy_write_u64(W, frames->count) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
    for (j = 0; j < frames->count; ++j) {
      if (tracy_write_time_offset(W, &ref, frames->starts[j]) != LUA_OK
          || tracy_write_i32(W, -1) != LUA_OK)
        return LMPROF_REPORT_FAILURE;
    }
  }

  /* String storage and ID maps. */
  if (tracy_write_u64(W, E->string_count) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  for (i = 0; i < E->string_count; ++i) {
    if (tracy_write_u64(W, E->strings[i].ptr) != LUA_OK
        || tracy_write_u64(W, E->strings[i].len) != LUA_OK
        || tracy_file_write(W, E->strings[i].value, E->strings[i].len) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
  }

  if (tracy_write_u64(W, E->string_count) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  for (i = 0; i < E->string_count; ++i) {
    if (tracy_write_u64(W, E->strings[i].ptr) != LUA_OK
        || tracy_write_u64(W, E->strings[i].ptr) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
  }

  if (tracy_write_u64(W, E->thread_count) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  for (i = 0; i < E->thread_count; ++i) {
    TracyThread *thread = &E->threads[i];
    if (tracy_write_u64(W, thread->tid) != LUA_OK
        || tracy_write_u64(W, thread->name_ptr) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
  }

  /* External names. */
  if (tracy_write_u64(W, 0) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  /* Thread compression tables. */
  if (tracy_write_u64(W, E->thread_count + 1) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  for (i = 0; i < E->thread_count; ++i) {
    if (tracy_write_u64(W, E->threads[i].tid) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
  }
  if (tracy_write_u64(W, 0) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  /* Source locations. */
  if (tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 1) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, E->source_count) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  for (i = 0; i < E->source_count; ++i) {
    if (tracy_write_source_location(W, &E->sources[i]) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
  }

  if (tracy_write_u64(W, E->source_count) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  for (i = 0; i < E->source_count; ++i) {
    if (tracy_write_i16(W, E->sources[i].id) != LUA_OK
        || tracy_write_u64(W, E->sources[i].count) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
  }
  if (tracy_write_u64(W, 0) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  /* Locks, messages, and zone extras. */
  if (tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 1) != LUA_OK
      || tracy_write_zero_zone_extra(W) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  /* CPU zones. */
  if (tracy_write_u64(W, E->zone_count) != LUA_OK
      || tracy_write_u64(W, E->child_vector_count) != LUA_OK
      || tracy_write_u64(W, E->thread_count) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  for (i = 0; i < E->thread_count; ++i) {
    TracyThread *thread = &E->threads[i];
    int64_t ref = 0;
    if (tracy_write_u64(W, thread->tid) != LUA_OK
        || tracy_write_u64(W, thread->zone_count) != LUA_OK
        || tracy_write_u64(W, 0) != LUA_OK
        || tracy_write_u8(W, 0) != LUA_OK
        || tracy_write_i32(W, 0) != LUA_OK
        || tracy_write_zone_list(W, thread->roots, thread->root_count, &ref) != LUA_OK
        || tracy_write_u64(W, 0) != LUA_OK
        || tracy_write_u64(W, 0) != LUA_OK
        || tracy_write_u64(W, 0) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
  }

  /* GPU zones and plots. */
  if (tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, E->plot_count) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  for (i = 0; i < E->plot_count; ++i) {
    TracyPlot *plot = &E->plots[i];
    size_t p;
    int64_t ref = 0;
    if (tracy_write_u8(W, TRACY_PLOT_TYPE_USER) != LUA_OK
        || tracy_write_u8(W, plot->format) != LUA_OK
        || tracy_write_u8(W, 0) != LUA_OK
        || tracy_write_u8(W, 1) != LUA_OK
        || tracy_write_u32(W, 0) != LUA_OK
        || tracy_write_u64(W, plot->name_ptr) != LUA_OK
        || tracy_write_double(W, plot->min) != LUA_OK
        || tracy_write_double(W, plot->max) != LUA_OK
        || tracy_write_double(W, plot->sum) != LUA_OK
        || tracy_write_u64(W, plot->count) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
    for (p = 0; p < plot->count; ++p) {
      if (tracy_write_time_offset(W, &ref, plot->points[p].time) != LUA_OK
          || tracy_write_double(W, plot->points[p].value) != LUA_OK)
        return LMPROF_REPORT_FAILURE;
    }
  }

  /* Memory section with the default empty memory pool. */
  if (tracy_write_u64(W, 1) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, max64) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  /* Call stacks, call stack frames, app info, frame images. */
  if (tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u32(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  /* Context switches. */
  if (tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  for (i = 0; i < 256; ++i) {
    if (tracy_write_u64(W, 0) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
  }

  /* PID/TID maps, symbols, hardware samples, and source cache. */
  return tracy_write_u64(W, 0) == LUA_OK
      && tracy_write_u64(W, 0) == LUA_OK
      && tracy_write_u64(W, 0) == LUA_OK
      && tracy_write_u64(W, 0) == LUA_OK
      && tracy_write_u64(W, 0) == LUA_OK
      && tracy_write_u64(W, 0) == LUA_OK
      && tracy_write_u64(W, 0) == LUA_OK
      && tracy_write_u64(W, 0) == LUA_OK
      && tracy_write_u64(W, 0) == LUA_OK
      && tracy_file_flush(W) == LUA_OK
      ? LUA_OK : LMPROF_REPORT_FAILURE;
}

static const char *tracy_routine_name(lua_State *L, lmprof_State *st, const TraceEvent *event) {
  const lua_Integer tid = event->call.proc.tid;
  const char *opt = tid == st->thread.mainproc.tid ? CHROME_NAME_MAIN : CHROME_META_TICK;
  return lmprof_thread_name(L, tid, opt);
}

int lmprof_tracy_traceevent_report(lua_State *L, lmprof_Report *R, TraceEventTimeline *list, const char *file) {
  lmprof_State *st = R->st;
  TracyExport E;
  TracyFileWriter W;
  TraceEventBounds bounds;
  TraceEventPage *page = l_nullptr;
  TraceEvent *routine_begin = l_nullptr;
  size_t counter = 0;
  size_t counterFrequency = TRACE_EVENT_COUNTER_FREQ;
  uint64_t base_ns = 0;
  int result = LUA_OK;

  memset(&E, 0, sizeof(E));
  if (st->i.counterFrequency > 0)
    counterFrequency = l_cast(size_t, st->i.counterFrequency);

  timeline_adjust(list);
  if (BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_COMPRESS)) {
    TraceEventCompressOpts opts;
    opts.id.pid = 0;
    opts.id.tid = 0;
    opts.threshold = st->i.event_threshold;
    result = timeline_compress(list, opts);
    if (result != TRACE_EVENT_OK) {
      luaL_error(L, "trace event compression error: %d", result);
      result = LMPROF_REPORT_FAILURE;
      goto done;
    }
  }

  /*
  ** Tracy's Chrome importer sees browser-thread frame messages before the
  ** renderer CPU zone thread is saved, so the renderer compresses to ordinal 2.
  */
  if (tracy_export_set_chrome_thread_name(&E, st->thread.mainproc.pid, LMPROF_THREAD_BROWSER, CHROME_NAME_CR_BROWSER) != LUA_OK
      || tracy_export_set_chrome_thread_name(&E, st->thread.mainproc.pid, st->thread.mainproc.tid, CHROME_NAME_CR_RENDERER) != LUA_OK) {
    result = LMPROF_REPORT_FAILURE;
    goto done;
  }

  if (lmprof_traceevent_find_bounds(st, list, &bounds)) {
    const uint64_t end_ns = lmprof_traceevent_adjusted_time_ns(st, bounds.end);
    base_ns = lmprof_traceevent_adjusted_time_ns(st, bounds.start);
    if (tracy_export_add_zone(&E, tracy_main_tid(st), CHROME_NAME_RUN_TASK, "", 0, 0, tracy_rebase_time_ns(end_ns, base_ns)) == l_nullptr) {
      result = LMPROF_REPORT_FAILURE;
      goto done;
    }
  }

  for (page = list->head; page != l_nullptr; page = page->next) {
    size_t i;
    for (i = 0; i < page->count; ++i) {
      TraceEvent *event = &page->event_array[i];
      const TraceEventType op = lmprof_traceevent_report_op(event);
      const int64_t time_ns = op_adjust(op) ? tracy_rebase_time_ns(lmprof_traceevent_time_ns(st, event->call.s.time), base_ns) : 0;

      switch (op) {
        case BEGIN_FRAME:
          if (tracy_frame_add(&E, "BeginFrame", time_ns) != LUA_OK) {
            result = LMPROF_REPORT_FAILURE;
            goto done;
          }
          break;
        case END_FRAME:
          if (tracy_frame_add(&E, "DrawFrame", time_ns) != LUA_OK) {
            result = LMPROF_REPORT_FAILURE;
            goto done;
          }
          break;
        case BEGIN_ROUTINE:
          routine_begin = event;
          break;
        case END_ROUTINE:
          if (routine_begin != l_nullptr) {
            const int64_t start_ns = tracy_rebase_time_ns(lmprof_traceevent_time_ns(st, routine_begin->call.s.time), base_ns);
            const uint64_t tid = tracy_main_tid(st);
            if (tracy_export_add_zone(&E, tid, tracy_routine_name(L, st, routine_begin), "", 0, start_ns, time_ns) == l_nullptr) {
              result = LMPROF_REPORT_FAILURE;
              goto done;
            }
            routine_begin = l_nullptr;
          }
          break;
        case ENTER_SCOPE:
          if (event->data.event.sibling != l_nullptr) {
            const int64_t end_ns = tracy_rebase_time_ns(lmprof_traceevent_time_ns(st, event->data.event.sibling->call.s.time), base_ns);
            if (tracy_export_add_zone(&E, tracy_event_tid(event), CHROME_EVENT_NAME(event), "", 0, time_ns, end_ns) == l_nullptr) {
              result = LMPROF_REPORT_FAILURE;
              goto done;
            }
          }
          /* fall through */
        case EXIT_SCOPE:
          if (BITFIELD_TEST(st->mode, LMPROF_MODE_MEMORY) && (counterFrequency == 1 || ((++counter) % counterFrequency) == 0)) {
            if (tracy_plot_add_point(&E, "LuaMemory", TRACY_PLOT_FORMAT_NUMBER, time_ns, l_cast(double, unit_allocated(&event->call.s))) != LUA_OK) {
              result = LMPROF_REPORT_FAILURE;
              goto done;
            }
            counter = 0;
          }
          break;
        case THREAD:
          if (tracy_export_set_chrome_thread_name(&E, event->call.proc.pid, event->call.proc.tid, CHROME_OPT_NAME(event->data.process.name, CHROME_META_TICK)) != LUA_OK) {
            result = LMPROF_REPORT_FAILURE;
            goto done;
          }
          break;
        case PROCESS:
        case LINE_SCOPE:
        case SAMPLE_EVENT:
        case IGNORE_SCOPE:
          break;
        default:
          result = LMPROF_REPORT_FAILURE;
          goto done;
      }
    }
  }

  if (tracy_file_open(&W, R->f.file) != LUA_OK || tracy_write_capture(&W, &E, st, file) != LUA_OK)
    result = LMPROF_REPORT_FAILURE;

done:
  {
    size_t i;
    for (i = 0; i < E.string_count; ++i)
      free(E.strings[i].value);
    free(E.strings);
    for (i = 0; i < E.source_count; ++i) {
      free(E.sources[i].name);
      free(E.sources[i].file);
    }
    free(E.sources);
    for (i = 0; i < E.thread_count; ++i)
      free(E.threads[i].roots);
    free(E.threads);
    for (i = 0; i < E.zone_count; ++i) {
      free(E.zones[i]->children);
      free(E.zones[i]);
    }
    free(E.zones);
    for (i = 0; i < E.frame_count; ++i) {
      free(E.frames[i].name);
      free(E.frames[i].starts);
    }
    free(E.frames);
    for (i = 0; i < E.plot_count; ++i) {
      free(E.plots[i].name);
      free(E.plots[i].points);
    }
    free(E.plots);
  }

  return result;
}
