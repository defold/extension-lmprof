/*
** $Id: lmprof_report_traceevent.c $
** Chrome Trace Event table and JSON output.
** See Copyright Notice in lmprof_lib.h
*/
#define LUA_LIB

#include "lmprof_report_internal.h"

/*
** {==================================================================
** Trace Event Formatting
** ===================================================================
*/

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

TraceEventType lmprof_traceevent_report_op(const TraceEvent *event) {
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

int lmprof_traceevent_find_bounds(lmprof_State *st, TraceEventTimeline *list, TraceEventBounds *bounds) {
  TraceEventPage *page = l_nullptr;
  bounds->valid = 0;
  bounds->start = 0;
  bounds->end = 0;

  for (page = list->head; page != l_nullptr; page = page->next) {
    size_t i;
    for (i = 0; i < page->count; ++i) {
      const TraceEvent *event = &page->event_array[i];
      const TraceEventType op = lmprof_traceevent_report_op(event);
      if (op == IGNORE_SCOPE || !op_adjust(op))
        continue;

      traceevent_bounds_add(bounds, LMPROF_TIME_ADJ(event->call.s.time, st->conf));
    }
  }

  return bounds->valid && bounds->end > bounds->start;
}


uint64_t lmprof_traceevent_time_ns(const lmprof_State *st, lu_time time) {
  const uint64_t adjusted = l_cast(uint64_t, LMPROF_TIME_ADJ(time, st->conf));
#if LUA_32BITS
  return adjusted * 1000u;
#else
  return BITFIELD_TEST(st->conf, LMPROF_OPT_CLOCK_MICRO) ? adjusted * 1000u : adjusted;
#endif
}

uint64_t lmprof_traceevent_adjusted_time_ns(const lmprof_State *st, lu_time time) {
  const uint64_t adjusted = l_cast(uint64_t, time);
#if LUA_32BITS
  return adjusted * 1000u;
#else
  return BITFIELD_TEST(st->conf, LMPROF_OPT_CLOCK_MICRO) ? adjusted * 1000u : adjusted;
#endif
}

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
  if (lmprof_traceevent_find_bounds(st, list, &bounds))
    REPORT_TABLE_APPEND(L, R, __eventRunTask(L, R, bounds.start, bounds.end - bounds.start));

  for (page = list->head; page != l_nullptr; page = page->next) {
    size_t i;
    for (i = 0; i < page->count; ++i) {
      TraceEvent *event = &page->event_array[i];
      TraceEventType op = lmprof_traceevent_report_op(event);

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

    lmprof_report_profiler_header(L, R);
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

int lmprof_traceevent_report(lua_State *L, lmprof_Report *report) {
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
      return lmprof_perfetto_traceevent_report(L, report, list);
    if (report->f.binary == LMPROF_TRACE_FILE_TRACY)
      return lmprof_tracy_traceevent_report(L, report, list, report->f.path);

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
