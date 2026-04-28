/*
** $Id: lmprof_report.c $
** Output/Formatting
** See Copyright Notice in lmprof_lib.h
*/
#define LUA_LIB

#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

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
static FILE **io_fud(lua_State *L, const char *output, const char *mode) {
  FILE **pf = l_pcast(FILE **, lmprof_newuserdata(L, sizeof(FILE *)));
  *pf = l_nullptr;

  #if LUA_VERSION_NUM == 501
  luaL_getmetatable(L, LMPROF_IO_METATABLE);
  lua_setmetatable(L, -2);
  #else
  luaL_setmetatable(L, LMPROF_IO_METATABLE);
  #endif

  /* Open File... consider destroying the profiler state on failure? */
  if ((*pf = fopen(output, mode)) == l_nullptr) {
    luaL_error(L, "cannot open file '%s' (%s)", output, strerror(errno));
    return l_nullptr;
  }
  return pf;
}

#endif
/* }================================================================== */

static int lmprof_ascii_equal_ci(char a, char b) {
  if (a >= 'A' && a <= 'Z')
    a = l_cast(char, a + ('a' - 'A'));
  if (b >= 'A' && b <= 'Z')
    b = l_cast(char, b + ('a' - 'A'));
  return a == b;
}

static int lmprof_path_ends_with(const char *file, const char *ext) {
  size_t i;
  const size_t file_len = file == l_nullptr ? 0 : strlen(file);
  const size_t ext_len = strlen(ext);
  if (file_len < ext_len)
    return 0;

  file += file_len - ext_len;
  for (i = 0; i < ext_len; ++i) {
    if (!lmprof_ascii_equal_ci(file[i], ext[i]))
      return 0;
  }
  return 1;
}

static int lmprof_path_ends_with_json(const char *file) {
  return lmprof_path_ends_with(file, ".json");
}

static int lmprof_path_ends_with_tracy(const char *file) {
  return lmprof_path_ends_with(file, ".tracy");
}

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

/* chrome://tracing/ metadata reporting/ */
static int __metaAbout(lua_State *L, lmprof_Report *R, const char *name, const char *url);

/* Synthetic renderer task root */
static int __eventRunTask(lua_State *L, lmprof_Report *R, lu_time start, lu_time duration);

/* BEGIN_FRAME */
static int __enterFrame(lua_State *L, lmprof_Report *R, const TraceEvent *event);

/* END_FRAME */
static int __exitFrame(lua_State *L, lmprof_Report *R, const TraceEvent *event);
static int __drawFrame(lua_State *L, lmprof_Report *R, const TraceEvent *event);

/* BEGIN_ROUTINE/END_ROUTINE ENTER_SCOPE/EXIT_SCOPE */
static int __eventScope(lua_State *L, lmprof_Report *R, const TraceEvent *event, const char *name, const char *eventName);
static int __eventCompleteScope(lua_State *L, lmprof_Report *R, const TraceEvent *event, const char *eventName);
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

