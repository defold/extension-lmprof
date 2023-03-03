/*
** $Id: lmprof_report.c $
** Output/Formatting
** See Copyright Notice in lmprof_lib.h
*/
#define LUA_LIB

#include <string.h>
#include <errno.h>

#include "lmprof_conf.h"

#include "collections/lmprof_record.h"
#include "collections/lmprof_traceevent.h"
#include "collections/lmprof_hash.h"

#include "lmprof.h"
#include "lmprof_state.h"
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

/*
** {==================================================================
** File Handling
** ===================================================================
*/
#if defined(LMPROF_FILE_API)
  #include <stdio.h>

  /* Hacky macro for printing consistently strings */
  #define LMPROF_PRINTF(F, L, I, ...) fprintf((F), "%s" LMPROF_INDENT "" L "," LMPROF_NL, (I), ##__VA_ARGS__)

/* iolib.f_gc */
static int io_fgc(lua_State *L) {
  FILE **f = l_pcast(FILE **, luaL_checkudata(L, 1, LMPROF_IO_METATABLE));
  if (*f != l_nullptr) {
    fclose(*f);
    *f = l_nullptr;
  }
  return 0;
}

/* Create & Open a file-handle userdata, placing it ontop of the Lua stack */
static FILE **io_fud(lua_State *L, const char *output) {
  FILE **pf = l_pcast(FILE **, lmprof_newuserdata(L, sizeof(FILE *)));
  *pf = l_nullptr;

  #if LUA_VERSION_NUM == 501
  luaL_getmetatable(L, LMPROF_IO_METATABLE);
  lua_setmetatable(L, -2);
  #else
  luaL_setmetatable(L, LMPROF_IO_METATABLE);
  #endif

  /* Open File... consider destroying the profiler state on failure? */
  if ((*pf = fopen(output, "w")) == l_nullptr) {
    luaL_error(L, "cannot open file '%s' (%s)", output, strerror(errno));
    return l_nullptr;
  }
  return pf;
}

#endif
/* }================================================================== */

/*
** {==================================================================
** Graph Profiler Format
** ===================================================================
*/

/*
** For identifiers to be faithfully represented in prior versions of Lua, they
** are encoded as formatted strings instead of integers.
*/
#define IDENTIFIER_BUFFER_LENGTH 256

static int profiler_header(lua_State *L, lmprof_Report *R) {
  lmprof_State *st = R->st;
  const uint32_t mode = R->st->mode;
  const uint32_t conf = R->st->conf;
  if (R->type == lTable) {
    luaL_settabss(L, "clockid", LMPROF_TIME_ID(conf));
    luaL_settabsb(L, "instrument", BITFIELD_TEST(mode, LMPROF_MODE_INSTRUMENT));
    luaL_settabsb(L, "memory", BITFIELD_TEST(mode, LMPROF_MODE_MEMORY));
    luaL_settabsb(L, "sample", BITFIELD_TEST(mode, LMPROF_MODE_SAMPLE));
    luaL_settabsb(L, "callback", BITFIELD_TEST(mode, LMPROF_CALLBACK_MASK));
    luaL_settabsb(L, "single_thread", BITFIELD_TEST(mode, LMPROF_MODE_SINGLE_THREAD));
    luaL_settabsb(L, "mismatch", BITFIELD_TEST(conf, LMPROF_OPT_STACK_MISMATCH));
    luaL_settabsb(L, "line_freq", BITFIELD_TEST(conf, LMPROF_OPT_LINE_FREQUENCY));
    luaL_settabsb(L, "compress_graph", BITFIELD_TEST(conf, LMPROF_OPT_COMPRESS_GRAPH));
    luaL_settabsi(L, "sampler_count", l_cast(lua_Integer, st->i.mask_count));
    luaL_settabsi(L, "instr_count", l_cast(lua_Integer, st->i.instr_count));
    luaL_settabsi(L, "profile_overhead", l_cast(lua_Integer, LMPROF_TIME_ADJ(st->thread.r.overhead, conf)));
    luaL_settabsi(L, "calibration", l_cast(lua_Integer, LMPROF_TIME_ADJ(st->i.calibration, conf)));
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;
    const char *indent = R->f.indent;
    LMPROF_PRINTF(f, "clockid = \"%s\"", indent, LMPROF_TIME_ID(conf));
    LMPROF_PRINTF(f, "instrument = %s", indent, BITFIELD_TEST(mode, LMPROF_MODE_INSTRUMENT) ? "true" : "false");
    LMPROF_PRINTF(f, "memory = %s", indent, BITFIELD_TEST(mode, LMPROF_MODE_MEMORY) ? "true" : "false");
    LMPROF_PRINTF(f, "sample = %s", indent, BITFIELD_TEST(mode, LMPROF_MODE_SAMPLE) ? "true" : "false");
    LMPROF_PRINTF(f, "callback = %s", indent, BITFIELD_TEST(mode, LMPROF_CALLBACK_MASK) ? "true" : "false");
    LMPROF_PRINTF(f, "single_thread = %s", indent, BITFIELD_TEST(mode, LMPROF_MODE_SINGLE_THREAD) ? "true" : "false");
    LMPROF_PRINTF(f, "mismatch = %s", indent, BITFIELD_TEST(conf, LMPROF_OPT_STACK_MISMATCH) ? "true" : "false");
    LMPROF_PRINTF(f, "line_freq = %s", indent, BITFIELD_TEST(conf, LMPROF_OPT_LINE_FREQUENCY) ? "true" : "false");
    LMPROF_PRINTF(f, "compress_graph = %s", indent, BITFIELD_TEST(conf, LMPROF_OPT_COMPRESS_GRAPH) ? "true" : "false");
    LMPROF_PRINTF(f, "sampler_count = " LUA_INTEGER_FMT, indent, l_cast(lua_Integer, st->i.mask_count));
    LMPROF_PRINTF(f, "instr_count = " LUA_INTEGER_FMT, indent, l_cast(lua_Integer, st->i.instr_count));
    LMPROF_PRINTF(f, "profile_overhead = " LUA_INTEGER_FMT, indent, l_cast(lua_Integer, LMPROF_TIME_ADJ(st->thread.r.overhead, conf)));
    LMPROF_PRINTF(f, "calibration = " LUA_INTEGER_FMT, indent, l_cast(lua_Integer, LMPROF_TIME_ADJ(st->i.calibration, conf)));
    return LUA_OK;
#else
    return LMPROF_REPORT_DISABLED_IO;
#endif
  }
  else if (R->type == lBuffer) {
    luaL_Buffer *b = &R->b.buff;
    const char *indent = R->b.indent;

    luaL_addifstring(L, b, "clockid = \"%s\"", indent, LMPROF_TIME_ID(conf));
    luaL_addifstring(L, b, "instrument = %s", indent, BITFIELD_TEST(mode, LMPROF_MODE_INSTRUMENT) ? "true" : "false");
    luaL_addifstring(L, b, "memory = %s", indent, BITFIELD_TEST(mode, LMPROF_MODE_MEMORY) ? "true" : "false");
    luaL_addifstring(L, b, "sample = %s", indent, BITFIELD_TEST(mode, LMPROF_MODE_SAMPLE) ? "true" : "false");
    luaL_addifstring(L, b, "callback = %s", indent, BITFIELD_TEST(mode, LMPROF_CALLBACK_MASK) ? "true" : "false");
    luaL_addifstring(L, b, "single_thread = %s", indent, BITFIELD_TEST(mode, LMPROF_MODE_SINGLE_THREAD) ? "true" : "false");
    luaL_addifstring(L, b, "mismatch = %s", indent, BITFIELD_TEST(conf, LMPROF_OPT_STACK_MISMATCH) ? "true" : "false");
    luaL_addifstring(L, b, "line_freq = %s", indent, BITFIELD_TEST(conf, LMPROF_OPT_LINE_FREQUENCY) ? "true" : "false");
    luaL_addifstring(L, b, "compress_graph = %s", indent, BITFIELD_TEST(conf, LMPROF_OPT_COMPRESS_GRAPH) ? "true" : "false");
    luaL_addifstring(L, b, "sampler_count = " LUA_INT_FORMAT, indent, LUA_INT_CAST(st->i.mask_count));
    luaL_addifstring(L, b, "instr_count = " LUA_INT_FORMAT, indent, LUA_INT_CAST(st->i.instr_count));
    luaL_addifstring(L, b, "profile_overhead = " LUA_INT_FORMAT, indent, LUA_INT_CAST(LMPROF_TIME_ADJ(st->thread.r.overhead, conf)));
    luaL_addifstring(L, b, "calibration = " LUA_INT_FORMAT, indent, LUA_INT_CAST(LMPROF_TIME_ADJ(st->i.calibration, conf)));
    return LUA_OK;
  }
  return LMPROF_REPORT_UNKNOWN_TYPE;
}

