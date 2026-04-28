/*
** $Id: lmprof_report.c $
** Output/Formatting
** See Copyright Notice in lmprof_lib.h
*/
#define LUA_LIB

#include <errno.h>
#include <string.h>

#include "lmprof_report_internal.h"

/*
** {==================================================================
** File Handling
** ===================================================================
*/
#if defined(LMPROF_FILE_API)
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

int lmprof_report_profiler_header(lua_State *L, lmprof_Report *R) {
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
** {==================================================================
** API
** ===================================================================
*/

static LUA_INLINE int lmprof_push_report(lua_State *L, lmprof_Report *report) {
  if (BITFIELD_TEST(report->st->mode, LMPROF_MODE_TIME | LMPROF_MODE_EXT_CALLBACK))
    return LMPROF_REPORT_FAILURE;
  else if (BITFIELD_TEST(report->st->mode, LMPROF_MODE_TRACE))
    return lmprof_traceevent_report(L, report);
  else if (BITFIELD_TEST(report->st->mode, LMPROF_MODE_INSTRUMENT | LMPROF_MODE_MEMORY | LMPROF_MODE_SAMPLE))
    return lmprof_graph_report(L, report);
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
