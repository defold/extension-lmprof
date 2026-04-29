/*
** $Id: lmprof_report_perfetto.c $
** Perfetto TrackEvent protobuf output.
** See Copyright Notice in lmprof_lib.h
*/
#define LUA_LIB

#include <stdlib.h>

#include "lmprof_report_internal.h"

/*
** {==================================================================
** Perfetto TrackEvent protobuf file output
** ===================================================================
*/

#define PB_WIRE_VARINT 0
#define PB_WIRE_DELIMITED 2

#define PERFETTO_TRACE_PACKET 1
#define PERFETTO_PACKET_TIMESTAMP 8
#define PERFETTO_PACKET_TRUSTED_SEQUENCE_ID 10
#define PERFETTO_PACKET_TRACK_EVENT 11
#define PERFETTO_PACKET_TRACK_DESCRIPTOR 60
#define PERFETTO_TRUSTED_SEQUENCE_ID 1

#define PERFETTO_TRACK_UUID 1
#define PERFETTO_TRACK_NAME 2
#define PERFETTO_TRACK_PROCESS 3
#define PERFETTO_TRACK_THREAD 4
#define PERFETTO_TRACK_PARENT_UUID 5
#define PERFETTO_TRACK_COUNTER 8
#define PERFETTO_TRACK_DISALLOW_SYSTEM_MERGE 9

#define PERFETTO_PROCESS_PID 1
#define PERFETTO_PROCESS_NAME 6
#define PERFETTO_THREAD_PID 1
#define PERFETTO_THREAD_TID 2
#define PERFETTO_THREAD_NAME 5
#define PERFETTO_COUNTER_UNIT 3
#define PERFETTO_COUNTER_UNIT_SIZE_BYTES 3

#define PERFETTO_EVENT_TYPE 9
#define PERFETTO_EVENT_TRACK_UUID 11
#define PERFETTO_EVENT_CATEGORY 22
#define PERFETTO_EVENT_NAME 23
#define PERFETTO_EVENT_COUNTER_VALUE 30

#define PERFETTO_EVENT_SLICE_BEGIN 1
#define PERFETTO_EVENT_SLICE_END 2
#define PERFETTO_EVENT_INSTANT 3
#define PERFETTO_EVENT_COUNTER 4

#define PERFETTO_PROCESS_UUID_BASE 0x1000000000000000ULL
#define PERFETTO_THREAD_UUID_BASE 0x2000000000000000ULL
#define PERFETTO_COUNTER_UUID_BASE 0x3000000000000000ULL

static size_t pb_size_varint(uint64_t value) {
  size_t size = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++size;
  }
  return size;
}

static size_t pb_size_key(uint32_t field_id, uint32_t wire_type) {
  return pb_size_varint((l_cast(uint64_t, field_id) << 3) | wire_type);
}

static size_t pb_size_uint64_field(uint32_t field_id, uint64_t value) {
  return pb_size_key(field_id, PB_WIRE_VARINT) + pb_size_varint(value);
}

static size_t pb_size_int32_field(uint32_t field_id, int32_t value) {
  return pb_size_uint64_field(field_id, l_cast(uint64_t, l_cast(int64_t, value)));
}

static size_t pb_size_int64_field(uint32_t field_id, int64_t value) {
  return pb_size_uint64_field(field_id, l_cast(uint64_t, value));
}

static size_t pb_size_bool_field(uint32_t field_id) {
  return pb_size_key(field_id, PB_WIRE_VARINT) + 1;
}

static size_t pb_size_string_field(uint32_t field_id, const char *value) {
  const size_t len = value == l_nullptr ? 0 : strlen(value);
  return pb_size_key(field_id, PB_WIRE_DELIMITED) + pb_size_varint(l_cast(uint64_t, len)) + len;
}

static size_t pb_size_message_field(uint32_t field_id, size_t len) {
  return pb_size_key(field_id, PB_WIRE_DELIMITED) + pb_size_varint(l_cast(uint64_t, len)) + len;
}

static int pb_write_bytes(FILE *f, const void *data, size_t len) {
  if (len == 0)
    return LUA_OK;
  return fwrite(data, 1, len, f) == len ? LUA_OK : LMPROF_REPORT_FAILURE;
}