/*
** @TODO: Casting from uint64_t to lua_Integer creates potential overflow issues.
**
** @TODO: Technically PRIluSIZE and PRIluTIME should not be used as those format
**  strings may not be compatible with LUA_INTEGER_FMT (>= Lua 5.3) or
**  size_t/ptrdiff_t (<= Lua 5.2)
**
**  The equivalent to:
**    fprintf(f, LUA_INTEGER_FMT, (LUAI_UACINT)lua_tointeger(L, arg))
**  should be used/back-ported.
*/
static int graph_hash_callback(lua_State *L, lmprof_Record *record, void *args) {
  lmprof_Report *R = l_pcast(lmprof_Report *, args);
  lmprof_State *st = R->st;

  const uint32_t mode = st->mode;
  const lmprof_FunctionInfo *info = &record->info;
  luaL_checkstack(L, 8, __FUNCTION__);
  if (R->type == lTable) {
    char rid_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    char fid_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    char pid_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    if (snprintf(rid_str, sizeof(rid_str), "%" PRIluADDR "", record->r_id) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);
    if (snprintf(fid_str, sizeof(fid_str), "%" PRIluADDR "", record->f_id) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);
    if (snprintf(pid_str, sizeof(pid_str), "%" PRIluADDR "", record->p_id) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);

    /* Function header */
    lua_newtable(L);
    luaL_settabss(L, "id", rid_str);
    luaL_settabss(L, "func", fid_str);
    luaL_settabss(L, "parent", pid_str);
    luaL_settabsi(L, "parent_line", l_cast(lua_Integer, record->p_currentline));
    luaL_settabsb(L, "ignored", BITFIELD_TEST(info->event, LMPROF_RECORD_IGNORED));
    luaL_settabss(L, "name", (info->name == l_nullptr) ? LMPROF_RECORD_NAME_UNKNOWN : info->name);
    luaL_settabss(L, "what", (info->what == l_nullptr) ? LMPROF_RECORD_NAME_UNKNOWN : info->what);
    luaL_settabss(L, "source", (info->source == l_nullptr) ? LMPROF_RECORD_NAME_UNKNOWN : info->source);

    /* Function statistics */
    luaL_settabsi(L, "count", record->graph.count);
    if (BITFIELD_TEST(mode, LMPROF_MODE_INSTRUMENT)) {
      luaL_settabsi(L, "time", l_cast(lua_Integer, LMPROF_TIME_ADJ(record->graph.node.time, st->conf)));
      luaL_settabsi(L, "total_time", l_cast(lua_Integer, LMPROF_TIME_ADJ(record->graph.path.time, st->conf)));
    }

    if (BITFIELD_TEST(mode, LMPROF_MODE_MEMORY)) {
      luaL_settabsi(L, "allocated", l_cast(lua_Integer, record->graph.node.allocated));
      luaL_settabsi(L, "deallocated", l_cast(lua_Integer, record->graph.node.deallocated));
      luaL_settabsi(L, "total_allocated", l_cast(lua_Integer, record->graph.path.allocated));
      luaL_settabsi(L, "total_deallocated", l_cast(lua_Integer, record->graph.path.deallocated));
    }

    /* Spurious activation record data */
    luaL_settabsi(L, "linedefined", info->linedefined);
    luaL_settabsi(L, "lastlinedefined", info->lastlinedefined);
    luaL_settabsi(L, "nups", info->nups);
#if LUA_VERSION_NUM >= 502
    luaL_settabsi(L, "nparams", info->nparams);
    luaL_settabsb(L, "isvararg", info->isvararg);
    luaL_settabsb(L, "istailcall", info->istailcall);
#endif
#if LUA_VERSION_NUM >= 504
    luaL_settabsi(L, "ftransfer", info->ftransfer);
    luaL_settabsi(L, "ntransfer", info->ntransfer);
#endif

    /* Line profiling enabled */
    if (record->graph.line_freq != l_nullptr && record->graph.line_freq_size > 0) {
      const size_t *freq = record->graph.line_freq;
      const int freq_size = record->graph.line_freq_size;
      int i = 0;

      lua_createtable(L, freq_size, 0);
      for (i = 0; i < freq_size; ++i) {
        lua_pushinteger(L, l_cast(lua_Integer, freq[i]));
#if LUA_VERSION_NUM >= 503
        lua_rawseti(L, -2, l_cast(lua_Integer, i + 1));
#else
        lua_rawseti(L, -2, i + 1);
#endif
      }
      lua_setfield(L, -2, "lines");
    }
    lua_rawseti(L, -2, R->t.array_count++); /* TABLE: APPEND */
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;
    const char *indent = R->f.indent;
    /*
    ** Output as an array of records. The 'map' equivalent has keys of the form
    **  ("%s_%s"):format(funcNode.func, funcNode.parent)
    **
    ** fprintf(f, "%s[\"%" PRIluADDR "_%" PRIluADDR "\"] = {" LMPROF_NL, indent, record->f_id, record->p_id);
    */
    fprintf(f, "%s{" LMPROF_NL, indent);

    /* Function header */
    LMPROF_PRINTF(f, "id = \"%" PRIluADDR "\"", indent, record->r_id);
    LMPROF_PRINTF(f, "func = \"%" PRIluADDR "\"", indent, record->f_id);
    LMPROF_PRINTF(f, "parent = \"%" PRIluADDR "\"", indent, record->p_id);
    LMPROF_PRINTF(f, "parent_line = %d", indent, record->p_currentline);
    LMPROF_PRINTF(f, "ignored = %s", indent, BITFIELD_TEST(info->event, LMPROF_RECORD_IGNORED) ? "true" : "false");
    LMPROF_PRINTF(f, "name = \"%s\"", indent, (info->name == l_nullptr) ? LMPROF_RECORD_NAME_UNKNOWN : info->name);
    LMPROF_PRINTF(f, "what = \"%s\"", indent, (info->what == l_nullptr) ? LMPROF_RECORD_NAME_UNKNOWN : info->what);
    LMPROF_PRINTF(f, "source = \"%s\"", indent, (info->source == l_nullptr) ? LMPROF_RECORD_NAME_UNKNOWN : info->source);

    /* Function statistics */
    LMPROF_PRINTF(f, "count = %zu", indent, record->graph.count);
    if (BITFIELD_TEST(mode, LMPROF_MODE_INSTRUMENT)) {
      LMPROF_PRINTF(f, "time = %" PRIluTIME "", indent, LMPROF_TIME_ADJ(record->graph.node.time, st->conf));
      LMPROF_PRINTF(f, "total_time = %" PRIluTIME "", indent, LMPROF_TIME_ADJ(record->graph.path.time, st->conf));
    }

    if (BITFIELD_TEST(mode, LMPROF_MODE_MEMORY)) {
      LMPROF_PRINTF(f, "allocated = %" PRIluSIZE "", indent, record->graph.node.allocated);
      LMPROF_PRINTF(f, "deallocated = %" PRIluSIZE "", indent, record->graph.node.deallocated);
      LMPROF_PRINTF(f, "total_allocated = %" PRIluSIZE "", indent, record->graph.path.allocated);
      LMPROF_PRINTF(f, "total_deallocated = %" PRIluSIZE "", indent, record->graph.path.deallocated);
    }

    /* Spurious activation record data */
    LMPROF_PRINTF(f, "linedefined = %d", indent, info->linedefined);
    LMPROF_PRINTF(f, "lastlinedefined = %d", indent, info->lastlinedefined);
    LMPROF_PRINTF(f, "nups = %d", indent, info->nups);
  #if LUA_VERSION_NUM >= 502
    LMPROF_PRINTF(f, "nparams = %d", indent, info->nparams);
    LMPROF_PRINTF(f, "isvararg = %d", indent, info->isvararg);
    LMPROF_PRINTF(f, "istailcall = %d", indent, info->istailcall);
  #endif
  #if LUA_VERSION_NUM >= 504
    LMPROF_PRINTF(f, "ftransfer = %d", indent, info->ftransfer);
    LMPROF_PRINTF(f, "ntransfer = %d", indent, info->ntransfer);
  #endif

    /* Line profiling enabled */
    if (record->graph.line_freq != l_nullptr && record->graph.line_freq_size > 0) {
      const size_t *freq = record->graph.line_freq;
      const int freq_size = record->graph.line_freq_size;
      int i = 0;

      fprintf(f, "%s" LMPROF_INDENT "lines = {", indent);
      fprintf(f, "%zu", freq[0]);
      for (i = 1; i < freq_size; ++i)
        fprintf(f, ", %zu", freq[i]);
      fprintf(f, "}," LMPROF_NL);
    }
    fprintf(f, "%s}," LMPROF_NL, indent);
    return LUA_OK;
#else
    return LMPROF_REPORT_DISABLED_IO;
#endif
  }
  else if (R->type == lBuffer) {
    luaL_Buffer *b = &R->b.buff;
    const char *indent = R->b.indent;

    char rid_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    char fid_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    char pid_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    if (snprintf(rid_str, sizeof(rid_str), "%" PRIluADDR "", record->r_id) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);
    if (snprintf(fid_str, sizeof(fid_str), "%" PRIluADDR "", record->f_id) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);
    if (snprintf(pid_str, sizeof(pid_str), "%" PRIluADDR "", record->p_id) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);

    luaL_addfstring(L, b, "%s{" LMPROF_NL, indent);

    /* Function header */
    luaL_addifstring(L, b, "id = \"%s\"", indent, rid_str);
    luaL_addifstring(L, b, "func = \"%s\"", indent, fid_str);
    luaL_addifstring(L, b, "parent = \"%s\"", indent, pid_str);
    luaL_addifstring(L, b, "parent_line = %d", indent, record->p_currentline);
    luaL_addifstring(L, b, "ignored = %s", indent, BITFIELD_TEST(info->event, LMPROF_RECORD_IGNORED) ? "true" : "false");
    luaL_addifstring(L, b, "name = \"%s\"", indent, (info->name == l_nullptr) ? LMPROF_RECORD_NAME_UNKNOWN : info->name);
    luaL_addifstring(L, b, "what = \"%s\"", indent, (info->what == l_nullptr) ? LMPROF_RECORD_NAME_UNKNOWN : info->what);
    luaL_addifstring(L, b, "source = \"%s\"", indent, (info->source == l_nullptr) ? LMPROF_RECORD_NAME_UNKNOWN : info->source);

    /* Function statistics */
    luaL_addifstring(L, b, "count = " LUA_UNIT_FORMAT "", indent, LUA_UNIT_CAST(record->graph.count));
    if (BITFIELD_TEST(mode, LMPROF_MODE_INSTRUMENT)) {
      luaL_addifstring(L, b, "time = " LUA_UNIT_FORMAT "", indent, LUA_UNIT_CAST(LMPROF_TIME_ADJ(record->graph.node.time, st->conf)));
      luaL_addifstring(L, b, "total_time = " LUA_UNIT_FORMAT "", indent, LUA_UNIT_CAST(LMPROF_TIME_ADJ(record->graph.path.time, st->conf)));
    }

    if (BITFIELD_TEST(mode, LMPROF_MODE_MEMORY)) {
      luaL_addifstring(L, b, "allocated = " LUA_UNIT_FORMAT "", indent, LUA_UNIT_CAST(record->graph.node.allocated));
      luaL_addifstring(L, b, "deallocated = " LUA_UNIT_FORMAT "", indent, LUA_UNIT_CAST(record->graph.node.deallocated));
      luaL_addifstring(L, b, "total_allocated = " LUA_UNIT_FORMAT "", indent, LUA_UNIT_CAST(record->graph.path.allocated));
      luaL_addifstring(L, b, "total_deallocated = " LUA_UNIT_FORMAT "", indent, LUA_UNIT_CAST(record->graph.path.deallocated));
    }

    /* Spurious activation record data */
    luaL_addifstring(L, b, "linedefined = %d", indent, info->linedefined);
    luaL_addifstring(L, b, "lastlinedefined = %d", indent, info->lastlinedefined);
    luaL_addifstring(L, b, "nups = %d", indent, info->nups);
#if LUA_VERSION_NUM >= 502
    luaL_addifstring(L, b, "nparams = %d", indent, info->nparams);
    luaL_addifstring(L, b, "isvararg = %d", indent, info->isvararg);
    luaL_addifstring(L, b, "istailcall = %d", indent, info->istailcall);
#endif
#if LUA_VERSION_NUM >= 504
    luaL_addifstring(L, b, "ftransfer = %d", indent, info->ftransfer);
    luaL_addifstring(L, b, "ntransfer = %d", indent, info->ntransfer);
#endif

    /* Line profiling enabled */
    if (record->graph.line_freq != l_nullptr && record->graph.line_freq_size > 0) {
      const size_t *freq = record->graph.line_freq;
      const int freq_size = record->graph.line_freq_size;
      int i = 0;

      luaL_addfstring(L, b, "%s" LMPROF_INDENT "lines = {", indent);
      luaL_addfstring(L, b, "%zu", freq[0]);
      for (i = 1; i < freq_size; ++i)
        luaL_addfstring(L, b, ", %zu", freq[i]);
      luaL_addfstring(L, b, "}," LMPROF_NL);
    }
    luaL_addfstring(L, b, "%s}," LMPROF_NL, indent);
    return LUA_OK;
  }
  return LMPROF_REPORT_UNKNOWN_TYPE;
}

