/*
** $Id: lmprof_report_internal.h $
** Private report formatting helpers.
** See Copyright Notice in lmprof_lib.h
*/
#ifndef lmprof_report_internal_h
#define lmprof_report_internal_h

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lmprof.h"
#include "lmprof_report.h"

/* Unsafe macro to reduce fprintf clutter */
#define LMPROF_NL "\n"
#define LMPROF_INDENT "\t"

/* lua_pushfstring integer format */
#if LUA_VERSION_NUM >= 503
  #define LUA_INT_FORMAT "%I"
  #define LUA_UNIT_FORMAT "%I"

  #define LUA_INT_CAST(T) l_cast(lua_Integer, T)
  #define LUA_UNIT_CAST(T) l_cast(lua_Integer, T)
#else
  #define LUA_INT_FORMAT "%d"
  #define LUA_UNIT_FORMAT "%f"

  #define LUA_INT_CAST(T) l_cast(int, T)
  #define LUA_UNIT_CAST(T) l_cast(lua_Number, T)
#endif

/* Add a string literal to a string buffer. */
#if !defined(luaL_addliteral)
  #define luaL_addliteral(B, s) \
    luaL_addlstring(B, "" s, (sizeof((s)) / sizeof(char)) - 1)
#endif

/* Adds a formatted string literal to a string buffer. */
#define luaL_addfstring(L, B, F, ...)       \
  LUA_MLM_BEGIN                             \
  lua_pushfstring((L), (F), ##__VA_ARGS__); \
  luaL_addvalue((B));                       \
  LUA_MLM_END

/* LMPROF_PRINTF for string buffers. */
#define luaL_addifstring(L, B, F, ...)                                        \
  LUA_MLM_BEGIN                                                               \
  lua_pushfstring((L), "%s" LMPROF_INDENT "" F "," LMPROF_NL, ##__VA_ARGS__); \
  luaL_addvalue((B));                                                         \
  LUA_MLM_END

#if defined(LMPROF_FILE_API)
  /* Hacky macro for printing consistently strings */
  #define LMPROF_PRINTF(F, L, I, ...) fprintf((F), "%s" LMPROF_INDENT "" L "," LMPROF_NL, (I), ##__VA_ARGS__)
#endif

/* For identifiers to be faithfully represented in prior versions of Lua, they
** are encoded as formatted strings instead of integers. */
#define IDENTIFIER_BUFFER_LENGTH 256

#define CHROME_META_BEGIN "B"
#define CHROME_META_END "E"
#define CHROME_META_PROCESS "process_name"
#define CHROME_META_THREAD "thread_name"
#define CHROME_META_TICK "Routine"

#define CHROME_NAME_MAIN "Main"
#define CHROME_NAME_PROCESS "Process"
#define CHROME_NAME_BROWSER "Browser"
#define CHROME_NAME_SAMPLER "Instruction Sampling"
#define CHROME_NAME_CR_BROWSER "CrBrowserMain"
#define CHROME_NAME_CR_RENDERER "CrRendererMain"
#define CHROME_NAME_MEMORY_COUNTER "UpdateCounters LuaMemory"
#define CHROME_NAME_RUN_TASK "RunTask"

#define CHROME_USER_TIMING "blink.user_timing"
#define CHROME_TIMLINE "disabled-by-default-devtools.timeline"
#define CHROME_TIMELINE_FRAME "disabled-by-default-devtools.timeline.frame"

#define CHROME_OPT_NAME(n, o) (((n) == l_nullptr || *(n) == '\0') ? (o) : (n))
#define CHROME_EVENT_NAME(E) CHROME_OPT_NAME((E)->data.event.info->source, LMPROF_RECORD_NAME_UNKNOWN)

#define JSON_OPEN_OBJ "{"
#define JSON_CLOSE_OBJ "}"
#define JSON_OPEN_ARRAY "["
#define JSON_CLOSE_ARRAY "]"
#define JSON_DELIM ", "
#define JSON_NEWLINE "\n"
#define JSON_STRING(S) "\"" S "\""
#define JSON_ASSIGN(K, V) "\"" K "\":" V

typedef enum lmprof_TraceFileFormat {
  LMPROF_TRACE_FILE_JSON = 0,
  LMPROF_TRACE_FILE_PERFETTO = 1,
  LMPROF_TRACE_FILE_TRACY = 2
} lmprof_TraceFileFormat;

typedef struct TraceEventBounds {
  int valid;
  lu_time start;
  lu_time end;
} TraceEventBounds;

/* Some sugar for simplifying array incrementing for table reporting */
#define REPORT_TABLE_APPEND(L, R, X)                            \
  LUA_MLM_BEGIN                                                 \
  X; /* @TODO: Sanitize result */                               \
  if ((R)->type == lTable)                                      \
    lua_rawseti((L), (R)->t.table_index, (R)->t.array_count++); \
  LUA_MLM_END

#define REPORT_ENSURE_FILE_DELIM(R)                                    \
  LUA_MLM_BEGIN                                                        \
  if ((R)->f.delim) {                                                  \
    fprintf((R)->f.file, JSON_DELIM JSON_NEWLINE "%s", (R)->f.indent); \
    (R)->f.delim = 0;                                                  \
  }                                                                    \
  LUA_MLM_END

#define REPORT_ENSURE_BUFFER_DELIM(R)                     \
  LUA_MLM_BEGIN                                           \
  if ((R)->b.delim) {                                     \
    luaL_addliteral(&R->b.buff, JSON_DELIM JSON_NEWLINE); \
    luaL_addstring(&R->b.buff, (R)->b.indent);            \
    (R)->b.delim = 0;                                     \
  }                                                       \
  LUA_MLM_END

int lmprof_report_profiler_header(lua_State *L, lmprof_Report *R);
int lmprof_graph_report(lua_State *L, lmprof_Report *report);
int lmprof_traceevent_report(lua_State *L, lmprof_Report *report);
int lmprof_perfetto_traceevent_report(lua_State *L, lmprof_Report *R, TraceEventTimeline *list);
int lmprof_tracy_traceevent_report(lua_State *L, lmprof_Report *R, TraceEventTimeline *list, const char *file);

TraceEventType lmprof_traceevent_report_op(const TraceEvent *event);
int lmprof_traceevent_find_bounds(lmprof_State *st, TraceEventTimeline *list, TraceEventBounds *bounds);
uint64_t lmprof_traceevent_time_ns(const lmprof_State *st, lu_time time);
uint64_t lmprof_traceevent_adjusted_time_ns(const lmprof_State *st, lu_time time);

#endif