static int pb_write_varint(FILE *f, uint64_t value) {
  unsigned char buffer[10];
  size_t len = 0;
  do {
    unsigned char byte = l_cast(unsigned char, value & 0x7f);
    value >>= 7;
    if (value != 0)
      byte = l_cast(unsigned char, byte | 0x80);
    buffer[len++] = byte;
  } while (value != 0);

  return pb_write_bytes(f, buffer, len);
}

static int pb_write_key(FILE *f, uint32_t field_id, uint32_t wire_type) {
  return pb_write_varint(f, (l_cast(uint64_t, field_id) << 3) | wire_type);
}

static int pb_write_uint64_field(FILE *f, uint32_t field_id, uint64_t value) {
  if (pb_write_key(f, field_id, PB_WIRE_VARINT) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  return pb_write_varint(f, value);
}

static int pb_write_int32_field(FILE *f, uint32_t field_id, int32_t value) {
  return pb_write_uint64_field(f, field_id, l_cast(uint64_t, l_cast(int64_t, value)));
}

static int pb_write_int64_field(FILE *f, uint32_t field_id, int64_t value) {
  return pb_write_uint64_field(f, field_id, l_cast(uint64_t, value));
}

static int pb_write_bool_field(FILE *f, uint32_t field_id, int value) {
  return pb_write_uint64_field(f, field_id, value ? 1 : 0);
}

static int pb_write_message_prefix(FILE *f, uint32_t field_id, size_t len) {
  if (pb_write_key(f, field_id, PB_WIRE_DELIMITED) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  return pb_write_varint(f, l_cast(uint64_t, len));
}

static int pb_write_string_field(FILE *f, uint32_t field_id, const char *value) {
  const size_t len = value == l_nullptr ? 0 : strlen(value);
  if (pb_write_message_prefix(f, field_id, len) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  return pb_write_bytes(f, value == l_nullptr ? "" : value, len);
}

static uint64_t perfetto_process_uuid(lua_Integer pid) {
  return PERFETTO_PROCESS_UUID_BASE | l_cast(uint64_t, l_cast(uint32_t, pid));
}

static uint64_t perfetto_thread_uuid(lua_Integer pid, lua_Integer tid) {
  return PERFETTO_THREAD_UUID_BASE | (l_cast(uint64_t, l_cast(uint32_t, pid)) << 32) | l_cast(uint64_t, l_cast(uint32_t, tid));
}

static uint64_t perfetto_memory_counter_uuid(lua_Integer pid) {
  return PERFETTO_COUNTER_UUID_BASE | l_cast(uint64_t, l_cast(uint32_t, pid));
}

static lua_Integer perfetto_event_tid(lmprof_Report *R, const TraceEvent *event) {
  return op_routine(event->op) ? R->st->thread.mainproc.tid : event->call.proc.tid;
}

static uint64_t perfetto_event_track_uuid(lmprof_Report *R, const TraceEvent *event) {
  return perfetto_thread_uuid(event->call.proc.pid, perfetto_event_tid(R, event));
}

static int perfetto_write_trace_packet(FILE *f, size_t packet_size) {
  const size_t sequence_size = pb_size_uint64_field(PERFETTO_PACKET_TRUSTED_SEQUENCE_ID, PERFETTO_TRUSTED_SEQUENCE_ID);
  if (pb_write_message_prefix(f, PERFETTO_TRACE_PACKET, packet_size + sequence_size) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  return pb_write_uint64_field(f, PERFETTO_PACKET_TRUSTED_SEQUENCE_ID, PERFETTO_TRUSTED_SEQUENCE_ID);
}

static int perfetto_write_process_track(lmprof_Report *R, lua_Integer pid, const char *name) {
  FILE *f = R->f.file;
  const char *process_name = CHROME_OPT_NAME(name, CHROME_NAME_PROCESS);
  const size_t process_size = pb_size_int32_field(PERFETTO_PROCESS_PID, l_cast(int32_t, pid))
                            + pb_size_string_field(PERFETTO_PROCESS_NAME, process_name);
  const size_t descriptor_size = pb_size_uint64_field(PERFETTO_TRACK_UUID, perfetto_process_uuid(pid))
                               + pb_size_string_field(PERFETTO_TRACK_NAME, process_name)
                               + pb_size_message_field(PERFETTO_TRACK_PROCESS, process_size);
  const size_t packet_size = pb_size_message_field(PERFETTO_PACKET_TRACK_DESCRIPTOR, descriptor_size);

  if (perfetto_write_trace_packet(f, packet_size) != LUA_OK
      || pb_write_message_prefix(f, PERFETTO_PACKET_TRACK_DESCRIPTOR, descriptor_size) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_TRACK_UUID, perfetto_process_uuid(pid)) != LUA_OK
      || pb_write_string_field(f, PERFETTO_TRACK_NAME, process_name) != LUA_OK
      || pb_write_message_prefix(f, PERFETTO_TRACK_PROCESS, process_size) != LUA_OK
      || pb_write_int32_field(f, PERFETTO_PROCESS_PID, l_cast(int32_t, pid)) != LUA_OK
      || pb_write_string_field(f, PERFETTO_PROCESS_NAME, process_name) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  return LUA_OK;
}

static int perfetto_write_thread_track(lmprof_Report *R, lua_Integer pid, lua_Integer tid, const char *name) {
  FILE *f = R->f.file;
  const char *thread_name = CHROME_OPT_NAME(name, CHROME_META_TICK);
  const size_t thread_size = pb_size_int32_field(PERFETTO_THREAD_PID, l_cast(int32_t, pid))
                           + pb_size_int64_field(PERFETTO_THREAD_TID, l_cast(int64_t, tid))
                           + pb_size_string_field(PERFETTO_THREAD_NAME, thread_name);
  const size_t descriptor_size = pb_size_uint64_field(PERFETTO_TRACK_UUID, perfetto_thread_uuid(pid, tid))
                               + pb_size_string_field(PERFETTO_TRACK_NAME, thread_name)
                               + pb_size_message_field(PERFETTO_TRACK_THREAD, thread_size)
                               + pb_size_bool_field(PERFETTO_TRACK_DISALLOW_SYSTEM_MERGE);
  const size_t packet_size = pb_size_message_field(PERFETTO_PACKET_TRACK_DESCRIPTOR, descriptor_size);

  if (perfetto_write_trace_packet(f, packet_size) != LUA_OK
      || pb_write_message_prefix(f, PERFETTO_PACKET_TRACK_DESCRIPTOR, descriptor_size) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_TRACK_UUID, perfetto_thread_uuid(pid, tid)) != LUA_OK
      || pb_write_string_field(f, PERFETTO_TRACK_NAME, thread_name) != LUA_OK
      || pb_write_message_prefix(f, PERFETTO_TRACK_THREAD, thread_size) != LUA_OK
      || pb_write_int32_field(f, PERFETTO_THREAD_PID, l_cast(int32_t, pid)) != LUA_OK
      || pb_write_int64_field(f, PERFETTO_THREAD_TID, l_cast(int64_t, tid)) != LUA_OK
      || pb_write_string_field(f, PERFETTO_THREAD_NAME, thread_name) != LUA_OK
      || pb_write_bool_field(f, PERFETTO_TRACK_DISALLOW_SYSTEM_MERGE, 1) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  return LUA_OK;
}

static int perfetto_write_memory_counter_track(lmprof_Report *R, lua_Integer pid) {
  FILE *f = R->f.file;
  const size_t counter_size = pb_size_uint64_field(PERFETTO_COUNTER_UNIT, PERFETTO_COUNTER_UNIT_SIZE_BYTES);
  const size_t descriptor_size = pb_size_uint64_field(PERFETTO_TRACK_UUID, perfetto_memory_counter_uuid(pid))
                               + pb_size_uint64_field(PERFETTO_TRACK_PARENT_UUID, perfetto_process_uuid(pid))
                               + pb_size_string_field(PERFETTO_TRACK_NAME, CHROME_NAME_MEMORY_COUNTER)
                               + pb_size_message_field(PERFETTO_TRACK_COUNTER, counter_size);
  const size_t packet_size = pb_size_message_field(PERFETTO_PACKET_TRACK_DESCRIPTOR, descriptor_size);

  if (perfetto_write_trace_packet(f, packet_size) != LUA_OK
      || pb_write_message_prefix(f, PERFETTO_PACKET_TRACK_DESCRIPTOR, descriptor_size) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_TRACK_UUID, perfetto_memory_counter_uuid(pid)) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_TRACK_PARENT_UUID, perfetto_process_uuid(pid)) != LUA_OK
      || pb_write_string_field(f, PERFETTO_TRACK_NAME, CHROME_NAME_MEMORY_COUNTER) != LUA_OK
      || pb_write_message_prefix(f, PERFETTO_TRACK_COUNTER, counter_size) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_COUNTER_UNIT, PERFETTO_COUNTER_UNIT_SIZE_BYTES) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  return LUA_OK;
}

static int perfetto_write_default_tracks(lua_State *L, lmprof_Report *R) {
  lmprof_State *st = R->st;
  const lua_Integer pid = st->thread.mainproc.pid;
  int result = LUA_OK;

  luaL_checkstack(L, 3, __FUNCTION__);
  result = perfetto_write_process_track(R, pid, CHROME_NAME_BROWSER);
  if (result != LUA_OK)
    return result;
  result = perfetto_write_thread_track(R, pid, LMPROF_THREAD_BROWSER, CHROME_NAME_CR_BROWSER);
  if (result != LUA_OK)
    return result;
  result = perfetto_write_thread_track(R, pid, st->thread.mainproc.tid, CHROME_NAME_CR_RENDERER);
  if (result != LUA_OK)
    return result;
  result = perfetto_write_thread_track(R, pid, LMPROF_THREAD_SAMPLE_TIMELINE, CHROME_NAME_SAMPLER);
  if (result != LUA_OK)
    return result;
  result = perfetto_write_memory_counter_track(R, pid);
  if (result != LUA_OK)
    return result;

  if (BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_LAYOUT_SPLIT)) {
    lmprof_thread_info(L, LMPROF_TAB_THREAD_NAMES); /* [..., names] */
    lua_pushnil(L); /* [..., names, nil] */
    while (lua_next(L, -2) != 0) { /* [..., names, key, value] */
      if (lua_isnumber(L, -2)) {
        const lua_Integer tid = lua_tointeger(L, -2);
        if (tid != LMPROF_THREAD_BROWSER && tid != st->thread.mainproc.tid && tid != LMPROF_THREAD_SAMPLE_TIMELINE) {
          result = perfetto_write_thread_track(R, pid, tid, lua_tostring(L, -1));
          if (result != LUA_OK) {
            lua_pop(L, 2);
            return result;
          }
        }
      }
      lua_pop(L, 1); /* [..., names, key] */
    }
    lua_pop(L, 1);
  }

  return LUA_OK;
}

static int perfetto_write_track_event_prefix(lmprof_Report *R, uint64_t timestamp_ns, size_t event_size) {
  FILE *f = R->f.file;
  const size_t packet_size = pb_size_uint64_field(PERFETTO_PACKET_TIMESTAMP, timestamp_ns)
                           + pb_size_message_field(PERFETTO_PACKET_TRACK_EVENT, event_size);
  if (perfetto_write_trace_packet(f, packet_size) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_PACKET_TIMESTAMP, timestamp_ns) != LUA_OK
      || pb_write_message_prefix(f, PERFETTO_PACKET_TRACK_EVENT, event_size) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  return LUA_OK;
}

static int perfetto_write_slice_begin(lmprof_Report *R, uint64_t track_uuid, uint64_t timestamp_ns, const char *category, const char *name) {
  FILE *f = R->f.file;
  const char *event_category = CHROME_OPT_NAME(category, "lmprof");
  const char *event_name = CHROME_OPT_NAME(name, LMPROF_RECORD_NAME_UNKNOWN);
  const size_t event_size = pb_size_string_field(PERFETTO_EVENT_CATEGORY, event_category)
                          + pb_size_string_field(PERFETTO_EVENT_NAME, event_name)
                          + pb_size_uint64_field(PERFETTO_EVENT_TYPE, PERFETTO_EVENT_SLICE_BEGIN)
                          + pb_size_uint64_field(PERFETTO_EVENT_TRACK_UUID, track_uuid);
  if (perfetto_write_track_event_prefix(R, timestamp_ns, event_size) != LUA_OK
      || pb_write_string_field(f, PERFETTO_EVENT_CATEGORY, event_category) != LUA_OK
      || pb_write_string_field(f, PERFETTO_EVENT_NAME, event_name) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_EVENT_TYPE, PERFETTO_EVENT_SLICE_BEGIN) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_EVENT_TRACK_UUID, track_uuid) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  return LUA_OK;
}