static int graph_report(lua_State *L, lmprof_Report *report) {
  lmprof_State *st = report->st;

  luaL_checkstack(L, 8, __FUNCTION__);
  if (report->type == lTable) {
    lua_newtable(L); /* [..., header] */
    profiler_header(L, report);
    if (BITFIELD_TEST(st->conf, LMPROF_OPT_REPORT_VERBOSE) && !BITFIELD_TEST(st->mode, LMPROF_CALLBACK_MASK)) {
      lua_newtable(L); /* [..., hash_debug] */
      lmprof_hash_debug(L, st->i.hash);
      lua_setfield(L, -2, "debug");
    }
    lua_setfield(L, report->t.table_index, "header");

    lua_newtable(L); /* [..., records] */
    lmprof_hash_report(L, st->i.hash, (lmprof_hash_Callback)graph_hash_callback, l_pcast(const void *, report));
    lua_setfield(L, report->t.table_index, "records");
    return LUA_OK;
  }
  else if (report->type == lFile) {
#if defined(LMPROF_FILE_API)
    /* Header */
    report->f.indent = LMPROF_INDENT;
    fprintf(report->f.file, "return {" LMPROF_NL);
    fprintf(report->f.file, LMPROF_INDENT "header = {" LMPROF_NL);
    profiler_header(L, report);
    fprintf(report->f.file, LMPROF_INDENT "}," LMPROF_NL);

    /* Profile Records */
    report->f.indent = LMPROF_INDENT LMPROF_INDENT;
    fprintf(report->f.file, LMPROF_INDENT "records = {" LMPROF_NL);
    lmprof_hash_report(L, st->i.hash, (lmprof_hash_Callback)graph_hash_callback, l_pcast(const void *, report));
    fprintf(report->f.file, LMPROF_INDENT "}" LMPROF_NL "}" LMPROF_NL);
    return LUA_OK;
#else
    return LMPROF_REPORT_DISABLED_IO;
#endif
  }
  else if (report->type == lBuffer) {
    luaL_Buffer *b = &report->b.buff;

    /* Header */
    report->b.indent = LMPROF_INDENT;
    luaL_addliteral(b, "return {" LMPROF_NL);
    luaL_addliteral(b, LMPROF_INDENT "header = {" LMPROF_NL);
    profiler_header(L, report);
    luaL_addliteral(b, LMPROF_INDENT "}," LMPROF_NL);

    /* Profile Records */
    report->b.indent = LMPROF_INDENT LMPROF_INDENT;
    luaL_addliteral(b, LMPROF_INDENT "records = {" LMPROF_NL);
    lmprof_hash_report(L, st->i.hash, (lmprof_hash_Callback)graph_hash_callback, l_pcast(const void *, report));
    luaL_addliteral(b, LMPROF_INDENT "}" LMPROF_NL "}" LMPROF_NL);
    return LUA_OK;
  }

  return LMPROF_REPORT_FAILURE;
}

/* }================================================================== */

/*
** {==================================================================
** Trace Event Formatting
** ===================================================================
*/

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

#define CHROME_USER_TIMING "blink.user_timing"
#define CHROME_TIMLINE "disabled-by-default-devtools.timeline"
#define CHROME_TIMELINE_FRAME "disabled-by-default-devtools.timeline.frame"

#define CHROME_OPT_NAME(n, o) (((n) == l_nullptr) ? (o) : (n))
#define CHROME_EVENT_NAME(E) CHROME_OPT_NAME((E)->data.event.info->source, LMPROF_RECORD_NAME_UNKNOWN)

#define JSON_OPEN_OBJ "{"
#define JSON_CLOSE_OBJ "}"
#define JSON_OPEN_ARRAY "["
#define JSON_CLOSE_ARRAY "]"
#define JSON_DELIM ", "
#define JSON_NEWLINE "\n"
#define JSON_STRING(S) "\"" S "\""
#define JSON_ASSIGN(K, V) "\"" K "\":" V

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

/* MetaEvents */
static int __metaProcess(lua_State *L, lmprof_Report *R, const lmprof_EventProcess *process, const char *name, const char *pname);
static int __metaTracingStarted(lua_State *L, lmprof_Report *R, const lmprof_EventProcess *process, const char *name, const char *url);

/* chrome://tracing/ metadata reporting/ */
static int __metaAbout(lua_State *L, lmprof_Report *R, const char *name, const char *url);

/* BEGIN_FRAME */
static int __enterFrame(lua_State *L, lmprof_Report *R, const TraceEvent *event);

/* END_FRAME */
static int __exitFrame(lua_State *L, lmprof_Report *R, const TraceEvent *event);
static int __drawFrame(lua_State *L, lmprof_Report *R, const TraceEvent *event);

/* BEGIN_ROUTINE/END_ROUTINE ENTER_SCOPE/EXIT_SCOPE */
static int __eventScope(lua_State *L, lmprof_Report *R, const TraceEvent *event, const char *name, const char *eventName);
static int __eventUpdateCounters(lua_State *L, lmprof_Report *R, const TraceEvent *event);
static int __eventLineInstance(lua_State *L, lmprof_Report *R, const TraceEvent *event);
static int __eventSampleInstance(lua_State *L, lmprof_Report *R, const TraceEvent *event);