static int __eventRunTask(lua_State *L, lmprof_Report *R, lu_time start, lu_time duration) {
  if (R->type == lTable) {
    lua_newtable(L);
    luaL_settabss(L, "cat", CHROME_TIMLINE);
    luaL_settabss(L, "name", CHROME_NAME_RUN_TASK);
    luaL_settabss(L, "ph", "X");
    luaL_settabsi(L, "pid", R->st->thread.mainproc.pid);
    luaL_settabsi(L, "tid", R->st->thread.mainproc.tid);
    luaL_settabsi(L, "ts", l_cast(lua_Integer, start));
    luaL_settabsi(L, "dur", l_cast(lua_Integer, duration));
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;

    REPORT_ENSURE_FILE_DELIM(R);
    fprintf(f, JSON_OPEN_OBJ);
    fprintf(f, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMLINE)));
    fprintf(f, JSON_DELIM JSON_ASSIGN("name", JSON_STRING(CHROME_NAME_RUN_TASK)));
    fprintf(f, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("X")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("pid", LUA_INTEGER_FMT), R->st->thread.mainproc.pid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), R->st->thread.mainproc.tid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("ts", "%" PRIluTIME ""), start);
    fprintf(f, JSON_DELIM JSON_ASSIGN("dur", "%" PRIluTIME ""), duration);
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
    if (snprintf(ts_str, sizeof(ts_str), "%" PRIluTIME "", start) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);
    if (snprintf(dur_str, sizeof(dur_str), "%" PRIluTIME "", duration) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);

    REPORT_ENSURE_BUFFER_DELIM(R);
    luaL_addliteral(b, JSON_OPEN_OBJ);
    luaL_addliteral(b, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMLINE)));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("name", JSON_STRING(CHROME_NAME_RUN_TASK)));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("X")));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("pid", LUA_INT_FORMAT), LUA_INT_CAST(R->st->thread.mainproc.pid));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), LUA_INT_CAST(R->st->thread.mainproc.tid));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("ts", "%s"), ts_str);
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("dur", "%s"), dur_str);
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
    luaL_settabss(L, "cat", CHROME_TIMLINE);
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
    fprintf(f, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMLINE)));
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
    luaL_addliteral(b, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMLINE)));
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

static int __eventCompleteScope(lua_State *L, lmprof_Report *R, const TraceEvent *event, const char *eventName) {
  const TraceEvent *sibling = event->data.event.sibling;
  const lu_time start = LMPROF_TIME_ADJ(event->call.s.time, R->st->conf);
  const lu_time end = sibling != l_nullptr ? LMPROF_TIME_ADJ(sibling->call.s.time, R->st->conf) : start;
  const lu_time duration = end > start ? end - start : 0;

  if (R->type == lTable) {
    lua_newtable(L);
    luaL_settabss(L, "cat", CHROME_TIMLINE);
    luaL_settabss(L, "name", eventName);
    luaL_settabss(L, "ph", "X");
    luaL_settabsi(L, "pid", event->call.proc.pid);
    if (op_routine(event->op))
      luaL_settabsi(L, "tid", R->st->thread.mainproc.tid);
    else
      luaL_settabsi(L, "tid", event->call.proc.tid);
    luaL_settabsi(L, "ts", l_cast(lua_Integer, start));
    luaL_settabsi(L, "dur", l_cast(lua_Integer, duration));
    return LUA_OK;
  }
  else if (R->type == lFile) {
#if defined(LMPROF_FILE_API)
    FILE *f = R->f.file;

    REPORT_ENSURE_FILE_DELIM(R);
    fprintf(f, JSON_OPEN_OBJ);
    fprintf(f, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMLINE)));
    fprintf(f, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("%s")), eventName);
    fprintf(f, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("X")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("pid", LUA_INTEGER_FMT), event->call.proc.pid);
    if (op_routine(event->op))
      fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), R->st->thread.mainproc.tid);
    else
      fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), event->call.proc.tid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("ts", "%" PRIluTIME ""), start);
    fprintf(f, JSON_DELIM JSON_ASSIGN("dur", "%" PRIluTIME ""), duration);
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
    if (snprintf(ts_str, sizeof(ts_str), "%" PRIluTIME "", start) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);
    if (snprintf(dur_str, sizeof(dur_str), "%" PRIluTIME "", duration) < 0)
      LMPROF_LOG("<%s>:sprintf encoding error\n", __FUNCTION__);

    REPORT_ENSURE_BUFFER_DELIM(R);
    luaL_addliteral(b, JSON_OPEN_OBJ);
    luaL_addliteral(b, JSON_ASSIGN("cat", JSON_STRING(CHROME_TIMLINE)));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("name", JSON_STRING("%s")), eventName);
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("X")));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("pid", LUA_INT_FORMAT), LUA_INT_CAST(event->call.proc.pid));
    if (op_routine(event->op))
      luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), LUA_INT_CAST(R->st->thread.mainproc.tid));
    else
      luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), LUA_INT_CAST(event->call.proc.tid));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("ts", "%s"), ts_str);
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("dur", "%s"), dur_str);
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
    luaL_settabss(L, "ph", "C");
    luaL_settabss(L, "s", "g");
    luaL_settabsi(L, "pid", event->call.proc.pid);
    luaL_settabsi(L, "tid", event->call.proc.tid);
    luaL_settabsi(L, "ts", l_cast(lua_Integer, LMPROF_TIME_ADJ(event->call.s.time, R->st->conf)));

    lua_newtable(L); /* [..., process, args] */
    luaL_settabsi(L, "LuaMemory", l_cast(lua_Integer, unit_allocated(&event->call.s)));
    /*
      luaL_settabsi(L, "documents", 0);
      luaL_settabsi(L, "jsEventListeners", 0);
      luaL_settabsi(L, "nodes", 0);
    */
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
    fprintf(f, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("C")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("s", JSON_STRING("g")));
    fprintf(f, JSON_DELIM JSON_ASSIGN("pid", LUA_INTEGER_FMT), event->call.proc.pid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("tid", LUA_INTEGER_FMT), event->call.proc.tid);
    fprintf(f, JSON_DELIM JSON_ASSIGN("ts", "%" PRIluTIME ""), LMPROF_TIME_ADJ(event->call.s.time, R->st->conf));
    fprintf(f, JSON_DELIM JSON_ASSIGN("args", JSON_OPEN_OBJ));
    fprintf(f, JSON_ASSIGN("LuaMemory", "%" PRIluSIZEDIFF), unit_allocated(&event->call.s));
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
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("ph", JSON_STRING("C")));
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("s", JSON_STRING("g")));
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("pid", LUA_INT_FORMAT), (int)event->call.proc.pid);
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("tid", LUA_INT_FORMAT), (int)event->call.proc.tid);
    luaL_addfstring(L, b, JSON_DELIM JSON_ASSIGN("ts", "%s"), ts_str);
    luaL_addliteral(b, JSON_DELIM JSON_ASSIGN("args", JSON_OPEN_OBJ));
    luaL_addfstring(L, b, JSON_ASSIGN("LuaMemory", "%s"), hs_str);
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