static int perfetto_write_slice_end(lmprof_Report *R, uint64_t track_uuid, uint64_t timestamp_ns) {
  FILE *f = R->f.file;
  const size_t event_size = pb_size_uint64_field(PERFETTO_EVENT_TYPE, PERFETTO_EVENT_SLICE_END)
                          + pb_size_uint64_field(PERFETTO_EVENT_TRACK_UUID, track_uuid);
  if (perfetto_write_track_event_prefix(R, timestamp_ns, event_size) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_EVENT_TYPE, PERFETTO_EVENT_SLICE_END) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_EVENT_TRACK_UUID, track_uuid) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  return LUA_OK;
}

static int perfetto_write_instant(lmprof_Report *R, uint64_t track_uuid, uint64_t timestamp_ns, const char *category, const char *name) {
  FILE *f = R->f.file;
  const char *event_category = CHROME_OPT_NAME(category, "lmprof");
  const char *event_name = CHROME_OPT_NAME(name, LMPROF_RECORD_NAME_UNKNOWN);
  const size_t event_size = pb_size_string_field(PERFETTO_EVENT_CATEGORY, event_category)
                          + pb_size_string_field(PERFETTO_EVENT_NAME, event_name)
                          + pb_size_uint64_field(PERFETTO_EVENT_TYPE, PERFETTO_EVENT_INSTANT)
                          + pb_size_uint64_field(PERFETTO_EVENT_TRACK_UUID, track_uuid);
  if (perfetto_write_track_event_prefix(R, timestamp_ns, event_size) != LUA_OK
      || pb_write_string_field(f, PERFETTO_EVENT_CATEGORY, event_category) != LUA_OK
      || pb_write_string_field(f, PERFETTO_EVENT_NAME, event_name) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_EVENT_TYPE, PERFETTO_EVENT_INSTANT) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_EVENT_TRACK_UUID, track_uuid) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  return LUA_OK;
}