static const char *__threadName(lua_State *L, lmprof_Report *R, TraceEvent *event) {
  const char *opt = CHROME_META_TICK;
  if (event->call.proc.tid == R->st->thread.mainproc.tid)
    opt = CHROME_NAME_MAIN;

  return lmprof_thread_name(L, event->call.proc.tid, opt);
}

static int __metaProcess(lua_State *L, lmprof_Report *R, const lmprof_EventProcess *process, const char *name, const char *pname) {
  if (R->type == lTable) {
    lua_newtable(L); /* [..., header] */
    luaL_settabss(L, "cat", "__metadata");
    luaL_settabss(L, "name", name);
    luaL_settabss(L, "ph", "M");
    luaL_settabsi(L, "ts", 0);
    luaL_settabsi(L, "pid", process->pid);
    luaL_settabsi(L, "tid", process->tid);

    lua_newtable(L); /* [..., header, args] */
    luaL_settabss(L, "name", pname);
    lua_setfield(L, -2, "args"); /* [..., header] */
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;

    REPORT_ENSURE_FILE_DELIM(R);
    fprintf(f, JSON_OPEN_OBJ);
    fprintf(f, JSON_ASSIGN("cat", JSON_STRING("__metadata")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("%s")), name);
    fprintf(f, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("M")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("ts", "0"));
    fprintf(f, JSON_DELIM JSON_ASSIGN("pid", LUA_INTEGER_FMT), process->pid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), process->tid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("args", ""));
    fprintf(f, JSON_OPEN_OBJ JSON_ASSIGN("name", JSON_STRING("%s")) JSON_CLOSE_OBJ, pname);
    fprintf(f, JSON_CLOSE_OBJ);
    R->f.delim = 1;
    return LUA_OK;
#else
    return LMPROF_REPORT_DISABLED_IO;
#endif
  }
  else if (R->type == lBuffer) {
    luaL_Buffer *b = &R->b.buff;

    REPORT_ENSURE_BUFFER_DELIM(R);
    luaL_addliteral(b, JSON_OPEN_OBJ);
    luaL_addliteral(b, JSON_ASSIGN("cat", JSON_STRING("__metadata")));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("%s")), name);
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("M")));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("ts", "0"));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("pid", LUA_INT_FORMAT), LUA_INT_CAST(process->pid));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), LUA_INT_CAST(process->tid));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("args", ""));
    luaL_addfstring(L, b, JSON_OPEN_OBJ JSON_ASSIGN("name", JSON_STRING("%s")) JSON_CLOSE_OBJ, pname);
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    R->b.delim = 1;
    return LUA_OK;
  }

  return LMPROF_REPORT_UNKNOWN_TYPE;
}

static int __metaAbout(lua_State *L, lmprof_Report *R, const char *name, const char *url) {
  if (R->type == lTable) {
    lua_newtable(L);
    luaL_settabsi(L, "bitness", 64);
    luaL_settabss(L, "domain", "WIN_QPC");
    luaL_settabss(L, "command_line", "");
    luaL_settabsi(L, "highres-ticks", 1);
    luaL_settabsi(L, "physical-memory", 0);
    luaL_settabss(L, "user-agent", name);
    luaL_settabss(L, "command_line", url);
    luaL_settabss(L, "v8-version", LUA_VERSION);
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;

    REPORT_ENSURE_FILE_DELIM(R);
    fprintf(f, JSON_ASSIGN("metadata", JSON_OPEN_OBJ JSON_NEWLINE));
    fprintf(f, JSON_ASSIGN("bitness", "64") JSON_DELIM JSON_NEWLINE);
    fprintf(f, JSON_ASSIGN("domain", JSON_STRING("WIN_QPC")) JSON_DELIM JSON_NEWLINE);
    fprintf(f, JSON_ASSIGN("command_line", JSON_STRING("")) JSON_DELIM JSON_NEWLINE);
    fprintf(f, JSON_ASSIGN("highres-ticks", "1") JSON_DELIM JSON_NEWLINE);
    fprintf(f, JSON_ASSIGN("physical-memory", "0") JSON_DELIM JSON_NEWLINE);
    fprintf(f, JSON_ASSIGN("user-agent", JSON_STRING("%s")) JSON_DELIM JSON_NEWLINE, name);
    fprintf(f, JSON_ASSIGN("command_line", JSON_STRING("%s")) JSON_DELIM JSON_NEWLINE, url);
    fprintf(f, JSON_ASSIGN("v8-version", JSON_STRING(LUA_VERSION)) JSON_NEWLINE);
    fprintf(f, JSON_CLOSE_OBJ);
    R->f.delim = 1;
    return LUA_OK;
#else
    return LMPROF_REPORT_DISABLED_IO;
#endif
  }
  else if (R->type == lBuffer) {
    luaL_Buffer *b = &R->b.buff;

    REPORT_ENSURE_BUFFER_DELIM(R);
    luaL_addliteral(b, JSON_ASSIGN("metadata", JSON_OPEN_OBJ JSON_NEWLINE));
    luaL_addliteral(b, JSON_ASSIGN("bitness", "64") JSON_DELIM JSON_NEWLINE);
    luaL_addliteral(b, JSON_ASSIGN("domain", JSON_STRING("WIN_QPC")) JSON_DELIM JSON_NEWLINE);
    luaL_addliteral(b, JSON_ASSIGN("command_line", JSON_STRING("")) JSON_DELIM JSON_NEWLINE);
    luaL_addliteral(b, JSON_ASSIGN("highres-ticks", "1") JSON_DELIM JSON_NEWLINE);
    luaL_addliteral(b, JSON_ASSIGN("physical-memory", "0") JSON_DELIM JSON_NEWLINE);
    luaL_addfstring(L, b, JSON_ASSIGN("user-agent", JSON_STRING("%s")) JSON_DELIM JSON_NEWLINE, name);
    luaL_addfstring(L, b, JSON_ASSIGN("command_line", JSON_STRING("%s")) JSON_DELIM JSON_NEWLINE, url);
    luaL_addliteral(b, JSON_ASSIGN("v8-version", JSON_STRING(LUA_VERSION)) JSON_NEWLINE);
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    R->b.delim = 1;
    return LUA_OK;
  }
  return LMPROF_REPORT_UNKNOWN_TYPE;
}

static int __metaTracingStarted(lua_State *L, lmprof_Report *R, const lmprof_EventProcess *process, const char *name, const char *url) {
  if (R->type == lTable) {
    lua_newtable(L); /* [..., header] */
    luaL_settabss(L, "cat", CHROME_TIMLINE);
    luaL_settabss(L, "name", "TracingStartedInBrowser");
    luaL_settabss(L, "ph", "I");
    luaL_settabsi(L, "pid", process->pid);
    luaL_settabsi(L, "tid", process->tid);
    luaL_settabsi(L, "ts", 0);

    lua_newtable(L); /* [..., header, args] */
    lua_newtable(L); /* [..., header, args, data] */
    luaL_settabsi(L, "frameTreeNodeId", 1);
    luaL_settabsb(L, "persistentIds", 1);

    lua_newtable(L); /* [..., header, args, data, frames] */
    lua_newtable(L); /* [..., header, args, data, frames, frames_array] */
    luaL_settabss(L, "frame", "FADE");
    luaL_settabss(L, "url", CHROME_OPT_NAME(url, TRACE_EVENT_DEFAULT_URL));
    luaL_settabss(L, "name", CHROME_OPT_NAME(name, TRACE_EVENT_DEFAULT_NAME));
    luaL_settabsi(L, "processId", process->pid);
    lua_rawseti(L, -2, 1); /* [..., header, args, data, frames] */
    lua_setfield(L, -2, "frames"); /* [..., header, args, data] */
    lua_setfield(L, -2, "data"); /* [..., header, args] */
    lua_setfield(L, -2, "args"); /* [..., header] */
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;

    REPORT_ENSURE_FILE_DELIM(R);
    fprintf(f, JSON_OPEN_OBJ);
    fprintf(f, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMLINE)));
    fprintf(f, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("TracingStartedInBrowser")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("I")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("pid", LUA_INTEGER_FMT), process->pid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), process->tid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("ts", "0"));
    fprintf(f, JSON_DELIM JSON_ASSIGN("args", JSON_OPEN_OBJ));
    fprintf(f, JSON_ASSIGN("data", JSON_OPEN_OBJ));
    fprintf(f, JSON_ASSIGN("frameTreeNodeId", "1"));
    fprintf(f, JSON_DELIM JSON_ASSIGN("persistentIds", "true"));
    fprintf(f, JSON_DELIM JSON_ASSIGN("frames", JSON_OPEN_ARRAY JSON_OPEN_OBJ));
    fprintf(f, JSON_ASSIGN("frame", JSON_STRING("FADE")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("url", JSON_STRING("%s")), CHROME_OPT_NAME(url, TRACE_EVENT_DEFAULT_URL));
    fprintf(f, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("%s")), CHROME_OPT_NAME(name, TRACE_EVENT_DEFAULT_NAME));
    fprintf(f, JSON_DELIM JSON_ASSIGN("processId", LUA_INTEGER_FMT), process->pid);
    fprintf(f, JSON_CLOSE_OBJ JSON_CLOSE_ARRAY);
    fprintf(f, JSON_CLOSE_OBJ);
    fprintf(f, JSON_CLOSE_OBJ);
    fprintf(f, JSON_CLOSE_OBJ);
    R->f.delim = 1;
    return LUA_OK;