typedef struct TraceEventBounds {
  int valid;
  lu_time start;
  lu_time end;
} TraceEventBounds;

static TraceEventType traceevent_report_op(const TraceEvent *event) {
  TraceEventType op = event->op;
  if ((op == ENTER_SCOPE || op == EXIT_SCOPE) && event->data.event.info != l_nullptr) {
    if (BITFIELD_TEST(event->data.event.info->event, LMPROF_RECORD_IGNORED | LMPROF_RECORD_ROOT))
      op = IGNORE_SCOPE; /* Function "ignored" during profiling */
  }
  return op;
}

static void traceevent_bounds_add(TraceEventBounds *bounds, lu_time time) {
  if (!bounds->valid) {
    bounds->valid = 1;
    bounds->start = time;
    bounds->end = time;
  }
  else {
    if (time < bounds->start)
      bounds->start = time;
    if (time > bounds->end)
      bounds->end = time;
  }
}

static int traceevent_find_bounds(lmprof_State *st, TraceEventTimeline *list, TraceEventBounds *bounds) {
  TraceEventPage *page = l_nullptr;
  bounds->valid = 0;
  bounds->start = 0;
  bounds->end = 0;

  for (page = list->head; page != l_nullptr; page = page->next) {
    size_t i;
    for (i = 0; i < page->count; ++i) {
      const TraceEvent *event = &page->event_array[i];
      const TraceEventType op = traceevent_report_op(event);
      if (op == IGNORE_SCOPE || !op_adjust(op))
        continue;

      traceevent_bounds_add(bounds, LMPROF_TIME_ADJ(event->call.s.time, st->conf));
    }
  }

  return bounds->valid && bounds->end > bounds->start;
}

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