static int perfetto_write_counter(lmprof_Report *R, lua_Integer pid, uint64_t timestamp_ns, int64_t value) {
  FILE *f = R->f.file;
  const size_t event_size = pb_size_uint64_field(PERFETTO_EVENT_TYPE, PERFETTO_EVENT_COUNTER)
                          + pb_size_uint64_field(PERFETTO_EVENT_TRACK_UUID, perfetto_memory_counter_uuid(pid))
                          + pb_size_int64_field(PERFETTO_EVENT_COUNTER_VALUE, value);
  if (perfetto_write_track_event_prefix(R, timestamp_ns, event_size) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_EVENT_TYPE, PERFETTO_EVENT_COUNTER) != LUA_OK
      || pb_write_uint64_field(f, PERFETTO_EVENT_TRACK_UUID, perfetto_memory_counter_uuid(pid)) != LUA_OK
      || pb_write_int64_field(f, PERFETTO_EVENT_COUNTER_VALUE, value) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  return LUA_OK;
}

typedef enum PerfettoQueuedEventType {
  PERFETTO_QUEUE_SLICE_BEGIN,
  PERFETTO_QUEUE_SLICE_END,
  PERFETTO_QUEUE_INSTANT,
  PERFETTO_QUEUE_LINE_INSTANT,
  PERFETTO_QUEUE_COUNTER
} PerfettoQueuedEventType;