#endif
  }
  else if (R->type == lBuffer) {
    luaL_Buffer *b = &R->b.buff;

    REPORT_ENSURE_BUFFER_DELIM(R);
    luaL_addliteral(b, JSON_OPEN_OBJ);
    luaL_addliteral(b, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMLINE)));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("TracingStartedInBrowser")));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("I")));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("pid", LUA_INT_FORMAT), LUA_INT_CAST(process->pid));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), LUA_INT_CAST(process->tid));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("ts", "0"));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("args", JSON_OPEN_OBJ));
    luaL_addliteral(b, JSON_ASSIGN("data", JSON_OPEN_OBJ));
    luaL_addliteral(b, JSON_ASSIGN("frameTreeNodeId", "1"));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("persistentIds", "true"));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("frames", JSON_OPEN_ARRAY JSON_OPEN_OBJ));
    luaL_addliteral(b, JSON_ASSIGN("frame", JSON_STRING("FADE")));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("url", JSON_STRING("%s")), CHROME_OPT_NAME(url, TRACE_EVENT_DEFAULT_URL));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("%s")), CHROME_OPT_NAME(name, TRACE_EVENT_DEFAULT_NAME));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("processId", LUA_INT_FORMAT), LUA_INT_CAST(process->pid));
    luaL_addliteral(b, JSON_CLOSE_OBJ JSON_CLOSE_ARRAY);
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    R->b.delim = 1;
    return LUA_OK;
  }
  return LMPROF_REPORT_UNKNOWN_TYPE;
}

static int __enterFrame(lua_State *L, lmprof_Report *R, const TraceEvent *event) {
  if (R->type == lTable) {
    lua_newtable(L);
    luaL_settabss(L, "cat", CHROME_TIMELINE_FRAME);
    luaL_settabss(L, "name", "BeginFrame");
    luaL_settabss(L, "s", "t");
    luaL_settabss(L, "ph", "I");
    luaL_settabsi(L, "ts", l_cast(lua_Integer, LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)));
    luaL_settabsi(L, "pid", event->call.proc.pid);
    luaL_settabsi(L, "tid", event->call.proc.tid);
    /* layerTreeId = NULL */
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;

    REPORT_ENSURE_FILE_DELIM(R);
    fprintf(f, JSON_OPEN_OBJ);
    fprintf(f, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMELINE_FRAME)));
    fprintf(f, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("BeginFrame")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("s", JSON_STRING("t")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("I")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("ts", "%" PRIluTIME ""), LMPROF_TIME_ADJ(event->call.s.time, R->st->conf));
    fprintf(f, JSON_DELIM JSON_ASSIGN("pid", LUA_INTEGER_FMT), event->call.proc.pid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), event->call.proc.tid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("args", ""));
    fprintf(f, JSON_OPEN_OBJ JSON_ASSIGN("layerTreeId", "null") " }");
    fprintf(f, JSON_CLOSE_OBJ);
    R->f.delim = 1;
    return LUA_OK;
#endif
  }
  else if (R->type == lBuffer) {
    luaL_Buffer *b = &R->b.buff;
    char ts_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    if (snprintf(ts_str, sizeof(ts_str), "%" PRIluTIME "", LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);

    REPORT_ENSURE_BUFFER_DELIM(R);
    luaL_addliteral(b, JSON_OPEN_OBJ);
    luaL_addliteral(b, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMELINE_FRAME)));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("BeginFrame")));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("s", JSON_STRING("t")));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("I")));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("ts", "%s"), ts_str);
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("pid", LUA_INT_FORMAT), LUA_INT_CAST(event->call.proc.pid));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), LUA_INT_CAST(event->call.proc.tid));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("args", ""));
    luaL_addliteral(b, JSON_OPEN_OBJ JSON_ASSIGN("layerTreeId", "null") " }");
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    R->b.delim = 1;
    return LUA_OK;
  }
  return LMPROF_REPORT_UNKNOWN_TYPE;
}

static int __exitFrame(lua_State *L, lmprof_Report *R, const TraceEvent *event) {
  if (R->type == lTable) {
    lua_newtable(L); /* [..., exit_tab] */
    luaL_settabss(L, "cat", CHROME_TIMELINE_FRAME);
    luaL_settabss(L, "name", "ActivateLayerTree");
    luaL_settabss(L, "s", "t");
    luaL_settabss(L, "ph", "I");
    luaL_settabsi(L, "ts", l_cast(lua_Integer, LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)));
    luaL_settabsi(L, "pid", event->call.proc.pid);
    luaL_settabsi(L, "tid", event->call.proc.tid);

    lua_newtable(L); /* [..., exit_tab, args] */
    luaL_settabsi(L, "frameId", l_cast(lua_Integer, event->data.frame.frame));
    lua_setfield(L, -2, "args"); /* [..., exit_tab] */
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;

    REPORT_ENSURE_FILE_DELIM(R);
    fprintf(f, JSON_OPEN_OBJ);
    fprintf(f, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMELINE_FRAME)));
    fprintf(f, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("ActivateLayerTree")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("s", JSON_STRING("t")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("I")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("ts", "%" PRIluTIME ""), LMPROF_TIME_ADJ(event->call.s.time, R->st->conf));
    fprintf(f, JSON_DELIM JSON_ASSIGN("pid", LUA_INTEGER_FMT), event->call.proc.pid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), event->call.proc.tid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("args", ""));
    fprintf(f, JSON_OPEN_OBJ);
    fprintf(f, JSON_ASSIGN("frameId", LUA_INTEGER_FMT), l_cast(lua_Integer, event->data.frame.frame));
    fprintf(f, JSON_DELIM JSON_ASSIGN("layerTreeId", "null"));
    fprintf(f, JSON_CLOSE_OBJ);
    fprintf(f, JSON_CLOSE_OBJ);
    R->f.delim = 1;
    return LUA_OK;
#endif
  }
  else if (R->type == lBuffer) {
    luaL_Buffer *b = &R->b.buff;
    char ts_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    if (snprintf(ts_str, sizeof(ts_str), "%" PRIluTIME "", LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);

    REPORT_ENSURE_BUFFER_DELIM(R);
    luaL_addliteral(b, JSON_OPEN_OBJ);
    luaL_addliteral(b, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMELINE_FRAME)));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("ActivateLayerTree")));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("s", JSON_STRING("t")));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("I")));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("ts", "%s"), ts_str);
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("pid", LUA_INT_FORMAT), LUA_INT_CAST(event->call.proc.pid));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), LUA_INT_CAST(event->call.proc.tid));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("args", ""));
    luaL_addliteral(b, JSON_OPEN_OBJ);
    luaL_addfstring(L, b, JSON_ASSIGN("frameId", LUA_INT_FORMAT), LUA_INT_CAST(event->data.frame.frame));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("layerTreeId", "null"));
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    R->b.delim = 1;
    return LUA_OK;
  }
  return LMPROF_REPORT_UNKNOWN_TYPE;
}

/* Per TimelineFrameModel.ts: "Legacy behavior: If DrawFrame is an instant event..." */
static int __drawFrame(lua_State *L, lmprof_Report *R, const TraceEvent *event) {
  if (R->type == lTable) {
    lua_newtable(L);
    luaL_settabss(L, "cat", CHROME_TIMELINE_FRAME);
    luaL_settabss(L, "name", "DrawFrame");
    luaL_settabss(L, "s", "t");
    luaL_settabss(L, "ph", "I");
    luaL_settabsi(L, "ts", l_cast(lua_Integer, LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)));
    luaL_settabsi(L, "pid", event->call.proc.pid);
    luaL_settabsi(L, "tid", event->call.proc.tid);
    /* layerTreeId = NULL */
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;

    REPORT_ENSURE_FILE_DELIM(R);
    fprintf(f, JSON_OPEN_OBJ);
    fprintf(f, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMELINE_FRAME)));
    fprintf(f, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("DrawFrame")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("s", JSON_STRING("t")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("I")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("ts", "%" PRIluTIME ""), LMPROF_TIME_ADJ(event->call.s.time, R->st->conf));
    fprintf(f, JSON_DELIM JSON_ASSIGN("pid", LUA_INTEGER_FMT), event->call.proc.pid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), event->call.proc.tid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("args", ""));
    fprintf(f, JSON_OPEN_OBJ JSON_ASSIGN("layerTreeId", "null") " " JSON_CLOSE_OBJ);
    fprintf(f, JSON_CLOSE_OBJ);
    R->f.delim = 1;
    return LUA_OK;