static uint64_t perfetto_time_ns(const lmprof_State *st, lu_time time) {
  const uint64_t adjusted = l_cast(uint64_t, LMPROF_TIME_ADJ(time, st->conf));
#if LUA_32BITS
  return adjusted * 1000u;
#else
  return BITFIELD_TEST(st->conf, LMPROF_OPT_CLOCK_MICRO) ? adjusted * 1000u : adjusted;
#endif
}

static uint64_t perfetto_adjusted_time_ns(const lmprof_State *st, lu_time time) {
  const uint64_t adjusted = l_cast(uint64_t, time);
#if LUA_32BITS
  return adjusted * 1000u;
#else
  return BITFIELD_TEST(st->conf, LMPROF_OPT_CLOCK_MICRO) ? adjusted * 1000u : adjusted;
#endif
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

static int perfetto_traceevent_report(lua_State *L, lmprof_Report *R, TraceEventTimeline *list) {
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

  has_run_task = traceevent_find_bounds(st, list, &bounds);
  if (has_run_task) {
    result = perfetto_queue_slice(&queue,
        perfetto_thread_uuid(st->thread.mainproc.pid, st->thread.mainproc.tid),
        perfetto_adjusted_time_ns(st, bounds.start),
        perfetto_adjusted_time_ns(st, bounds.end),
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
      TraceEventType op = traceevent_report_op(event);
      const uint64_t timestamp_ns = op_adjust(op) ? perfetto_time_ns(st, event->call.s.time) : 0;
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
                perfetto_time_ns(st, samples->call.s.time),
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
                perfetto_time_ns(st, event->data.event.sibling->call.s.time),
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
#define TRACY_PLOT_FORMAT_MEMORY 1

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

  TracyFrameSet frames;

  TracyPlot *plots;
  size_t plot_count;
  size_t plot_capacity;

  uint32_t frame_name_idx;
  uint64_t frame_name_ptr;
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

static int tracy_frame_add(TracyExport *E, int64_t time) {
  if (tracy_reserve_array(l_pcast(void **, &E->frames.starts), &E->frames.capacity, E->frames.count + 1, sizeof(int64_t)) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  E->frames.starts[E->frames.count++] = time;
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

static int tracy_file_flush(TracyFileWriter *W) {
  unsigned char compressed[TRACY_FILE_COMPRESS_BOUND];
  uint32_t size;
  if (W->failed || W->offset == 0)
    return W->failed ? LMPROF_REPORT_FAILURE : LUA_OK;

  size = l_cast(uint32_t, tracy_lz4_literals(compressed, W->buffer, W->offset));
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

static int tracy_write_capture(TracyFileWriter *W, TracyExport *E, lmprof_State *st, const char *file) {
  static const unsigned char file_header[8] = { 't', 'r', 'a', 'c', 'y', TRACY_FILE_MAJOR, TRACY_FILE_MINOR, TRACY_FILE_PATCH };
  const char *capture_name = CHROME_OPT_NAME(file, "lmprof.tracy");
  const char *program_name = CHROME_OPT_NAME(st->i.name, LMPROF);
  const char cpu_manufacturer[12] = { 0 };
  const uint64_t max64 = ~(uint64_t)0;
  size_t i;

  if (E->frames.count == 0) {
    if (tracy_frame_add(E, 0) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
  }
  if (E->frames.count == 1) {
    const int64_t last = E->last_time > E->frames.starts[0] ? E->last_time : E->frames.starts[0] + 1;
    if (tracy_frame_add(E, last) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
  }

  if (tracy_export_add_string(E, "Frame", &E->frame_name_idx, &E->frame_name_ptr) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  for (i = 0; i < E->thread_count; ++i) {
    TracyThread *thread = &E->threads[i];
    if (thread->name_ptr == 0
        && tracy_export_set_thread_name(E, thread->tid, thread->tid == l_cast(uint64_t, st->thread.mainproc.tid) ? CHROME_NAME_MAIN : CHROME_META_TICK) != LUA_OK)
      return LMPROF_REPORT_FAILURE;
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

  /* Frame set. */
  if (tracy_write_u64(W, 1) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u8(W, 1) != LUA_OK
      || tracy_write_u64(W, E->frames.count) != LUA_OK)
    return LMPROF_REPORT_FAILURE;
  {
    int64_t ref = 0;
    for (i = 0; i < E->frames.count; ++i) {
      if (tracy_write_time_offset(W, &ref, E->frames.starts[i]) != LUA_OK
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

  if (tracy_write_u64(W, E->string_count + 1) != LUA_OK
      || tracy_write_u64(W, 0) != LUA_OK
      || tracy_write_u64(W, E->frame_name_ptr) != LUA_OK)
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

static uint64_t tracy_event_tid(const TraceEvent *event) {
  return l_cast(uint64_t, event->call.proc.tid);
}

static uint32_t tracy_event_line(const TraceEvent *event) {
  const lmprof_FunctionInfo *info = event->data.event.info;
  if (info == l_nullptr)
    return 0;
  if (info->currentline > 0)
    return l_cast(uint32_t, info->currentline);
  if (info->linedefined > 0)
    return l_cast(uint32_t, info->linedefined);
  return 0;
}

static const char *tracy_event_file(const TraceEvent *event) {
  const lmprof_FunctionInfo *info = event->data.event.info;
  return info == l_nullptr ? "" : CHROME_OPT_NAME(info->short_src, "");
}

static int tracy_traceevent_report(lua_State *L, lmprof_Report *R, TraceEventTimeline *list, const char *file) {
  lmprof_State *st = R->st;
  TracyExport E;
  TracyFileWriter W;
  TraceEventBounds bounds;
  TraceEventPage *page = l_nullptr;
  size_t counter = 0;
  size_t counterFrequency = TRACE_EVENT_COUNTER_FREQ;
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

  if (tracy_export_set_thread_name(&E, st->thread.mainproc.tid, CHROME_NAME_MAIN) != LUA_OK) {
    result = LMPROF_REPORT_FAILURE;
    goto done;
  }

  if (traceevent_find_bounds(st, list, &bounds)) {
    const int64_t start = l_cast(int64_t, perfetto_adjusted_time_ns(st, bounds.start));
    const int64_t end = l_cast(int64_t, perfetto_adjusted_time_ns(st, bounds.end));
    if (tracy_export_add_zone(&E, l_cast(uint64_t, st->thread.mainproc.tid), CHROME_NAME_RUN_TASK, "", 0, start, end) == l_nullptr) {
      result = LMPROF_REPORT_FAILURE;
      goto done;
    }
  }

  for (page = list->head; page != l_nullptr; page = page->next) {
    size_t i;
    for (i = 0; i < page->count; ++i) {
      TraceEvent *event = &page->event_array[i];
      const TraceEventType op = traceevent_report_op(event);
      const int64_t time_ns = op_adjust(op) ? l_cast(int64_t, perfetto_time_ns(st, event->call.s.time)) : 0;

      switch (op) {
        case BEGIN_FRAME:
          if (tracy_frame_add(&E, time_ns) != LUA_OK) {
            result = LMPROF_REPORT_FAILURE;
            goto done;
          }
          break;
        case ENTER_SCOPE:
          if (event->data.event.sibling != l_nullptr) {
            const int64_t end_ns = l_cast(int64_t, perfetto_time_ns(st, event->data.event.sibling->call.s.time));
            if (tracy_export_add_zone(&E, tracy_event_tid(event), CHROME_EVENT_NAME(event), tracy_event_file(event), tracy_event_line(event), time_ns, end_ns) == l_nullptr) {
              result = LMPROF_REPORT_FAILURE;
              goto done;
            }
          }
          /* fall through */
        case EXIT_SCOPE:
          if (BITFIELD_TEST(st->mode, LMPROF_MODE_MEMORY) && (counterFrequency == 1 || ((++counter) % counterFrequency) == 0)) {
            if (tracy_plot_add_point(&E, "LuaMemory", TRACY_PLOT_FORMAT_MEMORY, time_ns, l_cast(double, unit_allocated(&event->call.s))) != LUA_OK) {
              result = LMPROF_REPORT_FAILURE;
              goto done;
            }
            counter = 0;
          }
          break;
        case THREAD:
          if (tracy_export_set_thread_name(&E, tracy_event_tid(event), CHROME_OPT_NAME(event->data.process.name, CHROME_META_TICK)) != LUA_OK) {
            result = LMPROF_REPORT_FAILURE;
            goto done;
          }
          break;
        case PROCESS:
        case END_FRAME:
        case BEGIN_ROUTINE:
        case END_ROUTINE:
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
    free(E.frames.starts);
    for (i = 0; i < E.plot_count; ++i) {
      free(E.plots[i].name);
      free(E.plots[i].points);
    }
    free(E.plots);
  }

  return result;
}

/* }================================================================== */

/*
** Assuming a LUA_TTABLE is on top of the provided lua_State, format all trace
** buffered trace events and append them to the array, starting at "arrayIndex"
*/
static void traceevent_table_events(lua_State *L, lmprof_Report *R, TraceEventTimeline *list) {
  lmprof_State *st = R->st;
  TraceEventPage *page = l_nullptr; /* Page iterator */
  TraceEvent *samples = l_nullptr; /* Linked-list of SAMPLE_EVENT events*/
  TraceEventBounds bounds;

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
  if (traceevent_find_bounds(st, list, &bounds))
    REPORT_TABLE_APPEND(L, R, __eventRunTask(L, R, bounds.start, bounds.end - bounds.start));

  for (page = list->head; page != l_nullptr; page = page->next) {
    size_t i;
    for (i = 0; i < page->count; ++i) {
      TraceEvent *event = &page->event_array[i];
      TraceEventType op = traceevent_report_op(event);

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
          if (event->data.event.sibling != l_nullptr)
            REPORT_TABLE_APPEND(L, R, __eventCompleteScope(L, R, event, CHROME_EVENT_NAME(event)));
          if (BITFIELD_TEST(st->mode, LMPROF_MODE_MEMORY) && (counterFrequency == 1 || ((++counter) % counterFrequency) == 0)) {
            REPORT_TABLE_APPEND(L, R, __eventUpdateCounters(L, R, event));
            counter = 0;
          }
          break;
        }
        case EXIT_SCOPE: {
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
    if (report->f.binary == LMPROF_TRACE_FILE_PERFETTO)
      return perfetto_traceevent_report(L, report, list);
    if (report->f.binary == LMPROF_TRACE_FILE_TRACY)
      return tracy_traceevent_report(L, report, list, report->f.path);

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

static lmprof_TraceFileFormat lmprof_trace_file_format(lmprof_State *st, const char *file) {
  if (BITFIELD_TEST(st->mode, LMPROF_MODE_TRACE)) {
    if (lmprof_path_ends_with_tracy(file))
      return LMPROF_TRACE_FILE_TRACY;
    if (!lmprof_path_ends_with_json(file))
      return LMPROF_TRACE_FILE_PERFETTO;
  }
  return LMPROF_TRACE_FILE_JSON;
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

LUA_API int lmprof_report_file_binary(lmprof_State *st, const char *file) {
  return lmprof_trace_file_format(st, file) != LMPROF_TRACE_FILE_JSON;
}

LUA_API int lmprof_report_file(lua_State *L, lmprof_State *st, FILE *file, const char *path) {
  lmprof_Report report;

  if (file == l_nullptr || path == l_nullptr)
    return LMPROF_REPORT_FAILURE;

  report.st = st;
  report.type = lFile;
  report.f.file = file;
  report.f.delim = 0;
  report.f.binary = lmprof_trace_file_format(st, path);
  report.f.path = path;
  report.f.indent = "";
  return lmprof_push_report(L, &report);
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
    else if ((pf = io_fud(L, file, lmprof_report_file_binary(st, file) ? "wb" : "w")) != l_nullptr) { /* [..., io_ud] */
      result = lmprof_report_file(L, st, *pf, file);
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