typedef struct PerfettoQueuedEvent {
  PerfettoQueuedEventType type;
  const TraceEvent *event;
  uint64_t timestamp_ns;
  uint64_t start_ns;
  uint64_t end_ns;
  uint64_t track_uuid;
  lua_Integer pid;
  int64_t counter_value;
  const char *category;
  const char *name;
  unsigned int rank;
  size_t sequence;
} PerfettoQueuedEvent;

typedef struct PerfettoEventQueue {
  PerfettoQueuedEvent *events;
  size_t count;
  size_t capacity;
} PerfettoEventQueue;

#define PERFETTO_QUEUE_RANK_RUN_TASK 0u
#define PERFETTO_QUEUE_RANK_SCOPE 2u

static int perfetto_queue_phase_rank(const PerfettoQueuedEvent *event) {
  switch (event->type) {
    case PERFETTO_QUEUE_SLICE_END:
      return 0;
    case PERFETTO_QUEUE_SLICE_BEGIN:
      return 1;
    case PERFETTO_QUEUE_INSTANT:
    case PERFETTO_QUEUE_LINE_INSTANT:
      return 2;
    case PERFETTO_QUEUE_COUNTER:
      return 3;
    default:
      return 4;
  }
}

static int perfetto_queue_compare(const void *left, const void *right) {
  const PerfettoQueuedEvent *a = l_pcast(const PerfettoQueuedEvent *, left);
  const PerfettoQueuedEvent *b = l_pcast(const PerfettoQueuedEvent *, right);
  const int phase_a = perfetto_queue_phase_rank(a);
  const int phase_b = perfetto_queue_phase_rank(b);

  if (a->timestamp_ns != b->timestamp_ns)
    return a->timestamp_ns < b->timestamp_ns ? -1 : 1;
  if (phase_a != phase_b)
    return phase_a < phase_b ? -1 : 1;

  if (a->type == PERFETTO_QUEUE_SLICE_END && b->type == PERFETTO_QUEUE_SLICE_END) {
    if (a->start_ns != b->start_ns)
      return a->start_ns > b->start_ns ? -1 : 1;
    if (a->rank != b->rank)
      return a->rank > b->rank ? -1 : 1;
  }
  else if (a->type == PERFETTO_QUEUE_SLICE_BEGIN && b->type == PERFETTO_QUEUE_SLICE_BEGIN) {
    if (a->end_ns != b->end_ns)
      return a->end_ns > b->end_ns ? -1 : 1;
    if (a->rank != b->rank)
      return a->rank < b->rank ? -1 : 1;
  }

  if (a->sequence != b->sequence)
    return a->sequence < b->sequence ? -1 : 1;
  return 0;
}