#endif
  }
  else if (R->type == lBuffer) {
    luaL_Buffer *b = &R->b.buff;
    char ts_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    if (snprintf(ts_str, sizeof(ts_str), "%" PRIluTIME "", LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);

    REPORT_ENSURE_BUFFER_DELIM(R);
    luaL_addliteral(b, JSON_OPEN_OBJ);
    luaL_addliteral(b, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMELINE_FRAME)));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("DrawFrame")));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("s", JSON_STRING("t")));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("I")));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("ts", "%s"), ts_str);
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("pid", LUA_INT_FORMAT), LUA_INT_CAST(event->call.proc.pid));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), LUA_INT_CAST(event->call.proc.tid));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("args", ""));
    luaL_addliteral(b, JSON_OPEN_OBJ JSON_ASSIGN("layerTreeId", "null") " " JSON_CLOSE_OBJ);
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    R->b.delim = 1;
    return LUA_OK;
  }
  return LMPROF_REPORT_UNKNOWN_TYPE;
}

static int __eventScope(lua_State *L, lmprof_Report *R, const TraceEvent *event, const char *name, const char *eventName) {
  if (R->type == lTable) {
    lua_newtable(L);
    luaL_settabss(L, "cat", CHROME_USER_TIMING);
    luaL_settabss(L, "name", eventName);
    luaL_settabss(L, "ph", name);
    luaL_settabsi(L, "pid", event->call.proc.pid);
    if (op_routine(event->op))
      luaL_settabsi(L, "tid", R->st->thread.mainproc.tid);
    else
      luaL_settabsi(L, "tid", event->call.proc.tid);
    luaL_settabsi(L, "ts", l_cast(lua_Integer, LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)));
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;

    REPORT_ENSURE_FILE_DELIM(R);
    fprintf(f, JSON_OPEN_OBJ);
    fprintf(f, JSON_ASSIGN("cat", JSON_STRING(CHROME_USER_TIMING)));
    fprintf(f, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("%s")), eventName);
    fprintf(f, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("%s")), name);
    fprintf(f, JSON_DELIM JSON_ASSIGN("pid", LUA_INTEGER_FMT), event->call.proc.pid);
    if (op_routine(event->op))
      fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), R->st->thread.mainproc.tid);
    else
      fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), event->call.proc.tid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("ts", "%" PRIluTIME ""), LMPROF_TIME_ADJ(event->call.s.time, R->st->conf));
    fprintf(f, JSON_CLOSE_OBJ);
    R->f.delim = 1;
    return LUA_OK;
#else
    return LMPROF_REPORT_DISABLED_IO;
#endif
  }
  else if (R->type == lBuffer) {
    luaL_Buffer *b = &R->b.buff;
    char ts_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    if (snprintf(ts_str, sizeof(ts_str), "%" PRIluTIME "", LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);

    REPORT_ENSURE_BUFFER_DELIM(R);
    luaL_addliteral(b, JSON_OPEN_OBJ);
    luaL_addliteral(b, JSON_ASSIGN("cat", JSON_STRING(CHROME_USER_TIMING)));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("%s")), eventName);
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("%s")), name);
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("pid", LUA_INT_FORMAT), LUA_INT_CAST(event->call.proc.pid));
    if (op_routine(event->op))
      luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), LUA_INT_CAST(R->st->thread.mainproc.tid));
    else
      luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), LUA_INT_CAST(event->call.proc.tid));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("ts", "%s"), ts_str);
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    R->b.delim = 1;
    return LUA_OK;
  }
  return LMPROF_REPORT_UNKNOWN_TYPE;
}

static int __eventLineInstance(lua_State *L, lmprof_Report *R, const TraceEvent *event) {
  if (R->type == lTable) {
    lua_newtable(L);
    luaL_settabss(L, "cat", CHROME_USER_TIMING);
    luaL_settabss(L, "ph", "I");
    luaL_settabss(L, "s", "t");
    luaL_settabsi(L, "pid", event->call.proc.pid);
    luaL_settabsi(L, "tid", event->call.proc.tid);
    luaL_settabsi(L, "ts", l_cast(lua_Integer, LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)));
    lua_pushfstring(L, "%s: Line %d\"", event->data.line.info->source, event->data.line.line);
    lua_setfield(L, -2, "name");
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;

    REPORT_ENSURE_FILE_DELIM(R);
    fprintf(f, JSON_OPEN_OBJ);
    fprintf(f, JSON_ASSIGN("cat", JSON_STRING(CHROME_USER_TIMING)));
    fprintf(f, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("%s: Line %d")), event->data.line.info->source, event->data.line.line);
    fprintf(f, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("I")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("s", JSON_STRING("t")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("ts", "%" PRIluTIME ""), LMPROF_TIME_ADJ(event->call.s.time, R->st->conf));
    fprintf(f, JSON_DELIM JSON_ASSIGN("pid", LUA_INTEGER_FMT), event->call.proc.pid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), event->call.proc.tid);
    fprintf(f, JSON_CLOSE_OBJ);
    R->f.delim = 1;
    return LUA_OK;
#else
    return LMPROF_REPORT_DISABLED_IO;
#endif
  }
  else if (R->type == lBuffer) {
    luaL_Buffer *b = &R->b.buff;
    char ts_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    if (snprintf(ts_str, sizeof(ts_str), "%" PRIluTIME "", LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);

    REPORT_ENSURE_BUFFER_DELIM(R);
    luaL_addliteral(b, JSON_OPEN_OBJ);
    luaL_addliteral(b, JSON_ASSIGN("cat", JSON_STRING(CHROME_USER_TIMING)));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("%s: Line %d")), event->data.line.info->source, event->data.line.line);
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("I")));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("s", JSON_STRING("t")));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("ts", "%s"), ts_str);
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("pid", LUA_INT_FORMAT), LUA_INT_CAST(event->call.proc.pid));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), LUA_INT_CAST(event->call.proc.tid));
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    R->b.delim = 1;
    return LUA_OK;
  }
  return LMPROF_REPORT_UNKNOWN_TYPE;
}

static int __eventSampleInstance(lua_State *L, lmprof_Report *R, const TraceEvent *event) {
  const lu_time duration = event->data.sample.next->call.s.time - event->call.s.time;
  if (R->type == lTable) {
    lua_newtable(L);
    luaL_settabss(L, "cat", CHROME_TIMLINE);
    luaL_settabss(L, "name", "EvaluateScript");
    luaL_settabss(L, "ph", "X");
    luaL_settabsi(L, "pid", R->st->thread.mainproc.pid);
    luaL_settabsi(L, "tid", LMPROF_THREAD_SAMPLE_TIMELINE);
    luaL_settabsi(L, "ts", LMPROF_TIME_ADJ(event->call.s.time, R->st->conf));
    luaL_settabsi(L, "dur", LMPROF_TIME_ADJ(duration, R->st->conf));
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;

    REPORT_ENSURE_FILE_DELIM(R);
    fprintf(f, JSON_OPEN_OBJ);
    fprintf(f, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMLINE)));
    fprintf(f, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("EvaluateScript")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("X")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("pid", LUA_INTEGER_FMT), R->st->thread.mainproc.pid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), l_cast(lua_Integer, LMPROF_THREAD_SAMPLE_TIMELINE));
    fprintf(f, JSON_DELIM JSON_ASSIGN("ts", "%" PRIluTIME ""), LMPROF_TIME_ADJ(event->call.s.time, R->st->conf));
    fprintf(f, JSON_DELIM JSON_ASSIGN("dur", "%" PRIluTIME ""), LMPROF_TIME_ADJ(duration, R->st->conf));
    fprintf(f, JSON_CLOSE_OBJ);
    R->f.delim = 1;
    return LUA_OK;
#else
    return LMPROF_REPORT_DISABLED_IO;
#endif
  }
  else if (R->type == lBuffer) {
    luaL_Buffer *b = &R->b.buff;
    char ts_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    char dur_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    if (snprintf(ts_str, sizeof(ts_str), "%" PRIluTIME "", LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);
    if (snprintf(dur_str, sizeof(dur_str), "%" PRIluTIME "", LMPROF_TIME_ADJ(duration, R->st->conf)) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);

    REPORT_ENSURE_BUFFER_DELIM(R);
    luaL_addliteral(b, JSON_OPEN_OBJ);
    luaL_addliteral(b, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMLINE)));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("EvaluateScript")));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("X")));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("pid", LUA_INT_FORMAT), LUA_INT_CAST(R->st->thread.mainproc.pid));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), LUA_INT_CAST(LMPROF_THREAD_SAMPLE_TIMELINE));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("ts", "%s"), ts_str);
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("dur", "%s"), dur_str);
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    R->b.delim = 1;
    return LUA_OK;
  }
  return LMPROF_REPORT_UNKNOWN_TYPE;
}

