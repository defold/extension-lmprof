/*
** $Id: lmprof_report_graph.c $
** Graph report output.
** See Copyright Notice in lmprof_lib.h
*/
#define LUA_LIB

#include "lmprof_report_internal.h"
#include "collections/lmprof_hash.h"

/*
** {==================================================================
** Graph Profiler Format
** ===================================================================
*/

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

int lmprof_graph_report(lua_State *L, lmprof_Report *report) {
  lmprof_State *st = report->st;

  luaL_checkstack(L, 8, __FUNCTION__);
  if (report->type == lTable) {
    lua_newtable(L); /* [..., header] */
    lmprof_report_profiler_header(L, report);
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
    lmprof_report_profiler_header(L, report);
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
    lmprof_report_profiler_header(L, report);
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