static int perfetto_queue_reserve(PerfettoEventQueue *queue, size_t count) {
  PerfettoQueuedEvent *next;
  size_t next_capacity;
  if (count <= queue->capacity)
    return LUA_OK;

  next_capacity = queue->capacity == 0 ? 256 : queue->capacity * 2;
  while (next_capacity < count)
    next_capacity *= 2;

  next = l_pcast(PerfettoQueuedEvent *, realloc(queue->events, next_capacity * sizeof(PerfettoQueuedEvent)));
  if (next == l_nullptr)
    return LMPROF_REPORT_FAILURE;

  queue->events = next;
  queue->capacity = next_capacity;
  return LUA_OK;
}

static int perfetto_queue_push(PerfettoEventQueue *queue, PerfettoQueuedEvent event) {
  if (perfetto_queue_reserve(queue, queue->count + 1) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  event.sequence = queue->count;
  queue->events[queue->count++] = event;
  return LUA_OK;
}

static int perfetto_queue_instant(PerfettoEventQueue *queue, uint64_t track_uuid, uint64_t timestamp_ns, const char *category, const char *name) {
  PerfettoQueuedEvent event = LMPROF_ZERO_STRUCT;
  event.type = PERFETTO_QUEUE_INSTANT;
  event.timestamp_ns = timestamp_ns;
  event.track_uuid = track_uuid;
  event.category = category;
  event.name = name;
  return perfetto_queue_push(queue, event);
}

static int perfetto_queue_line_instant(PerfettoEventQueue *queue, uint64_t track_uuid, uint64_t timestamp_ns, const TraceEvent *source) {
  PerfettoQueuedEvent event = LMPROF_ZERO_STRUCT;
  event.type = PERFETTO_QUEUE_LINE_INSTANT;
  event.event = source;
  event.timestamp_ns = timestamp_ns;
  event.track_uuid = track_uuid;
  event.category = CHROME_USER_TIMING;
  return perfetto_queue_push(queue, event);
}

static int perfetto_queue_counter(PerfettoEventQueue *queue, lua_Integer pid, uint64_t timestamp_ns, int64_t value) {
  PerfettoQueuedEvent event = LMPROF_ZERO_STRUCT;
  event.type = PERFETTO_QUEUE_COUNTER;
  event.timestamp_ns = timestamp_ns;
  event.pid = pid;
  event.counter_value = value;
  return perfetto_queue_push(queue, event);
}

static int perfetto_queue_slice(PerfettoEventQueue *queue, uint64_t track_uuid, uint64_t start_ns, uint64_t end_ns, const char *category, const char *name, unsigned int rank) {
  PerfettoQueuedEvent begin = LMPROF_ZERO_STRUCT;
  PerfettoQueuedEvent end = LMPROF_ZERO_STRUCT;

  if (end_ns <= start_ns)
    return perfetto_queue_instant(queue, track_uuid, start_ns, category, name);

  begin.type = PERFETTO_QUEUE_SLICE_BEGIN;
  begin.timestamp_ns = start_ns;
  begin.start_ns = start_ns;
  begin.end_ns = end_ns;
  begin.track_uuid = track_uuid;
  begin.category = category;
  begin.name = name;
  begin.rank = rank;

  end.type = PERFETTO_QUEUE_SLICE_END;
  end.timestamp_ns = end_ns;
  end.start_ns = start_ns;
  end.end_ns = end_ns;
  end.track_uuid = track_uuid;
  end.rank = rank;

  if (perfetto_queue_push(queue, begin) != LUA_OK
      || perfetto_queue_push(queue, end) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  return LUA_OK;
}

static int perfetto_write_queued_event(lmprof_Report *R, const PerfettoQueuedEvent *event) {
  switch (event->type) {
    case PERFETTO_QUEUE_SLICE_BEGIN:
      return perfetto_write_slice_begin(R, event->track_uuid, event->timestamp_ns, event->category, event->name);
    case PERFETTO_QUEUE_SLICE_END:
      return perfetto_write_slice_end(R, event->track_uuid, event->timestamp_ns);
    case PERFETTO_QUEUE_INSTANT:
      return perfetto_write_instant(R, event->track_uuid, event->timestamp_ns, event->category, event->name);
    case PERFETTO_QUEUE_LINE_INSTANT: {
      char line_name[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
      const TraceEvent *source = event->event;
      const char *source_name = source != l_nullptr && source->data.line.info != l_nullptr
                              ? source->data.line.info->source
                              : LMPROF_RECORD_NAME_UNKNOWN;
      const int line = source != l_nullptr ? source->data.line.line : 0;
      if (snprintf(line_name, sizeof(line_name), "%s: Line %d", CHROME_OPT_NAME(source_name, LMPROF_RECORD_NAME_UNKNOWN), line) < 0)
        LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);
      return perfetto_write_instant(R, event->track_uuid, event->timestamp_ns, event->category, line_name);
    }
    case PERFETTO_QUEUE_COUNTER:
      return perfetto_write_counter(R, event->pid, event->timestamp_ns, event->counter_value);
    default:
      return LMPROF_REPORT_FAILURE;
  }
}

int lmprof_perfetto_traceevent_report(lua_State *L, lmprof_Report *R, TraceEventTimeline *list) {
  lmprof_State *st = R->st;
  PerfettoEventQueue queue = LMPROF_ZERO_STRUCT;
  TraceEventPage *page = l_nullptr;
  TraceEvent *samples = l_nullptr;
  TraceEventBounds bounds;
  int has_run_task = 0;
  int result = LUA_OK;

  size_t counter = 0;
  size_t counterFrequency = TRACE_EVENT_COUNTER_FREQ;

  timeline_adjust(list);
  if (st->i.counterFrequency > 0)
    counterFrequency = l_cast(size_t, st->i.counterFrequency);

  if (BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_COMPRESS)) {
    TraceEventCompressOpts opts;
    opts.id.pid = 0;
    opts.id.tid = 0;
    opts.threshold = st->i.event_threshold;
    if ((result = timeline_compress(list, opts)) != TRACE_EVENT_OK) {
      luaL_error(L, "trace event compression error: %d", result);
      return LMPROF_REPORT_FAILURE;
    }
  }

  if (perfetto_write_default_tracks(L, R) != LUA_OK)
    return LMPROF_REPORT_FAILURE;

  has_run_task = lmprof_traceevent_find_bounds(st, list, &bounds);
  if (has_run_task) {
    result = perfetto_queue_slice(&queue,
        perfetto_thread_uuid(st->thread.mainproc.pid, st->thread.mainproc.tid),
        lmprof_traceevent_adjusted_time_ns(st, bounds.start),
        lmprof_traceevent_adjusted_time_ns(st, bounds.end),
        CHROME_TIMLINE,
        CHROME_NAME_RUN_TASK,
        PERFETTO_QUEUE_RANK_RUN_TASK);
    if (result != LUA_OK)
      goto done;
  }

  for (page = list->head; page != l_nullptr; page = page->next) {
    size_t i;
    for (i = 0; i < page->count; ++i) {
      TraceEvent *event = &page->event_array[i];
      TraceEventType op = lmprof_traceevent_report_op(event);
      const uint64_t timestamp_ns = op_adjust(op) ? lmprof_traceevent_time_ns(st, event->call.s.time) : 0;
      const uint64_t track_uuid = op_adjust(op) ? perfetto_event_track_uuid(R, event) : 0;

      switch (op) {
        case BEGIN_FRAME:
          result = perfetto_queue_instant(&queue, track_uuid, timestamp_ns, CHROME_TIMELINE_FRAME, "BeginFrame");
          if (result != LUA_OK)
            goto done;
          break;
        case END_FRAME:
          if (perfetto_queue_instant(&queue, track_uuid, timestamp_ns, CHROME_TIMELINE_FRAME, "ActivateLayerTree") != LUA_OK
              || perfetto_queue_instant(&queue, track_uuid, timestamp_ns, CHROME_TIMELINE_FRAME, "DrawFrame") != LUA_OK) {
            result = LMPROF_REPORT_FAILURE;
            goto done;
          }
          break;
        case BEGIN_ROUTINE:
        case END_ROUTINE:
          break;
        case LINE_SCOPE:
          result = perfetto_queue_line_instant(&queue, track_uuid, timestamp_ns, event);
          if (result != LUA_OK)
            goto done;
          break;
        case SAMPLE_EVENT:
          if (samples != l_nullptr) {
            const uint64_t sample_track = perfetto_thread_uuid(st->thread.mainproc.pid, LMPROF_THREAD_SAMPLE_TIMELINE);
            result = perfetto_queue_slice(&queue,
                sample_track,
                lmprof_traceevent_time_ns(st, samples->call.s.time),
                timestamp_ns,
                CHROME_TIMLINE,
                "EvaluateScript",
                PERFETTO_QUEUE_RANK_SCOPE);
            if (result != LUA_OK)
              goto done;
          }
          samples = event;
          break;
        case ENTER_SCOPE:
          if (event->data.event.sibling != l_nullptr) {
            result = perfetto_queue_slice(&queue,
                track_uuid,
                timestamp_ns,
                lmprof_traceevent_time_ns(st, event->data.event.sibling->call.s.time),
                CHROME_TIMLINE,
                CHROME_EVENT_NAME(event),
                PERFETTO_QUEUE_RANK_SCOPE);
            if (result != LUA_OK)
              goto done;
          }
          if (BITFIELD_TEST(st->mode, LMPROF_MODE_MEMORY) && (counterFrequency == 1 || ((++counter) % counterFrequency) == 0)) {
            result = perfetto_queue_counter(&queue, event->call.proc.pid, timestamp_ns, l_cast(int64_t, unit_allocated(&event->call.s)));
            if (result != LUA_OK)
              goto done;
            counter = 0;
          }
          break;
        case EXIT_SCOPE:
          if (BITFIELD_TEST(st->mode, LMPROF_MODE_MEMORY) && (counterFrequency == 1 || ((++counter) % counterFrequency) == 0)) {
            result = perfetto_queue_counter(&queue, event->call.proc.pid, timestamp_ns, l_cast(int64_t, unit_allocated(&event->call.s)));
            if (result != LUA_OK)
              goto done;
            counter = 0;
          }
          break;
        case PROCESS:
          if (perfetto_write_process_track(R, event->call.proc.pid, event->data.process.name) != LUA_OK
              || perfetto_write_memory_counter_track(R, event->call.proc.pid) != LUA_OK) {
            result = LMPROF_REPORT_FAILURE;
            goto done;
          }
          break;
        case THREAD:
          if (perfetto_write_thread_track(R, event->call.proc.pid, event->call.proc.tid, event->data.process.name) != LUA_OK) {
            result = LMPROF_REPORT_FAILURE;
            goto done;
          }
          break;
        case IGNORE_SCOPE:
          break;
        default:
          result = LMPROF_REPORT_FAILURE;
          goto done;
      }
    }
  }

  if (queue.count > 1)
    qsort(queue.events, queue.count, sizeof(PerfettoQueuedEvent), perfetto_queue_compare);

  {
    size_t i;
    for (i = 0; i < queue.count; ++i) {
      if (perfetto_write_queued_event(R, &queue.events[i]) != LUA_OK) {
        result = LMPROF_REPORT_FAILURE;
        goto done;
      }
    }
  }

done:
  free(queue.events);
  return result;
}