static int __eventUpdateCounters(lua_State *L, lmprof_Report *R, const TraceEvent *event) {
  if (R->type == lTable) {
    lua_newtable(L); /* [..., process] */
    luaL_settabss(L, "cat", CHROME_TIMLINE);
    luaL_settabss(L, "name", "UpdateCounters");
    luaL_settabss(L, "ph", "I");
    luaL_settabss(L, "s", "g");
    luaL_settabsi(L, "pid", event->call.proc.pid);
    luaL_settabsi(L, "tid", event->call.proc.tid);
    luaL_settabsi(L, "ts", l_cast(lua_Integer, LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)));

    lua_newtable(L); /* [..., process, args] */
    lua_newtable(L); /* [..., process, args, data] */
    luaL_settabsi(L, "jsHeapSizeUsed", l_cast(lua_Integer, unit_allocated(&event->call.s)));
    /*
      luaL_settabsi(L, "documents", 0);
      luaL_settabsi(L, "jsEventListeners", 0);
      luaL_settabsi(L, "nodes", 0);
    */
    lua_setfield(L, -2, "data"); /* [..., process, args] */
    lua_setfield(L, -2, "args"); /* [..., process] */
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;

    REPORT_ENSURE_FILE_DELIM(R);
    fprintf(f, JSON_OPEN_OBJ);
    fprintf(f, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMLINE)));
    fprintf(f, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("UpdateCounters")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("I")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("s", JSON_STRING("g")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("pid", LUA_INTEGER_FMT), event->call.proc.pid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), event->call.proc.tid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("ts", "%" PRIluTIME ""), LMPROF_TIME_ADJ(event->call.s.time, R->st->conf));
    fprintf(f, JSON_DELIM JSON_ASSIGN("args", JSON_OPEN_OBJ));
    fprintf(f, JSON_ASSIGN("data", JSON_OPEN_OBJ));
    fprintf(f, JSON_ASSIGN("jsHeapSizeUsed", "%" PRIluSIZEDIFF), unit_allocated(&event->call.s));
    fprintf(f, JSON_CLOSE_OBJ);
    fprintf(f, JSON_CLOSE_OBJ);
    fprintf(f, JSON_CLOSE_OBJ);
    R->f.delim = 1;
    return LUA_OK;
#else
    return LMPROF_REPORT_DISABLED_IO;
#endif
  }
  else if (R->type == lBuffer) {
    luaL_Buffer *b = &R->b.buff;
    char hs_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    char ts_str[IDENTIFIER_BUFFER_LENGTH] = LMPROF_ZERO_STRUCT;
    if (snprintf(ts_str, sizeof(ts_str), "%" PRIluTIME "", LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);
    if (snprintf(hs_str, sizeof(hs_str), "%" PRIluSIZEDIFF "", unit_allocated(&event->call.s)) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);

    REPORT_ENSURE_BUFFER_DELIM(R);
    luaL_addliteral(b, JSON_OPEN_OBJ);
    luaL_addliteral(b, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMLINE)));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("UpdateCounters")));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("I")));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("s", JSON_STRING("g")));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("pid", LUA_INT_FORMAT), (int)event->call.proc.pid);
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), (int)event->call.proc.tid);
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("ts", "%s"), ts_str);
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("args", JSON_OPEN_OBJ));
    luaL_addliteral(b, JSON_ASSIGN("data", JSON_OPEN_OBJ));
    luaL_addfstring(L, b, JSON_ASSIGN("jsHeapSizeUsed", "%s"), hs_str);
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    luaL_addliteral(b, JSON_CLOSE_OBJ);
    R->b.delim = 1;
    return LUA_OK;
  }
  return LMPROF_REPORT_UNKNOWN_TYPE;
}

/*
** Append the chromium required CrBrowserMain/CrRendererMain metaevents to
** correctly format the profiled events.
*/
static void tracevent_table_header(lua_State *L, lmprof_Report *R, const TraceEventTimeline *list) {
  lmprof_EventProcess browser = { R->st->thread.mainproc.pid, LMPROF_THREAD_BROWSER };
  lmprof_EventProcess renderer = { R->st->thread.mainproc.pid, R->st->thread.mainproc.tid };
  lmprof_EventProcess sampler = { R->st->thread.mainproc.pid, LMPROF_THREAD_SAMPLE_TIMELINE };
  luaL_checkstack(L, 3, __FUNCTION__);
  UNUSED(list);

  /* Default process information */
  REPORT_TABLE_APPEND(L, R, __metaProcess(L, R, &browser, CHROME_META_PROCESS, CHROME_NAME_BROWSER));
  REPORT_TABLE_APPEND(L, R, __metaProcess(L, R, &browser, CHROME_META_THREAD, CHROME_NAME_CR_BROWSER));
  REPORT_TABLE_APPEND(L, R, __metaProcess(L, R, &renderer, CHROME_META_THREAD, CHROME_NAME_CR_RENDERER));
  REPORT_TABLE_APPEND(L, R, __metaProcess(L, R, &sampler, CHROME_META_THREAD, CHROME_NAME_SAMPLER));
  if (!BITFIELD_TEST(R->st->conf, LMPROF_OPT_TRACE_ABOUT_TRACING)) {
    REPORT_TABLE_APPEND(L, R, __metaTracingStarted(L, R, &browser, R->st->i.name, R->st->i.url));
  }

  /* Named threads */
  if (BITFIELD_TEST(R->st->conf, LMPROF_OPT_TRACE_LAYOUT_SPLIT)) {
    lmprof_thread_info(L, LMPROF_TAB_THREAD_NAMES); /* [..., names] */
    lua_pushnil(L); /* [..., names, nil] */
    while (lua_next(L, -2) != 0) { /* [..., names, key, value] */
      if (lua_isnumber(L, -2)) {
        const char *name = lua_tostring(L, -1);

        lmprof_EventProcess thread;
        thread.pid = LMPROF_PROCESS_MAIN;
        thread.tid = lua_tointeger(L, -2);

        REPORT_TABLE_APPEND(L, R, __metaProcess(L, R, &thread, CHROME_META_THREAD, name));
      }
      lua_pop(L, 1); /* [..., names, key] */
    }
    lua_pop(L, 1);
  }
}

/*
** Assuming a LUA_TTABLE is on top of the provided lua_State, format all trace
** buffered trace events and append them to the array, starting at "arrayIndex"
*/
static void traceevent_table_events(lua_State *L, lmprof_Report *R, TraceEventTimeline *list) {
  lmprof_State *st = R->st;
  TraceEventPage *page = l_nullptr; /* Page iterator */
  TraceEvent *samples = l_nullptr; /* Linked-list of SAMPLE_EVENT events*/

  size_t counter = 0;
  size_t counterFrequency = TRACE_EVENT_COUNTER_FREQ;

  timeline_adjust(list);
  if (st->i.counterFrequency > 0)
    counterFrequency = l_cast(size_t, st->i.counterFrequency);

  /* Compress small records to reduce size of output */
  if (BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_COMPRESS)) {
    int result;
    TraceEventCompressOpts opts;
    opts.id.pid = 0;
    opts.id.tid = 0;
    opts.threshold = st->i.event_threshold;
    if ((result = timeline_compress(list, opts)) != TRACE_EVENT_OK) {
      luaL_error(L, "trace event compression error: %d", result);
      return;
    }
  }

  luaL_checkstack(L, 8, __FUNCTION__);
  for (page = list->head; page != l_nullptr; page = page->next) {
    size_t i;
    for (i = 0; i < page->count; ++i) {
      TraceEvent *event = &page->event_array[i];
      TraceEventType op = event->op;
      if (op == ENTER_SCOPE || op == EXIT_SCOPE) {
        if (BITFIELD_TEST(event->data.event.info->event, LMPROF_RECORD_IGNORED | LMPROF_RECORD_ROOT))
          op = IGNORE_SCOPE; /* Function "ignored" during profiling */
      }

      switch (op) {
        case BEGIN_FRAME: {
          REPORT_TABLE_APPEND(L, R, __enterFrame(L, R, event));
          break;
        }
        case END_FRAME: {
          REPORT_TABLE_APPEND(L, R, __exitFrame(L, R, event));
          REPORT_TABLE_APPEND(L, R, __drawFrame(L, R, event));
          break;
        }
        case BEGIN_ROUTINE: {
          REPORT_TABLE_APPEND(L, R, __eventScope(L, R, event, CHROME_META_BEGIN, __threadName(L, R, event)));
          break;
        }
        case END_ROUTINE: {
          REPORT_TABLE_APPEND(L, R, __eventScope(L, R, event, CHROME_META_END, __threadName(L, R, event)));
          break;
        }
        case LINE_SCOPE: {
          REPORT_TABLE_APPEND(L, R, __eventLineInstance(L, R, event));
          break;
        }
        case SAMPLE_EVENT: {
          if (samples != l_nullptr) {
            samples->data.sample.next = event;
            REPORT_TABLE_APPEND(L, R, __eventSampleInstance(L, R, samples));
          }
          samples = event;
          break;
        }
        case ENTER_SCOPE: {
          REPORT_TABLE_APPEND(L, R, __eventScope(L, R, event, CHROME_META_BEGIN, CHROME_EVENT_NAME(event)));
          if (BITFIELD_TEST(st->mode, LMPROF_MODE_MEMORY) && (counterFrequency == 1 || ((++counter) % counterFrequency) == 0)) {
            REPORT_TABLE_APPEND(L, R, __eventUpdateCounters(L, R, event));
            counter = 0;
          }
          break;
        }
        case EXIT_SCOPE: {
          REPORT_TABLE_APPEND(L, R, __eventScope(L, R, event, CHROME_META_END, CHROME_EVENT_NAME(event)));
          if (BITFIELD_TEST(st->mode, LMPROF_MODE_MEMORY) && (counterFrequency == 1 || ((++counter) % counterFrequency) == 0)) {
            REPORT_TABLE_APPEND(L, R, __eventUpdateCounters(L, R, event));
            counter = 0;
          }
          break;
        }
        case PROCESS: {
          REPORT_TABLE_APPEND(L, R, __metaProcess(L, R, &event->call.proc, CHROME_META_PROCESS, CHROME_OPT_NAME(event->data.process.name, CHROME_NAME_PROCESS)));
          break;
        }
        case THREAD: {
          REPORT_TABLE_APPEND(L, R, __metaProcess(L, R, &event->call.proc, CHROME_META_THREAD, CHROME_OPT_NAME(event->data.process.name, CHROME_NAME_PROCESS)));
          break;
        }
        case IGNORE_SCOPE:
          break;
        default:
          return;
      }
    }
  }
}

static int traceevent_report_header(lua_State *L, lmprof_Report *R) {
  if (R->type == lTable) {
    lmprof_State *st = R->st;
    const TraceEventTimeline *list = l_pcast(TraceEventTimeline *, st->i.trace.arg);

    profiler_header(L, R);
    luaL_settabsb(L, "compress", BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_COMPRESS));
    luaL_settabsi(L, "eventsize", l_cast(lua_Integer, sizeof(TraceEvent)));
    luaL_settabsi(L, "eventpages", l_cast(lua_Integer, timeline_event_array_size()));

    luaL_settabsi(L, "usedpages", l_cast(lua_Integer, list->pageCount));
    luaL_settabsi(L, "totalpages", l_cast(lua_Integer, list->pageLimit));
    luaL_settabsi(L, "pagelimit", l_cast(lua_Integer, list->pageLimit * timeline_page_size()));
    luaL_settabsi(L, "pagesize", l_cast(lua_Integer, timeline_page_size()));
    luaL_settabsn(L, "pageusage", l_cast(lua_Number, timeline_usage(list)));
  }
  return LUA_OK;
}

static int traceevent_report(lua_State *L, lmprof_Report *report) {
  lmprof_State *st = report->st;
  TraceEventTimeline *list = l_pcast(TraceEventTimeline *, st->i.trace.arg);
  if (report->type == lTable) {
    const int prev_table = report->t.table_index;

    lua_newtable(L); /* [..., header_table] */
    report->t.table_index = lua_absindex(L, -1);
    traceevent_report_header(L, report);
    lua_setfield(L, prev_table, "header");
    report->t.table_index = prev_table;

    lua_newtable(L); /* [..., records] */
    report->t.table_index = lua_absindex(L, -1);
    if (BITFIELD_TEST(report->st->conf, LMPROF_OPT_TRACE_ABOUT_TRACING)) {
      lua_newtable(L); /* [..., records, traceEvents] */
      tracevent_table_header(L, report, list);
      traceevent_table_events(L, report, list);
      lua_setfield(L, -2, "traceEvents"); /* [..., records] */
      __metaAbout(L, report, LMPROF, LUA_VERSION); /* [..., records, metadata] */
      lua_setfield(L, -2, "metadata"); /* [..., records] */
    }
    else {
      tracevent_table_header(L, report, list);
      traceevent_table_events(L, report, list);
    }

    lua_setfield(L, prev_table, "records");
    report->t.table_index = prev_table;
    return LUA_OK;
  }
  else if (report->type == lFile) {
#if defined(LMPROF_FILE_API)
    if (BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_ABOUT_TRACING))
      fprintf(report->f.file, JSON_OPEN_OBJ JSON_STRING("traceEvents") ":" JSON_OPEN_ARRAY JSON_NEWLINE);
    else
      fprintf(report->f.file, JSON_OPEN_ARRAY JSON_NEWLINE);

    tracevent_table_header(L, report, list);
    traceevent_table_events(L, report, list);
    if (BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_ABOUT_TRACING)) {
      report->f.delim = 0;
      fprintf(report->f.file, JSON_CLOSE_ARRAY JSON_DELIM);
      __metaAbout(L, report, LMPROF, LUA_VERSION);
      fprintf(report->f.file, JSON_CLOSE_OBJ JSON_NEWLINE);
    }
    else {
      fprintf(report->f.file, JSON_NEWLINE JSON_CLOSE_ARRAY JSON_NEWLINE);
    }
    return LUA_OK;
#else
    return LMPROF_REPORT_DISABLED_IO;
#endif
  }
  else if (report->type == lBuffer) {
    if (BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_ABOUT_TRACING))
      luaL_addliteral(&report->b.buff, JSON_OPEN_OBJ JSON_STRING("traceEvents") ":" JSON_OPEN_ARRAY JSON_NEWLINE);
    else
      luaL_addliteral(&report->b.buff, JSON_OPEN_ARRAY JSON_NEWLINE);

    tracevent_table_header(L, report, list);
    traceevent_table_events(L, report, list);
    if (BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_ABOUT_TRACING)) {
      report->f.delim = 0;
      luaL_addliteral(&report->b.buff, JSON_CLOSE_ARRAY JSON_DELIM);
      __metaAbout(L, report, LMPROF, LUA_VERSION);
      luaL_addliteral(&report->b.buff, JSON_CLOSE_OBJ JSON_NEWLINE);
    }
    else {
      luaL_addliteral(&report->b.buff, JSON_NEWLINE JSON_CLOSE_ARRAY JSON_NEWLINE);
    }
    return LUA_OK;
  }
  return LMPROF_REPORT_FAILURE;
}

/* }================================================================== */

/*
** {==================================================================
** API
** ===================================================================
*/

static LUA_INLINE int lmprof_push_report(lua_State *L, lmprof_Report *report) {
  if (BITFIELD_TEST(report->st->mode, LMPROF_MODE_TIME | LMPROF_MODE_EXT_CALLBACK))
    return LMPROF_REPORT_FAILURE;
  else if (BITFIELD_TEST(report->st->mode, LMPROF_MODE_TRACE))
    return traceevent_report(L, report);
  else if (BITFIELD_TEST(report->st->mode, LMPROF_MODE_INSTRUMENT | LMPROF_MODE_MEMORY | LMPROF_MODE_SAMPLE))
    return graph_report(L, report);
  return LMPROF_REPORT_FAILURE;
}

LUA_API void lmprof_report_initialize(lua_State *L) {
#if defined(LMPROF_FILE_API)
  static const luaL_Reg metameth[] = {
    { "__gc", io_fgc },
    { "__close", io_fgc },
    { l_nullptr, l_nullptr }
  };

  if (luaL_newmetatable(L, LMPROF_IO_METATABLE)) { /* metatable for file handles */
  #if LUA_VERSION_NUM == 501
    luaL_register(L, l_nullptr, metameth); /* add metamethods to new metatable */
  #else
    luaL_setfuncs(L, metameth, 0);
  #endif
  }
  lua_pop(L, 1); /* pop metatable */
#else
  ((void)L);
#endif
}

LUA_API int lmprof_report(lua_State *L, lmprof_State *st, lmprof_ReportType type, const char *file) {
  lmprof_Report report;
  report.st = st;

  if (BITFIELD_TEST(st->mode, LMPROF_MODE_TIME)) {
    const lu_time t = lmprof_clock_diff(st->thread.r.s.time, LMPROF_TIME(st));
    lua_pushinteger(L, l_cast(lua_Integer, LMPROF_TIME_ADJ(t, st->conf)));
  }
  else if (type == lTable) {
    lua_newtable(L);
    report.type = lTable;
    report.t.array_count = 1;
    report.t.table_index = lua_gettop(L);
    if (lmprof_push_report(L, &report) != LUA_OK) {
      lua_pop(L, 1); /* Invalid encoding; return nil.*/
      lua_pushnil(L);
    }
  }
  else if (type == lBuffer) {
    const int top = lua_gettop(L);
    report.type = lBuffer;
    report.b.delim = 0;
    report.b.indent = "";
    luaL_buffinit(L, &report.b.buff);
    if (lmprof_push_report(L, &report) != LUA_OK) {
      lua_settop(L, top); /* Invalid encoding; return nil.*/
      lua_pushnil(L);
    }

    luaL_pushresult(&report.b.buff);
  }
  else if (type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE **pf;
    int result = LUA_OK;
    if (file == l_nullptr)
      result = LMPROF_REPORT_FAILURE;
    else if ((pf = io_fud(L, file)) != l_nullptr) { /* [..., io_ud] */
      report.type = lFile;
      report.f.file = *pf;
      report.f.delim = 0;
      report.f.indent = "";
      result = lmprof_push_report(L, &report);
      if (fclose(*pf) == 0) {
        *pf = l_nullptr; /* marked as closed */
        lua_pushnil(L); /* preemptively remove finalizer */
        lua_setmetatable(L, -2);
      }

      lua_pop(L, 1);
    }

    lua_pushboolean(L, result == LUA_OK); /* Success */
#else
    UNUSED(file);
    lua_pushboolean(L, 0); /* Failure */
#endif
  }
  else {
    lua_pushnil(L);
  }
  return lua_type(L, -1);
}

/* }================================================================== */
