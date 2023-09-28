/*
** $Id: lmprof_lib.c $
** See Copyright Notice in lmprof_lib.h
*/
#define LUA_LIB

#include <stdint.h>
#include <inttypes.h>
#define LUA_LIB
#define LIB_NAME "lmprof"
#define MODULE_NAME "lmprof"
// include the Defold SDK
#include <dmsdk/sdk.h>
extern "C" {
  #include <dmsdk/lua/lua.h>
  // #include <dmsdk/lua/lauxlib.h>
  // #include <dmsdk/lua/lualib.h>
}
#include "lmprof_conf.h"

#include "collections/lmprof_hash.h"

#include "lmprof_state.h"
#include "lmprof.h"
#include "lmprof_report.h"
#include "lmprof_lib.h"
#include "script_bitop.h"

extern const char *const lmprof_mode_strings[];
extern const uint32_t lmprof_mode_codes[];

extern const char *const lmprof_option_strings[];
extern const uint32_t lmprof_option_codes[];

extern const char *const lmprof_state_strings[];
extern const uint32_t lmprof_state_codes[];

/* lua_State active function changed from int to struct in 5.2 */
#if LUA_VERSION_NUM > 501
  #define LUA_IS_TAILCALL(AR) ((AR)->event == LUA_HOOKTAILCALL)
#else
  #define LUA_IS_TAILCALL(ar) 0
#endif

/*
** {==================================================================
** Profiler Hooks
** ===================================================================
*/

/* Forward declare 'start' / 'stop' functions */
static int state_start(lua_State *L);
static int state_stop(lua_State *L);

/* Used on Lua callback; compare lua_CFunction references. */
#define PROFILE_IS_STOP(F) ((F) == lmprof_stop || (F) == state_stop)

/* Used when 'peeking' the call stack; compare identifiers. */
#define PROFILE_IS_START(F) ((F) == (lu_addr)lmprof_start || (F) == (lu_addr)state_start)

/*
** Helper to determine whether an action record or event code corresponds to a
** tailcall call.
*/
#if defined(LUA_HOOKTAILRET)
  #define PROFILE_TAIL_EVENT(A, I) ((A)->event == LUA_HOOKTAILRET)
#else
  #define PROFILE_TAIL_EVENT(A, I) ((I) != l_nullptr && (I)->tail_call)
#endif

/*
** Compute the difference between the current time and the last polled time
** stored by the profiler (thread.time). Append that difference to the running
** overhead counter.
*/
#define PROFILE_ADJUST_OVERHEAD(L, st)                   \
  LUA_MLM_BEGIN {                                        \
    const lu_time _t = LMPROF_TIME(st);                  \
    st->thread.r.overhead += (_t - st->thread.r.s.time); \
    st->thread.r.s.time = _t;                            \
  }                                                      \
  LUA_MLM_END

/*
** Perform a calibration, i.e., determine an estimation, preferably an
** underestimation, of the Lua function call overhead. The result can be used to
** increase profiling precision.
*/
static lu_time lmprof_calibrate(lua_State *L) {
  static const char *lua_code = "\
    do                           \
        local t = function() end \
        for i=1,10000000 do      \
            t()                  \
        end                      \
    end                          \
";

  if (luaL_loadstring(L, lua_code) == LUA_OK) {
    const lu_time start = LUA_TIME();
    if (lua_pcall(L, 0, 0, 0) == LUA_OK) {
      return lmprof_clock_diff(start, LUA_TIME()) / 10000000;
    }
    return luaL_error(L, "could not call calibration string");
  }
  return luaL_error(L, "could not load calibration string");
}

static void *alloc_hook(void *ud, void *ptr, size_t osize, size_t nsize) {
  lmprof_State *st = l_pcast(lmprof_State *, ud);

  size_t sz = 0;
  if (ptr != l_nullptr)
    sz = osize;
  if (nsize > sz && !BITFIELD_TEST(st->state, LMPROF_STATE_IGNORE_ALLOC))
    st->thread.r.s.allocated += (nsize - sz);
  if (nsize < sz && !BITFIELD_TEST(st->state, LMPROF_STATE_IGNORE_ALLOC))
    st->thread.r.s.deallocated += (sz - nsize);

  return st->hook.alloc.f(st->hook.alloc.ud, ptr, osize, nsize);
}

/*
** {==================================================================
** Graph Interface
** ===================================================================
*/

/* @HACK: Temporary helper macro for selecting 'parent' identifiers */
#define P_ID(ST, R) BITFIELD_TEST((ST)->conf, LMPROF_OPT_COMPRESS_GRAPH) ? (R)->f_id : (R)->r_id

/*
** Check for a 'stack mismatch', i.e., the function that invoked start() has
** left the profiler scope. The below logic must consider LMPROF_OPT_LOAD_STACK:
**
**  Enabled: 'start' will be placed onto the call_stack as it is the 'level 0'
**    activation record. Therefore, check if the 'start' function is being
**    popped from the call stack.
**
**  Disabled: When leaving the function that invoked 'start', the size of the
**    call stack will be zero and 'pop' will return NULL.
*/
static LUA_INLINE void check_stack_mismatch(lua_State *L, lmprof_State *st, lmprof_Stack *stack, lmprof_StackInst *inst, int allow_nil) {
  if (allow_nil && inst == l_nullptr)
    return;
  else if (!BITFIELD_TEST(st->conf, LMPROF_OPT_STACK_MISMATCH)) {
    if (inst == l_nullptr) {
      lmprof_error(L, st, "stop was not called at the same level as start");
      return;
    }
  }

  stack_clear_instance(stack, inst);
}

/* @TODO: Additional logic in cases of coroutine.yield/resume. */
static LUA_INLINE lmprof_State *graph_prehook(lua_State *L) {
  lmprof_State *st = lmprof_singleton(L);
 
  /*
  ** Profiler has stopped recording since it inherited its debughook.
  **
  ** @SEE Note on 'Debughook Inheritance'
  */
  if (st == l_nullptr
      /* New profiler with different configuration... */
      || BITFIELD_TEST(st->mode, LMPROF_CALLBACK_MASK | LMPROF_MODE_TIME)
      /* Invalid state */
      || BITFIELD_TEST(st->state, LMPROF_STATE_ERROR)
      || !BITFIELD_TEST(st->state, LMPROF_STATE_RUNNING)) {
        lua_sethook(L, l_nullptr, 0, 0); /* reset hook */
    return l_nullptr;
  }
  /*
  ** If configured for single threaded profiling and the invoking thread is not
  ** that thread.
  */
  else if (BITFIELD_TEST(st->mode, LMPROF_MODE_SINGLE_THREAD) && st->thread.main != L) {
    return l_nullptr;
  }
  /* Configured to ignore subsequent hook call. */
  else if (BITFIELD_TEST(st->state, LMPROF_STATE_IGNORE_CALL)) {
    BITFIELD_CLEAR(st->state, LMPROF_STATE_IGNORE_CALL);
    return l_nullptr;
  }

  st->thread.r.s.time = LMPROF_TIME(st);
  st->thread.r.overhead += st->i.calibration;
  BITFIELD_SET(st->state, LMPROF_STATE_IGNORE_ALLOC); /* disable alloc count inside hook */

  /*
  ** When instrumenting/tracing is enabled the call stack for the new thread
  ** must be fetched.
  */
  if (st->thread.state != L) {
    st->thread.state = L;
    st->thread.call_stack = l_nullptr;
    if (BITFIELD_TEST(st->mode, LMPROF_MODE_INSTRUMENT)) { /* Fetch the stack, create if needed */
      st->thread.call_stack = lmprof_thread_stacktable_get(L, st);
      if (st->thread.call_stack == l_nullptr) {
        lmprof_error(L, st, "could not allocate local stack");
        return l_nullptr;
      }

      st->thread.r.proc.tid = st->thread.call_stack->thread_identifier;
      PROFILE_ADJUST_OVERHEAD(L, st);
    }
  }
  return st;
}

/*
** @TODO: Change implementation to not use lua_lastlevel() and to iterate from 0
**  until lua_getstack fails.
*/
static int lmprof_sample_stack(lua_State *L, lmprof_State *st) {
  int level, last_line = 0;
  const int gc_disabled = BITFIELD_TEST(st->conf, LMPROF_OPT_GC_DISABLE);
  const int line_triples = BITFIELD_TEST(st->conf, LMPROF_OPT_LINE_FREQUENCY);

  /* Ensure the root has its count updated at least once. */
  lmprof_Record *record = lmprof_fetch_record(L, st, l_nullptr, LMPROF_RECORD_ID_ROOT, LMPROF_RECORD_ID_ROOT, 0);
  lu_addr last_fid = BITFIELD_TEST(st->conf, LMPROF_OPT_COMPRESS_GRAPH) ? LMPROF_RECORD_ID_ROOT : record->r_id;
  if (record->graph.count == 0)
    record->graph.count++;

  for (level = lua_lastlevel(L); level >= 0; --level) {
    lua_Debug debug = LMPROF_ZERO_STRUCT;
    if (lua_getstack(L, level, &debug)) {
      const lu_addr fid = lmprof_record_id(L, &debug, gc_disabled, l_nullptr);

      /*
      ** The current 'graph' sampling approach ensures that each non-leaf node
      ** in a sample has its "count" and "line_freq" updated at least once to
      ** reflect that at some point the function/line has been visited.
      **
      ** @TODO This can be improved.
      */

      /* Update function counts */
      record = lmprof_fetch_record(L, st, &debug, fid, last_fid, last_line);
      if (level == 0 || (level > 0 && record->graph.count == 0))
        record->graph.count++;

      /* Update line counts */
      if (record->graph.line_freq != l_nullptr && record->info.linedefined > 0) {
        const int diff = debug.currentline - record->info.linedefined;
        if (diff >= 0 && diff < record->graph.line_freq_size
            && (level == 0 || record->graph.line_freq[diff] == 0)) {
          record->graph.line_freq[diff]++;
        }
      }

      last_fid = BITFIELD_TEST(st->conf, LMPROF_OPT_COMPRESS_GRAPH) ? fid : record->r_id;
      if (line_triples)
        last_line = (debug.currentline >= 0) ? debug.currentline : 0;
    }
    else {
      LMPROF_LOG("%s lua_getstack failure!", __FUNCTION__);
      break;
    }
  }
  return LUA_OK;
}

static void graph_sample(lua_State *L, lua_Debug *ar) {
  lu_time time = 0;
  lmprof_State *st = graph_prehook(L);
  if (st == l_nullptr)
    return;

  switch (ar->event) {
    case LUA_HOOKCOUNT: { /* Sampling profiler: only update the 'count' fields */
      st->i.instr_count += st->i.mask_count;
      lmprof_sample_stack(L, st);
      break;
    }
    default:
      break;
  }

  BITFIELD_CLEAR(st->state, LMPROF_STATE_IGNORE_ALLOC); /* enable alloc count */

  time = LMPROF_TIME(st);
  st->thread.r.overhead += (time - st->thread.r.s.time);
  st->thread.r.s.time = time;
}

static void graph_instrument(lua_State *L, lua_Debug *ar) {
  lmprof_Stack *stack = l_nullptr;
  lmprof_State *st = graph_prehook(L);
  if (st == l_nullptr) {
    return;
  }
  stack = st->thread.call_stack;
  if (!stack) return;
  switch (ar->event) {
#if defined(LUA_HOOKTAILCALL)
    case LUA_HOOKTAILCALL:
#endif
case LUA_HOOKCALL: {
  lua_CFunction result = l_nullptr;
  const lu_addr fid = lmprof_record_id(L, ar, BITFIELD_TEST(st->conf, LMPROF_OPT_GC_DISABLE), &result);
  if (!PROFILE_IS_STOP(result)) {
    const lmprof_StackInst *parent = lmprof_stack_peek(stack);
    const lu_addr pid = (parent == l_nullptr) ? LMPROF_RECORD_ID_ROOT : P_ID(st, parent->graph.record);
    const int pid_lastLine = (parent == l_nullptr) ? 0 : parent->last_line;

    lmprof_Record *record = lmprof_fetch_record(L, st, ar, fid, pid, pid_lastLine);
    lmprof_StackInst *inst = lmprof_stack_measured_push(stack, record, &st->thread.r.s, LUA_IS_TAILCALL(ar));
    if (inst == l_nullptr) {
          lmprof_error(L, st, "profiler stack overflow");
          return;
        }
      }
      break;
    }
    /*
    ** Pop the current function and any potential tail calls (>= Lua 52). If Lua
    ** is returning from a "pcall" that threw an error or an assertion failure,
    ** the stack must be readjusted.
    */
#if defined(LUA_HOOKTAILRET)
    case LUA_HOOKTAILRET:
#endif
case LUA_HOOKRET: {
      lu_addr fid = 0, tail_return = 0;
      lmprof_StackInst *inst = (stack->head > 1) ? lmprof_stack_measured_pop(stack, &st->thread.r.s) : l_nullptr;
      if (!(tail_return = PROFILE_TAIL_EVENT(ar, inst))) {
        fid = lmprof_record_id(L, ar, BITFIELD_TEST(st->conf, LMPROF_OPT_GC_DISABLE), l_nullptr);
      }

      for (;
           inst != l_nullptr && (inst->tail_call || (!tail_return && inst->graph.record->f_id != fid));
           inst = (stack->head > 1) ? lmprof_stack_measured_pop(stack, &st->thread.r.s) : l_nullptr) {
        check_stack_mismatch(L, st, stack, inst, 0);
      }
      check_stack_mismatch(L, st, stack, inst, 1);
      break;
    }
    case LUA_HOOKCOUNT: {
      st->i.instr_count += st->i.mask_count;
      stack->instr_count += st->i.mask_count;
      stack->instr_last = st->thread.r.s.time;
      break;
    }
    case LUA_HOOKLINE: {
      lmprof_StackInst *inst = lmprof_stack_peek(stack);
      if (inst != l_nullptr) {
        inst->last_line = (ar->currentline >= 0) ? ar->currentline : 0;
        inst->last_line_instructions = stack->instr_count;

        /* Ensure the currentline is consistent with the activation record. */
        if (inst->graph.record->graph.line_freq != l_nullptr && inst->graph.record->info.linedefined > 0) {
          const int diff = ar->currentline - inst->graph.record->info.linedefined;
          if (diff >= 0 && diff < inst->graph.record->graph.line_freq_size) {
            inst->graph.record->graph.line_freq[diff]++;
          }
        }
      }
      break;
    }
    default: {
      lmprof_error(L, st, "lmprof unknown event");
      return;
    }
  }

  BITFIELD_CLEAR(st->state, LMPROF_STATE_IGNORE_ALLOC); /* enable alloc count */
  {
    lmprof_StackInst *inst = lmprof_stack_peek(stack);
    const lu_time time = LMPROF_TIME(st);
    const lu_time amount = (time - st->thread.r.s.time);

    st->thread.r.s.time = time;
    st->thread.r.overhead += amount;
    if (inst != l_nullptr)
      inst->graph.overhead += amount;
  }
}

/* }================================================================== */

/*
** {==================================================================
** Trace Event Interface
** ===================================================================
*/

/* Generic handler for TraceEvent routine operations. */
static LUA_INLINE int traceevent_routine(lua_State *L, lmprof_State *st, lmprof_EventProcess thread, int begin) {
  int lmproferrno = LUA_OK;
  if ((lmproferrno = st->i.trace.routine(L, st, thread, begin)) != LUA_OK)
    return lmprof_error(L, st, "Error: %s", traceevent_strerror(lmproferrno));
  return 1;
}

/* Generic handler for TraceEvent scope operations. */
static LUA_INLINE int traceevent_scope(lua_State *L, lmprof_State *st, lmprof_StackInst *inst, lmprof_EventMeasurement *r, int enter) {
  if (!BITFIELD_TEST(inst->trace.record->info.event, LMPROF_RECORD_IGNORED)) {
    int lmproferrno = LUA_OK;

    inst->trace.call = *r;
    if ((lmproferrno = st->i.trace.scope(L, st, inst, enter)) != LUA_OK)
      return lmprof_error(L, st, "Error: %s", traceevent_strerror(lmproferrno));
  }

  return 1;
}

/*
** Generate ENTER_SCOPE events for each function already in the stack.
**
** Returning one on success; zero otherwise. On failure this function will throw
** an lmprof_error which is technically a l_noret operation.
*/
static LUA_INLINE int traceevent_append_stack(lua_State *L, lmprof_State *st) {
  lmprof_EventMeasurement unit = st->thread.r;
  if (BITFIELD_TEST(st->mode, LMPROF_MODE_TRACE) && BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_DRAW_FRAME)) {
    lmprof_EventMeasurement frame = unit;
    frame.proc.pid = st->thread.mainproc.pid;
    frame.proc.tid = LMPROF_THREAD_BROWSER;
    traceevent_beginframe(l_pcast(TraceEventTimeline *, st->i.trace.arg), frame);
  }

  if (!BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_LAYOUT_SPLIT)) {
    size_t i;
    lmprof_Stack *stack = st->thread.call_stack;
    lmprof_EventProcess thread = st->thread.r.proc;
    thread.tid = stack->thread_identifier;

    traceevent_routine(L, st, thread, 1);
    for (i = 0; i < stack->head; ++i) { /* @TODO propagate errors better. */
      lmprof_StackInst *inst = &stack->stack[i];
      if (!traceevent_scope(L, st, inst, &unit, 1))
        return 0;
    }
  }
  return 1;
}

/*
** Generate EXIT_SCOPE events for each remaining function on the stack.
**
** Returning one on success; zero otherwise. On failure this function will throw
** an lmprof_error which is technically a l_noret operation.
*/
static LUA_INLINE int traceevent_clear_stack(lua_State *L, lmprof_State *st) {
  lmprof_EventMeasurement unit = st->thread.r;
  if (!BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_LAYOUT_SPLIT)) {
    size_t i;
    lmprof_Stack *stack = st->thread.call_stack;
    lmprof_EventProcess thread = st->thread.r.proc;
    thread.tid = stack->thread_identifier;

    for (i = 0; i < stack->head; ++i) { /* @TODO improve error propagation */
      lmprof_StackInst *inst = &stack->stack[i];
      if (!traceevent_scope(L, st, inst, &unit, 0))
        return 0;
    }
    traceevent_routine(L, st, thread, 0);
  }

  if (BITFIELD_TEST(st->mode, LMPROF_MODE_TRACE) && BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_DRAW_FRAME)) {
    lmprof_EventMeasurement frame = unit;
    frame.proc.pid = st->thread.mainproc.pid;
    frame.proc.tid = LMPROF_THREAD_BROWSER;
    traceevent_endframe(l_pcast(TraceEventTimeline *, st->i.trace.arg), frame);
  }
  return 1;
}

static LUA_INLINE lmprof_State *traceevent_prehook(lua_State *L) {
  /* @NOTE: See graph_prehook */
  lmprof_State *st = lmprof_singleton(L);
  if (st == l_nullptr
      || !BITFIELD_TEST(st->mode, LMPROF_CALLBACK_MASK)
      || BITFIELD_TEST(st->state, LMPROF_STATE_ERROR)
      || !BITFIELD_TEST(st->state, LMPROF_STATE_RUNNING)) {
    lua_sethook(L, l_nullptr, 0, 0);
    return l_nullptr;
  }
  else if (BITFIELD_TEST(st->mode, LMPROF_MODE_SINGLE_THREAD) && st->thread.main != L) {
    return l_nullptr;
  }
  else if (BITFIELD_TEST(st->state, LMPROF_STATE_PAUSED)) {
    lmprof_error(L, st, "profiler in a deferred state");
    return l_nullptr;
  }
  else if (BITFIELD_TEST(st->state, LMPROF_STATE_IGNORE_CALL)) {
    BITFIELD_CLEAR(st->state, LMPROF_STATE_IGNORE_CALL);
    return l_nullptr;
  }

  st->thread.r.s.time = LMPROF_TIME(st);
  st->thread.r.overhead += st->i.calibration;
  BITFIELD_SET(st->state, LMPROF_STATE_IGNORE_ALLOC);

  if (st->thread.state != L && BITFIELD_TEST(st->mode, LMPROF_MODE_INSTRUMENT)) {
    lmprof_Stack *stack = st->thread.call_stack;
    if (stack != l_nullptr && !traceevent_clear_stack(L, st))
      return l_nullptr;
    else if ((stack = lmprof_thread_stacktable_get(L, st)) == l_nullptr) {
      lmprof_error(L, st, "could not allocate local stack");
      return l_nullptr;
    }

    st->thread.state = L;
    st->thread.call_stack = stack;

    /* Only change the thread identifier when configured to 'split' the layout */
    if (BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_LAYOUT_SPLIT))
      st->thread.r.proc.tid = st->thread.call_stack->thread_identifier;

    if (!traceevent_append_stack(L, st))
      return l_nullptr;
  }
  else if (st->thread.state != L) {
    st->thread.state = L;
    st->thread.call_stack = l_nullptr;
  }

  PROFILE_ADJUST_OVERHEAD(L, st);
  return st;
}

static void traceevent_instrument(lua_State *L, lua_Debug *ar) {
  lmprof_Stack *stack = l_nullptr;
  lmprof_State *st = traceevent_prehook(L);
  if (st == l_nullptr)
    return;

  stack = st->thread.call_stack;
  switch (ar->event) {
#if defined(LUA_HOOKTAILCALL)
    case LUA_HOOKTAILCALL:
#endif
    case LUA_HOOKCALL: { /* push call onto stack */
      lua_CFunction result = l_nullptr;
      const lu_addr fid = lmprof_record_id(L, ar, BITFIELD_TEST(st->conf, LMPROF_OPT_GC_DISABLE), &result);
      if (!(PROFILE_IS_STOP(result))) {
        const lmprof_StackInst *parent = lmprof_stack_peek(stack);
        const lu_addr pid = (parent == l_nullptr) ? LMPROF_RECORD_ID_ROOT : parent->graph.record->f_id;

        lmprof_Record *record = lmprof_fetch_record(L, st, ar, fid, pid, 0);
        lmprof_StackInst *inst = lmprof_stack_event_push(stack, record, &st->thread.r, LUA_IS_TAILCALL(ar));
        if (inst == l_nullptr) {
          lmprof_error(L, st, "profiler stack overflow");
          return;
        }
        else if (!traceevent_scope(L, st, inst, &st->thread.r, 1)) {
          return; /* lua_Error already thrown. */
        }
      }
      break;
    }
#if defined(LUA_HOOKTAILRET)
    case LUA_HOOKTAILRET:
#endif
    case LUA_HOOKRET: {
      lu_addr fid = 0, tail_return = 0;
      lmprof_StackInst *inst = (stack->head > 1) ? lmprof_stack_pop(stack) : l_nullptr;
      if (!(tail_return = PROFILE_TAIL_EVENT(ar, inst))) {
        fid = lmprof_record_id(L, ar, BITFIELD_TEST(st->conf, LMPROF_OPT_GC_DISABLE), l_nullptr);
      }

      for (;
           inst != l_nullptr && (inst->tail_call || (!tail_return && inst->trace.record->f_id != fid));
           inst = (stack->head > 1) ? lmprof_stack_pop(stack) : l_nullptr) {
        traceevent_scope(L, st, inst, &st->thread.r, 0);
        check_stack_mismatch(L, st, stack, inst, 0);
      }

      /* LMPROF_OPT_LOAD_STACK may be disabled, stack *could* be empty. */
      if (inst != l_nullptr) {
        traceevent_scope(L, st, inst, &st->thread.r, 0);
        check_stack_mismatch(L, st, stack, inst, 1);
      }
      break;
    }
    case LUA_HOOKCOUNT: {
      lmprof_StackInst *inst = lmprof_stack_peek(stack);
      st->i.instr_count += st->i.mask_count;
      stack->instr_count += st->i.mask_count;
      if (inst != l_nullptr)
        st->i.trace.sample(L, st, inst, -1);

      stack->instr_last = st->thread.r.s.time;
      break;
    }
    case LUA_HOOKLINE: {
      lmprof_StackInst *inst = lmprof_stack_peek(stack);
      if (inst != l_nullptr) {
        st->i.trace.sample(L, st, inst, ar->currentline);

        inst->last_line = (ar->currentline >= 0) ? ar->currentline : 0;
        inst->last_line_instructions = st->i.instr_count;
      }
      break;
    }
    default: {
      lmprof_error(L, st, "lmprof unknown event");
      break;
    }
  }

  BITFIELD_CLEAR(st->state, LMPROF_STATE_IGNORE_ALLOC); /* prehook: enable alloc count */
  PROFILE_ADJUST_OVERHEAD(L, st); /* change in times = overhead */
}

LUA_API int lmprof_resume_execution(lua_State *L, lmprof_State *st) {
  if (st != l_nullptr
      && st->thread.call_stack != l_nullptr
      && BITFIELD_TEST(st->state, LMPROF_STATE_PAUSED)
      && BITFIELD_TEST(st->mode, LMPROF_MODE_INSTRUMENT)
      && BITFIELD_TEST(st->mode, LMPROF_CALLBACK_MASK)) {

    st->thread.r.s.time = LMPROF_TIME(st);
    if (traceevent_append_stack(L, st))
      BITFIELD_CLEAR(st->state, LMPROF_STATE_PAUSED);

    return 1;
  }
  return 0;
}

LUA_API int lmprof_pause_execution(lua_State *L, lmprof_State *st) {
  if (st != l_nullptr
      && st->thread.call_stack != l_nullptr
      && !BITFIELD_TEST(st->state, LMPROF_STATE_PAUSED)
      && BITFIELD_TEST(st->mode, LMPROF_MODE_INSTRUMENT)
      && BITFIELD_TEST(st->mode, LMPROF_CALLBACK_MASK)) {

    st->thread.r.s.time = LMPROF_TIME(st);
    if (traceevent_clear_stack(L, st))
      BITFIELD_SET(st->state, LMPROF_STATE_PAUSED);

    return 1;
  }
  return 0;
}

/* }================================================================== */

/*
** {==================================================================
** Profiler Helpers
** ===================================================================
*/

static void traceevent_ifree(lua_State *L, void *args) {
  UNUSED(L);
  timeline_free(l_pcast(TraceEventTimeline *, args));
}

static int traceevent_iroutine(lua_State *L, lmprof_State *st, lmprof_EventProcess thread, int begin) {
  TraceEventTimeline *list = l_pcast(TraceEventTimeline *, st->i.trace.arg);
  UNUSED(L); ((void)(thread));
  return begin ? traceevent_beginroutine(list, st->thread.r)
               : traceevent_endroutine(list, st->thread.r);
}

static int traceevent_iscope(lua_State *L, lmprof_State *st, lmprof_StackInst *inst, int enter) {
  TraceEventTimeline *list = l_pcast(TraceEventTimeline *, st->i.trace.arg);
  UNUSED(L);
  return enter ? traceevent_enterscope(list, &inst->trace)
               : traceevent_exitscope(list, &inst->trace);
}

static int traceevent_isample(lua_State *L, lmprof_State *st, lmprof_StackInst *inst, int line) {
  UNUSED(L);
  return traceevent_sample(l_pcast(TraceEventTimeline *, st->i.trace.arg), &inst->trace, st->thread.r, line);
}

static int traceevent_frame(lmprof_State *st, int begin_frame) {
  if (BITFIELD_TEST(st->mode, LMPROF_MODE_TRACE)) {
    BITFIELD_SET(st->state, LMPROF_STATE_IGNORE_ALLOC); /* disable alloc count */
    if (!BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_DRAW_FRAME)) {
      TraceEventTimeline *list = l_pcast(TraceEventTimeline *, st->i.trace.arg);
      lmprof_EventMeasurement frame = st->thread.r;
      frame.proc.pid = st->thread.mainproc.pid;
      frame.proc.tid = LMPROF_THREAD_BROWSER;
      frame.s.time = LMPROF_TIME(st);
      if (begin_frame)
        traceevent_beginframe(list, frame);
      else
        traceevent_endframe(list, frame);
    }
    BITFIELD_CLEAR(st->state, LMPROF_STATE_IGNORE_ALLOC); /* enable alloc count */
  }
  return LUA_OK;
}

LUA_API void lmprof_default_error(lua_State *L, lmprof_State *st) {
  if (st != l_nullptr) {
    lmprof_finalize_profiler(L, st, 0);
    lmprof_shutdown_profiler(L, st);
    if (BITFIELD_TEST(st->state, LMPROF_STATE_PERSISTENT))
      BITFIELD_CLEAR(st->state, LMPROF_STATE_ERROR);
  }
}

LUA_API int lmprof_initialize_default(lua_State *L, lmprof_State *st, int idx) {
  if (BITFIELD_TEST(st->mode, LMPROF_MODE_TIME)) {
    /* FALLTHROUGH */
  }
  else if (BITFIELD_TEST(st->mode, LMPROF_MODE_TRACE)) {
    TraceEventTimeline *list = timeline_new(&st->hook.alloc, l_cast(size_t, st->i.pageLimit));
    if (list == l_nullptr)
      return lmprof_error(L, st, "Unable to create a TraceEvent list");

    st->i.trace.arg = l_pcast(void *, list);
    st->i.trace.routine = traceevent_iroutine;
    st->i.trace.scope = traceevent_iscope;
    st->i.trace.sample = traceevent_isample;
    st->i.trace.free = traceevent_ifree;
  }
  else if (BITFIELD_TEST(st->mode, LMPROF_MODE_INSTRUMENT | LMPROF_MODE_MEMORY | LMPROF_MODE_SAMPLE)) {
    /* FALLTHROUGH */
  }
  else {
    return lmprof_error(L, st, "Unknown profile mode: %d", l_cast(int, st->mode));
  }

  /* lmprof_check_can_profile(L); Handled by root function. */
  return lmprof_initialize_only_hooks(L, st, idx);
}

LUA_API int lmprof_initialize_only_hooks(lua_State *L, lmprof_State *st, int idx) {
  const int abs_idx = lua_absindex(L, idx);

  lua_Hook call = l_nullptr;
  lua_Alloc memory = l_nullptr;
  if (BITFIELD_TEST(st->mode, LMPROF_MODE_TIME)) {
    /* FALLTHROUGH */
  }
  else if (BITFIELD_TEST(st->mode, LMPROF_CALLBACK_MASK)) {
    if (BITFIELD_TEST(st->mode, LMPROF_MODE_INSTRUMENT | LMPROF_MODE_MEMORY | LMPROF_MODE_SAMPLE)) {
      if (st->i.hash == l_nullptr)
        st->i.hash = lmprof_hash_create(&st->hook.alloc, st->i.hash_size);

      call = traceevent_instrument;
      if (BITFIELD_TEST(st->mode, LMPROF_MODE_MEMORY))
        memory = alloc_hook;
    }
    else {
      return lmprof_error(L, st, "Unknown trace mode: %d", l_cast(int, st->mode));
    }
  }
  else if (BITFIELD_TEST(st->mode, LMPROF_MODE_INSTRUMENT | LMPROF_MODE_MEMORY | LMPROF_MODE_SAMPLE)) {
    if (st->i.hash == l_nullptr)
      st->i.hash = lmprof_hash_create(&st->hook.alloc, st->i.hash_size);

    call = graph_instrument;
    if (BITFIELD_TEST(st->mode, LMPROF_MODE_SAMPLE) && !BITFIELD_TEST(st->mode, LMPROF_MODE_INSTRUMENT))
      call = graph_sample; /* Configured for sampling and not instrumenting */
    if (BITFIELD_TEST(st->mode, LMPROF_MODE_MEMORY)) /* @TODO: Memory only profiling */
      memory = alloc_hook;
  }
  else {
    return lmprof_error(L, st, "Unknown profile mode: %d", l_cast(int, st->mode));
  }
  /* lmprof_check_can_profile(L); Handled by root function. */
  switch (lmprof_initialize_profiler(L, st, abs_idx, call, memory)) {
    case LMPROF_STARTUP_ERROR:
      return luaL_error(L, "profiler in an invalid state");
    case LMPROF_STARTUP_ERROR_RUNNING:
      return luaL_error(L, "profiler state already running");
    case LMPROF_STARTUP_ERROR_SINGLETON:
      return luaL_error(L, "could not register profiler singleton");
    default:
      break;
    }
  return 1;
}

static int quit_profiler(lua_State *L, lmprof_State *st) {
  lmprof_finalize_profiler(L, st, 0);
  lmprof_shutdown_profiler(L, st);
  return 0;
}

static void pop_remaining_stacks(lua_State *L, lmprof_State *st) {
  luaL_checkstack(L, 4, __FUNCTION__);
  if (BITFIELD_TEST(st->mode, LMPROF_MODE_TIME)) {
    /* FALLTHROUGH */
  }
  else if (BITFIELD_TEST(st->mode, LMPROF_CALLBACK_MASK)) {
    /* FALLTHROUGH */
  }
  else if (BITFIELD_TEST(st->mode, LMPROF_MODE_INSTRUMENT | LMPROF_MODE_MEMORY | LMPROF_MODE_SAMPLE)) {
    st->thread.r.s.time = LMPROF_TIME(st);

    lmprof_thread_info(L, LMPROF_TAB_THREAD_STACKS); /* [..., thread_stacks] */
    lua_pushnil(L); /* [..., thread_stacks, nil] */
    while (lua_next(L, -2) != 0) { /* [..., thread_stacks, key, value] */
      lmprof_Stack *stack = l_pcast(lmprof_Stack *, lua_touserdata(L, -1));
      while (stack != l_nullptr && stack->head > 0) {
        lmprof_stack_measured_pop(stack, &st->thread.r.s);
      }

      lua_pop(L, 1); /* [..., thread_stacks, key] */
    }
    lua_pop(L, 1);
  }
}

/*
** Compute the output format (file, table, string) depending on input context.
** Storing the file path, if one exists, to out_file.
*/
static LUA_INLINE lmprof_ReportType report_type(lua_State *L, lmprof_State *st, int file_idx, const char **out_file) {
  lmprof_ReportType type = lTable;

  if (file_idx != 0 && lua_type(L, file_idx) == LUA_TSTRING) {
    type = lFile;
    if (out_file != l_nullptr)
      *out_file = lua_tostring(L, file_idx);
  }
  else if (BITFIELD_TEST(st->conf, LMPROF_OPT_REPORT_STRING)) {
    type = lBuffer;
  }

  return type;
}

static int stop_profiler(lua_State *L, lmprof_State *st, int file_idx) {
  const char *file = l_nullptr;
  const lmprof_ReportType type = report_type(L, st, file_idx, &file);
  lmprof_finalize_profiler(L, st, 1);
  lmprof_report(L, st, type, file);
  lmprof_shutdown_profiler(L, st);
  return 1;
}

static int stack_object_profiler(lua_State *L, lmprof_State *active_state, int forcedMode, int forcedOpts, int state_idx, int args_top) {
  lmprof_State *st;
#if defined(LMPROF_FILE_API) || !defined(LMPROF_DISABLE_OUTPUT_PATH)
  const int file_idx = state_idx + 2;
  const int mode_idx = state_idx + 3;
#else
  const int file_idx = 0;
  const int mode_idx = state_idx + 2;
#endif
  luaL_checkstack(L, 3, __FUNCTION__);

  st = active_state;
  if (active_state == l_nullptr) {
    const uint32_t mode = lmprof_parsemode(L, mode_idx, args_top);
    st = lmprof_new(L, mode | forcedMode, lmprof_default_error); /* [..., state] */
    BITFIELD_SET(st->conf, forcedOpts);

    /*
    ** Simplify the code below by ensuring the top of the stack contains the
    ** object to be profiled.
    */
    lua_pushvalue(L, args_top + 1); /* [...[, state], object] */
    lua_remove(L, args_top + 1);
    state_idx = lua_absindex(L, -2);
  }

  if (lmprof_initialize_default(L, st, state_idx)) {
    if (lua_pcall(L, 0, 0, 0) == LUA_OK) {
      const char *file = l_nullptr;
      const lmprof_ReportType type = report_type(L, st, file_idx, &file);

      lmprof_finalize_profiler(L, st, 1);
      lmprof_report(L, st, type, file);

      /* If the profiler state was created in this function; destroy it. */
      lmprof_shutdown_profiler(L, st);
      if (active_state == l_nullptr)
        lua_remove(L, -2); /* State */

      return 1;
    }
    else {
      const char *error = luaL_optstring(L, -1, "");
      lmprof_finalize_profiler(L, st, 0);
      lmprof_shutdown_profiler(L, st);
      return lmprof_error(L, st, "Profiling Error: %s", error);
    }
  }

  return lmprof_error(L, st, "Could not start profiler");
}

#if defined(LMPROF_FILE_API)
static int file_profiler(lua_State *L, lmprof_State *active_state, int state_idx) {
  const int top = lua_gettop(L);
  const char *lua_file = luaL_checkstring(L, state_idx + 1);
  if (luaL_loadfile(L, lua_file) == LUA_OK) /* [..., chunk] */
    return stack_object_profiler(L, active_state, LMPROF_MODE_NONE, LMPROF_OPT_NONE, state_idx, top);
  return luaL_error(L, "could not luaL_loadfile file <%s>", lua_file);
}
#endif

static int string_profiler(lua_State *L, lmprof_State *active_state, int state_idx) {
  const int top = lua_gettop(L);
  const char *lua_code = luaL_checkstring(L, state_idx + 1);
  if (luaL_loadstring(L, lua_code) == LUA_OK) /* [..., chunk] */
    return stack_object_profiler(L, active_state, LMPROF_MODE_NONE, LMPROF_OPT_NONE, state_idx, top);
  return luaL_error(L, "could not load code string");
}

static int function_profiler(lua_State *L, lmprof_State *active_state, int state_idx) {
  const int top = lua_gettop(L);
  luaL_checktype(L, state_idx + 1, LUA_TFUNCTION);
  lua_pushvalue(L, state_idx + 1);
  return stack_object_profiler(L, active_state, LMPROF_MODE_SINGLE_THREAD, LMPROF_OPT_NONE, state_idx, top);
}

/* }================================================================== */

/*
** {==================================================================
** State
** ===================================================================
*/

/*
** Debughook Inheritance: created threads inherit their debug hook from the
** thread that created it (Note: LuaJIT uses a globally defined hook).
**
** When wanting to profile an already active Lua state, with potentially many
** threads, three options are available for initializing debug hooks and
** preallocating thread/profiler specific data.
**
**  1. Do nothing. Only coroutines created by the profiled thread will be added
**  to the profile. All profiling structures will be allocated the first time it
**  generates a debug callback.
**
**  2. Using LMPROF_BUILTIN: Traverse the garbage collectors object space and
**  look for active threads.
**
**  3. LMPROF_TAB_THREAD_IDS: Traverse the 'name/id' table, a registry table
**  that associates human-readable labels to individual threads. As those
**  threads are referenced by the registry, they will not be garbage collected
**  and its assumed some 'scheduling' algorithm will manage their lifetime.
**
** It is also possible for a thread to inherit a debug hook from its parent and
** only begin its execution *after* the profiler has stopped. Unfortunately, the
** profiler is limited in how it can handle this edge-case.
**
**  On callback: If there is no active profiling state, reset the threads debug
**  hook. However, if there is a *new* profiling state, the threads execution
**  may clobber the new profilers state.
*/
#if !defined(LMPROF_BUILTIN)
  #define EACH_THREAD(L, C, ...)                       \
    LUA_MLM_BEGIN                                      \
    lmprof_thread_info((L), LMPROF_TAB_THREAD_IDS);    \
    lua_pushnil((L));                                  \
    while (lua_next((L), -2) != 0) {                   \
      lua_State *co = l_nullptr;                       \
      lua_pop((L), 1);                                 \
      if ((co = lua_tothread((L), -1)) != l_nullptr) { \
        (C)(co, __VA_ARGS__);                          \
      }                                                \
    }                                                  \
    lua_pop((L), 1);                                   \
    LUA_MLM_END
#endif

LUA_API lmprof_State *lmprof_new(lua_State *L, uint32_t mode, lmprof_Error error) {
  lmprof_State *st = l_pcast(lmprof_State *, lmprof_newuserdata(L, sizeof(lmprof_State)));
#if LUA_VERSION_NUM == 501
  luaL_getmetatable(L, LMPROF_LMPROF_STATE_METATABLE);
  lua_setmetatable(L, -2);
#else
  luaL_setmetatable(L, LMPROF_LMPROF_STATE_METATABLE);
#endif
  lmprof_initialize_state(L, st, mode, error);
  return st;
}

LUA_API int lmprof_initialize_profiler(lua_State *L, lmprof_State *st, int idx, lua_Hook fhook, lua_Alloc ahook) {
  if (BITFIELD_TEST(st->state, LMPROF_STATE_RUNNING))
    return LMPROF_STARTUP_ERROR_RUNNING;
  else if (BITFIELD_TEST(st->state, LMPROF_STATE_ERROR))
    return LMPROF_STARTUP_ERROR_RUNNING;
  else if (!lmprof_register_singleton(L, idx))
    return LMPROF_STARTUP_ERROR_SINGLETON;

  /* Clear 'state' flags that may have lingered from a previous profile */
  BITFIELD_CLEAR(st->state, LMPROF_STATE_GC_WAS_RUNNING | LMPROF_STATE_IGNORE_ALLOC | LMPROF_STATE_IGNORE_CALL);
  BITFIELD_SET(st->state, LMPROF_STATE_RUNNING | LMPROF_STATE_SETTING_UP);

  /* Reinitialize the clock if specified */
  if (BITFIELD_TEST(st->conf, LMPROF_OPT_CLOCK_INIT)) {
    lmprof_clock_init();
    BITFIELD_CLEAR(st->conf, LMPROF_OPT_CLOCK_INIT);
  }

  st->thread.main = L;
  st->thread.r.s.time = LMPROF_TIME(st);
  if (BITFIELD_TEST(st->mode, LMPROF_MODE_TRACE)) {
    TraceEventTimeline *list = l_pcast(TraceEventTimeline *, st->i.trace.arg);
    list->baseTime = st->thread.r.s.time;
  }
  /*
  ** LUA_GCISRUNNING was introduced in Lua 52. Therefore for previous Lua
  ** versions the garbage collector will be forced-stopped and forced-restarted
  ** after the profiling (regardless of the current garbage collector state).
  */
  if (BITFIELD_TEST(st->conf, LMPROF_OPT_GC_DISABLE)
#if LUA_VERSION_NUM >= 503
      && lua_gc(L, LUA_GCISRUNNING, 0)
#endif
  ) {
    BITFIELD_SET(st->state, LMPROF_STATE_GC_WAS_RUNNING);
    lua_gc(L, LUA_GCSTOP, 0);
  }
  if (fhook != l_nullptr) {
    int line_count = 0;

    /* @NOTE: This logic assumes 'st->mode' is valid according to lmprof_parsemode. */
    uint32_t flags = 0;
    if (BITFIELD_TEST(st->mode, LMPROF_MODE_INSTRUMENT | LMPROF_MODE_MEMORY))
      flags = LUA_MASKCALL | LUA_MASKRET;

    /* Enable MASKLINE events when profiling w/ TraceEvent mode. */
    if (BITFIELD_TEST(st->mode, LMPROF_MODE_LINE))
      flags |= LUA_MASKLINE;

    /*
    ** For TRACE mode ensure a 'sampler' callback has been supplied. See note in
    ** lmprof_parsemode.
    */
    if (BITFIELD_TEST(st->mode, LMPROF_MODE_SAMPLE)) {
      int valid = st->i.mask_count > 0;
      if (BITFIELD_TEST(st->mode, LMPROF_CALLBACK_MASK)) {
        valid = valid && st->i.trace.sample != l_nullptr
                && BITFIELD_TEST(st->mode, LMPROF_MODE_SINGLE_THREAD);
      }

      if (valid) {
        flags |= LUA_MASKCOUNT;
        line_count = l_cast(int, st->i.mask_count);
      }
    }

    st->hook.l_hook = fhook;
    st->hook.flags = (fhook == l_nullptr) ? 0 : flags;
    st->hook.line_count = (fhook == l_nullptr) ? 0 : line_count;
    if (BITFIELD_TEST(st->conf, LMPROF_OPT_GC_COUNT_INIT)
        && !BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_LAYOUT_SPLIT)) {
      const lu_size size = (l_cast(lu_size, lua_gc(L, LUA_GCCOUNT, 0)) << 10) + lua_gc(L, LUA_GCCOUNTB, 0);
      st->thread.r.s.allocated = size;
    }
    /* For all reachable coroutines: initialize their profiler hooks */
    lmprof_initialize_thread(L, st, l_nullptr);
    if (!BITFIELD_TEST(st->mode, LMPROF_MODE_SINGLE_THREAD)) {
#if defined(LMPROF_BUILTIN)
#if LUA_VERSION_NUM < 502
      EACH_THREAD(G(L)->rootgc, lmprof_initialize_thread, st, L);
  #else
      EACH_THREAD(G(L)->allgc, lmprof_initialize_thread, st, L);
  #endif
#else
      EACH_THREAD(L, lmprof_initialize_thread, st, L);
#endif
    }
  }
  if (BITFIELD_TEST(st->mode, LMPROF_MODE_MEMORY)) {
    lua_setallocf(L, ahook, l_pcast(void *, st));
  }
  BITFIELD_CLEAR(st->state, LMPROF_STATE_SETTING_UP);
  return LMPROF_STARTUP_OK;
}

LUA_API void lmprof_finalize_profiler(lua_State *L, lmprof_State *st, int pop_remaining) {
  if (BITFIELD_TEST(st->state, LMPROF_STATE_RUNNING)) {
    void *current;
    if (pop_remaining) {
      pop_remaining_stacks(L, st);
    }

    /* Restore alloc function if object being destroyed is the current obj */
    lua_getallocf(L, &current);
    if (current == st && BITFIELD_TEST(st->mode, LMPROF_MODE_MEMORY)) {
      lua_setallocf(L, st->hook.alloc.f, st->hook.alloc.ud);
    }

    /* Reset the garbage collector. */
    if (BITFIELD_TEST(st->state, LMPROF_STATE_GC_WAS_RUNNING)) {
      lua_gc(L, LUA_GCRESTART, 0);
    }

    BITFIELD_CLEAR(st->state, LMPROF_STATE_RUNNING | LMPROF_STATE_SETTING_UP | LMPROF_STATE_GC_WAS_RUNNING);

    /*
    ** For all reachable coroutines: shutdown their profiler states and reset
    ** their debug hooks.
    */
    if (st->hook.l_hook != l_nullptr) {
      lmprof_clear_thread(L, st, l_nullptr);
      if (!BITFIELD_TEST(st->mode, LMPROF_MODE_SINGLE_THREAD)) {
#if defined(LMPROF_BUILTIN)
  #if LUA_VERSION_NUM < 502
        EACH_THREAD(G(L)->rootgc, lmprof_clear_thread, st, L);
  #else
        EACH_THREAD(G(L)->allgc, lmprof_clear_thread, st, L);
  #endif
#else
        EACH_THREAD(L, lmprof_clear_thread, st, L);
#endif
      }
    }
  }
}

LUA_API void lmprof_shutdown_profiler(lua_State *L, lmprof_State *st) {
  /* See lmprof_initialize_default */
  if (BITFIELD_TEST(st->mode, LMPROF_CALLBACK_MASK)) {
    if (st->i.trace.free != l_nullptr)
      st->i.trace.free(L, st->i.trace.arg);

    st->i.trace.arg = l_nullptr;
    st->i.trace.free = l_nullptr;
    st->i.trace.scope = l_nullptr;
    st->i.trace.sample = l_nullptr;
  }

  lmprof_clear_state(L, st); /* Cleanup allocated memory */
  if (st == lmprof_singleton(L))
    lmprof_clear_singleton(L);
}

/* }================================================================== */

/*
** {==================================================================
** Userdata API
** ===================================================================
*/

static LUA_INLINE lmprof_State *state_get(lua_State *L, int idx) {
  return l_pcast(lmprof_State *, luaL_checkudata(L, idx, LMPROF_LMPROF_STATE_METATABLE));
}

static lmprof_State *state_get_valid(lua_State *L) {
  lmprof_State *st = state_get(L, 1);
  if (BITFIELD_TEST(st->state, LMPROF_STATE_ERROR)) {
    luaL_error(L, "profiler in an invalid state.");
    return l_nullptr;
  }
  return st;
}

static int state_getstate(lua_State *L) {
  lmprof_State *st = state_get(L, 1);
  uint32_t flags = lmprof_state_codes[luaL_checkoption(L, 2, l_nullptr, lmprof_state_strings)];
  lua_pushboolean(L, BITFIELD_TEST(st->state, flags) != 0);
  return 1;
}

static int state_getmode(lua_State *L) {
  lmprof_State *st = state_get(L, 1);
  const int top = lua_gettop(L);

  size_t i = 0;
  for (; lmprof_mode_strings[i] != l_nullptr; ++i) {
    if (BITFIELD_TEST(st->mode, lmprof_mode_codes[i]))
      lua_pushstring(L, lmprof_mode_strings[i]);
  }

  return lua_gettop(L) - top;
}

static int state_setmode(lua_State *L) {
  lmprof_State *st = state_get_valid(L);
  uint32_t mode = lmprof_mode_codes[luaL_checkoption(L, 2, "", lmprof_mode_strings)];

  int i;
  for (i = 3; i <= lua_gettop(L); ++i) { /* at least one option is required */
    mode |= lmprof_mode_codes[luaL_checkoption(L, i, "", lmprof_mode_strings)];
  }

  st->mode = mode;
  lua_pushvalue(L, 1);
  return 1; /* self */
}

static int state_getoption(lua_State *L) {
  lmprof_State *st = state_get(L, 1);
  const uint32_t opt = lmprof_option_codes[luaL_checkoption(L, 2, l_nullptr, lmprof_option_strings)];
  switch (opt) {
    case LMPROF_OPT_GC_DISABLE:
    case LMPROF_OPT_CLOCK_INIT:
    case LMPROF_OPT_CLOCK_MICRO:
    case LMPROF_OPT_LOAD_STACK:
    case LMPROF_OPT_STACK_MISMATCH:
    case LMPROF_OPT_COMPRESS_GRAPH:
    case LMPROF_OPT_GC_COUNT_INIT:
    case LMPROF_OPT_REPORT_VERBOSE:
    case LMPROF_OPT_REPORT_STRING:
    case LMPROF_OPT_LINE_FREQUENCY:
    case LMPROF_OPT_TRACE_IGNORE_YIELD:
    case LMPROF_OPT_TRACE_DRAW_FRAME:
    case LMPROF_OPT_TRACE_LAYOUT_SPLIT:
    case LMPROF_OPT_TRACE_ABOUT_TRACING:
    case LMPROF_OPT_TRACE_COMPRESS:
      lua_pushboolean(L, BITFIELD_TEST(st->conf, opt) != 0);
      break;
    case LMPROF_OPT_INSTRUCTION_COUNT:
      lua_pushinteger(L, l_cast(lua_Integer, st->i.mask_count));
      break;
    case LMPROF_OPT_HASH_SIZE:
      lua_pushinteger(L, l_cast(lua_Integer, st->i.hash_size));
      break;
    case LMPROF_OPT_TRACE_PROCESS:
      lua_pushinteger(L, st->thread.mainproc.pid);
      break;
    case LMPROF_OPT_TRACE_URL:
      lua_pushstring(L, (st->i.url == l_nullptr) ? TRACE_EVENT_DEFAULT_URL : st->i.url);
      break;
    case LMPROF_OPT_TRACE_NAME:
      lua_pushstring(L, (st->i.name == l_nullptr) ? TRACE_EVENT_DEFAULT_NAME : st->i.name);
      break;
    case LMPROF_OPT_TRACE_PAGELIMIT:
      lua_pushinteger(L, st->i.pageLimit);
      break;
    case LMPROF_OPT_TRACE_COUNTERS_FREQ:
      lua_pushinteger(L, st->i.counterFrequency);
      break;
    case LMPROF_OPT_TRACE_THRESHOLD:
      lua_pushinteger(L, l_cast(lua_Integer, st->i.event_threshold));
      break;
    default:
      lua_pushnil(L);
      break;
  }
  return 1;
}

static int state_setoption(lua_State *L) {
  const char *str = l_nullptr;

  lmprof_State *st = state_get_valid(L);
  const uint32_t opt = lmprof_option_codes[luaL_checkoption(L, 2, l_nullptr, lmprof_option_strings)];
  switch (opt) {
    case LMPROF_OPT_GC_DISABLE:
    case LMPROF_OPT_CLOCK_INIT:
    case LMPROF_OPT_CLOCK_MICRO:
    case LMPROF_OPT_LOAD_STACK:
    case LMPROF_OPT_STACK_MISMATCH:
    case LMPROF_OPT_COMPRESS_GRAPH:
    case LMPROF_OPT_GC_COUNT_INIT:
    case LMPROF_OPT_REPORT_VERBOSE:
    case LMPROF_OPT_REPORT_STRING:
    case LMPROF_OPT_LINE_FREQUENCY:
    case LMPROF_OPT_TRACE_IGNORE_YIELD:
    case LMPROF_OPT_TRACE_DRAW_FRAME:
    case LMPROF_OPT_TRACE_LAYOUT_SPLIT:
    case LMPROF_OPT_TRACE_ABOUT_TRACING:
    case LMPROF_OPT_TRACE_COMPRESS: {
      luaL_checktype(L, 3, LUA_TBOOLEAN);
      st->conf = lua_toboolean(L, 3) ? (st->conf | opt) : (st->conf & ~opt);
      break;
    }
    case LMPROF_OPT_INSTRUCTION_COUNT: {
      const lua_Integer count = luaL_checkinteger(L, 3);
      if (count > 0) {
        st->i.mask_count = l_cast(int, count);
        break;
      }
      return luaL_error(L, "instruction count less-than/equal to zero");
    }
    case LMPROF_OPT_HASH_SIZE: {
      const lua_Integer count = luaL_checkinteger(L, 3);
      if (count >= 1 && count <= LMPROF_HASH_MAXSIZE) {
        st->i.hash_size = l_cast(size_t, count);
        break;
      }
      return luaL_error(L, "hashtable size is less-than/equal to zero");
    }
    case LMPROF_OPT_TRACE_PROCESS: {
      st->thread.mainproc.pid = luaL_checkinteger(L, 3);
      break;
    }
    case LMPROF_OPT_TRACE_URL: {
      if (st->i.url != l_nullptr)
        lmprof_strdup_free(&st->hook.alloc, st->i.url, 0);

      st->i.url = l_nullptr;
      if ((str = lua_tostring(L, 3)) != l_nullptr)
        st->i.url = lmprof_strdup(&st->hook.alloc, str, 0);
      break;
    }
    case LMPROF_OPT_TRACE_NAME: {
      if (st->i.name != l_nullptr)
        lmprof_strdup_free(&st->hook.alloc, st->i.name, 0);

      st->i.name = l_nullptr;
      if ((str = lua_tostring(L, 3)) != l_nullptr)
        st->i.name = lmprof_strdup(&st->hook.alloc, str, 0);
      break;
    }
    case LMPROF_OPT_TRACE_PAGELIMIT: {
      st->i.pageLimit = l_cast(size_t, luaL_checkinteger(L, 3));
      break;
    }
    case LMPROF_OPT_TRACE_COUNTERS_FREQ: {
      st->i.counterFrequency = luaL_checkinteger(L, 3);
      break;
    }
    case LMPROF_OPT_TRACE_THRESHOLD: {
      const lua_Integer max_threshold = 1024 * 1024;
      const lua_Integer threshold = luaL_checkinteger(L, 2);
      if (0 <= threshold && threshold <= max_threshold) {
        st->i.event_threshold = l_cast(lu_time, threshold);
        break;
      }
      return luaL_error(L, "threshold not within [0, %d]", (int)max_threshold);
    }
    default:
      break;
  }

  lua_pushvalue(L, 1);
  return 1;
}

static int state_string(lua_State *L) {
  lmprof_State *st = state_get(L, 1);
  if (st != l_nullptr) {
    const char *type = "Profiler";
    const char *state = "Active";
    if (BITFIELD_TEST(st->mode, LMPROF_MODE_TRACE))
      type = "TraceEventProfiler";
    else if (BITFIELD_TEST(st->mode, LMPROF_MODE_EXT_CALLBACK))
      type = "CallbackProfiler";

    if (BITFIELD_TEST(st->state, LMPROF_STATE_ERROR))
      state = "Error";
    else if (!BITFIELD_TEST(st->state, LMPROF_STATE_RUNNING))
      state = "Inactive";

    lua_pushfstring(L, "%s<%s>", type, state);
  }
  else {
    lua_pushliteral(L, "Unknown");
  }
  return 1;
}

static int state_gc(lua_State *L) {
  lmprof_State *st = state_get(L, 1);
  BITFIELD_CLEAR(st->state, LMPROF_STATE_PERSISTENT);
  lmprof_finalize_profiler(L, st, 0);
  lmprof_shutdown_profiler(L, st); /* Cleanup allocated memory */
  return 0;
}

static int state_close(lua_State *L) {
  state_gc(L);
  lua_pushnil(L); /* Preempt the metatable, ensuring _gc is not invoked again */
  lua_setmetatable(L, 1);
  return 0;
}

static int state_start(lua_State *L) {
  lmprof_State *st = state_get_valid(L); /* Ensure a valid state exists */
  lmprof_check_can_profile(L);
  lmprof_initialize_default(L, st, 1);
  lua_pushvalue(L, 1);
  return 1; /* self */
}

static int state_stop(lua_State *L) {
  lmprof_State *st = state_get(L, 1);
  if (st == lmprof_singleton(L)) {
#if defined(LMPROF_FILE_API) || !defined(LMPROF_DISABLE_OUTPUT_PATH)
    return stop_profiler(L, st, 2);
#else
    return stop_profiler(L, st, 0);
#endif
  }

  return luaL_error(L, "Could not stop profiler: profiler state inactive");
}

static int state_quit(lua_State *L) {
  lmprof_State *st = state_get(L, 1);
  if (st == lmprof_singleton(L))
    return quit_profiler(L, st);

  return 0;
}

static int state_profile_file(lua_State *L) {
#if defined(LMPROF_FILE_API)
  lmprof_State *st = state_get_valid(L);
  lmprof_check_can_profile(L);
  return file_profiler(L, st, 1);
#else
  return luaL_error(L, "luaL_loadfile support not enabled");
#endif
}

static int state_profile_string(lua_State *L) {
  lmprof_State *st = state_get_valid(L);
  lmprof_check_can_profile(L);
  return string_profiler(L, st, 1);
}

static int state_profile_function(lua_State *L) {
  lmprof_State *st = state_get_valid(L);
  lmprof_check_can_profile(L);
  return function_profiler(L, st, 1);
}

static int state_event_beginframe(lua_State *L) {
  lmprof_State *st = state_get_valid(L);
  if (BITFIELD_TEST(st->state, LMPROF_STATE_RUNNING)) {
    traceevent_frame(st, 1);
    lua_pushvalue(L, 1);
    return 1; /* self */
  }
  return luaL_error(L, "invalid profiler state");
}

static int state_event_endframe(lua_State *L) {
  lmprof_State *st = state_get_valid(L);
  if (BITFIELD_TEST(st->state, LMPROF_STATE_RUNNING)) {
    traceevent_frame(st, 0);
    lua_pushvalue(L, 1);
    return 1; /* self */
  }
  return luaL_error(L, "invalid profiler state");
}

/*
@@ LMPROF_RAW_CALIBRATION: Do not modify the calibration overhead. By default the
** calibration data is halved to ensure most/if-not-all potential variability is
** accounted for.
*/
static int state_calibrate(lua_State *L) {
  lmprof_State *st = state_get_valid(L);
  const lu_time overhead = lmprof_calibrate(L);
#if defined(LMPROF_RAW_CALIBRATION)
  st->i.calibration = overhead;
#else
  st->i.calibration = overhead >> 1;
#endif
  lua_pushvalue(L, 1);
  return 1; /* self */
}

static void lmprof_state_initialize(lua_State *L) {
  static const luaL_Reg metameth[] = {
    { "start", state_start },
    { "stop", state_stop },
    { "quit", state_quit },
    { "calibrate", state_calibrate },
    /* Profiler options */
    { "get_state", state_getstate },
    { "get_option", state_getoption },
    { "set_option", state_setoption },
    { "get_mode", state_getmode },
    { "set_mode", state_setmode },
    /* Additional profiler inputs */
    { "file", state_profile_file },
    { "string", state_profile_string },
    { "func", state_profile_function },
    /* Frame event generation. */
    { "begin_frame", state_event_beginframe }, /* timeline.frame.BeginFrame */
    { "end_frame", state_event_endframe },
    /* Metamethods */
    { "__gc", state_gc },
    { "__close", state_close },
    { "__tostring", state_string },
    { "__index", l_nullptr }, /* placeholder */
    { l_nullptr, l_nullptr }
  };

  if (luaL_newmetatable(L, LMPROF_LMPROF_STATE_METATABLE)) {
#if LUA_VERSION_NUM == 501
    luaL_register(L, l_nullptr, metameth);
#else
    luaL_setfuncs(L, metameth, 0);
#endif
    lua_pushvalue(L, -1); /* push metatable */
    lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  }
  lua_pop(L, 1); /* pop metatable */
}

/* }================================================================== */

/*
** {==================================================================
** MODULE API
** ===================================================================
*/
#if defined(__cplusplus)
extern "C" {
#endif

LUALIB_API int lmprof_create(lua_State *L) {
  const uint32_t mode = lmprof_parsemode(L, 1, lua_gettop(L));
  lmprof_State *st = lmprof_new(L, mode, lmprof_default_error);
  if (st != l_nullptr) {
    BITFIELD_SET(st->state, LMPROF_STATE_PERSISTENT);
  }

  return 1;
}

LUALIB_API int lmprof_start(lua_State *L) {
  lmprof_State *st = l_nullptr;
  const uint32_t mode = lmprof_parsemode(L, 1, lua_gettop(L));
  lmprof_check_can_profile(L);
  if ((st = lmprof_new(L, mode, lmprof_default_error)) != l_nullptr) { /* [..., profiler_state] */
    BITFIELD_CLEAR(st->state, LMPROF_STATE_PERSISTENT);
    lmprof_initialize_default(L, st, -1);
  }
  return 1;
}

LUALIB_API int lmprof_stop(lua_State *L) {
  lmprof_State *st = lmprof_singleton(L);
  if (st != l_nullptr) {
#if defined(LMPROF_FILE_API) || !defined(LMPROF_DISABLE_OUTPUT_PATH)
    return stop_profiler(L, st, 1);
#else
    return stop_profiler(L, st, 0);
#endif
  }

  return luaL_error(L, "Could not stop profiler: profiler state does not exist.");
}

LUALIB_API int lmprof_quit(lua_State *L) {
  lmprof_State *st = lmprof_singleton(L);
  if (st != l_nullptr)
    return quit_profiler(L, st);

  return 0;
}

LUALIB_API int lmprof_profile_file(lua_State *L) {
#if defined(LMPROF_FILE_API)
  lmprof_check_can_profile(L);
  return file_profiler(L, l_nullptr, 0);
#else
  return luaL_error(L, "luaL_loadfile support not enabled");
#endif
}

LUALIB_API int lmprof_profile_string(lua_State *L) {
  lmprof_check_can_profile(L);
  return string_profiler(L, l_nullptr, 0);
}

LUALIB_API int lmprof_profile_function(lua_State *L) {
  lmprof_check_can_profile(L);
  return function_profiler(L, l_nullptr, 0);
}

LUALIB_API int lchrome_trace_event_beginframe(lua_State *L) {
  lmprof_State *st = lmprof_singleton(L);
  if (st != l_nullptr && BITFIELD_TEST(st->state, LMPROF_STATE_RUNNING)
      && !BITFIELD_TEST(st->state, LMPROF_STATE_ERROR)) {
    traceevent_frame(st, 1);
    return 0;
  }
  return luaL_error(L, "invalid profiler state");
}

LUALIB_API int lchrome_trace_event_endframe(lua_State *L) {
  lmprof_State *st = lmprof_singleton(L);
  if (st != l_nullptr && BITFIELD_TEST(st->state, LMPROF_STATE_RUNNING)
      && !BITFIELD_TEST(st->state, LMPROF_STATE_ERROR)) {
    traceevent_frame(st, 0);
    return 0;
  }
  return luaL_error(L, "invalid profiler state");
}

#if defined(_DEBUG)
static int estimate_call_time(lua_State *L) {
  lua_pushinteger(L, l_cast(lua_Integer, lmprof_calibrate(L)));
  return 1;
}
#endif

LUAMOD_API int luaopen_lmprof(lua_State *L) {
  static const luaL_Reg lmproflib[] = {
    { "create", lmprof_create },
    { "start", lmprof_start },
    { "stop", lmprof_stop },
    { "quit", lmprof_quit },
    /* Global profiler options */
    { "set_option", lmprof_set_option },
    { "get_option", lmprof_get_option },
    { "time_unit", lmprof_get_timeunit },
    { "has_io", lmprof_get_has_io },
    /* Additional profiler inputs.*/
    { "file", lmprof_profile_file },
    { "string", lmprof_profile_string },
    { "func", lmprof_profile_function },
    /* IgnoreTable */
    { "ignore", lmprof_ignored_function_add },
    { "unignore", lmprof_ignored_function_remove },
    { "is_ignored", lmprof_is_ignored_function },
    /* NameTable */
    { "get_name", lmprof_get_name }, /* get thread name*/
    { "set_name", lmprof_set_name }, /* name a thread */
    /* Frame event generation. */
    { "begin_frame", lchrome_trace_event_beginframe }, /* timeline.frame.BeginFrame */
    { "end_frame", lchrome_trace_event_endframe },
    /* DEBUG */
#if defined(_DEBUG)
    { "call_time", estimate_call_time },
#endif
    { l_nullptr, l_nullptr }
  };

  lmprof_clock_init();
  lmprof_record_initialize(L);
  lmprof_report_initialize(L);
  lmprof_thread_stacks_initialize(L);
  lmprof_state_initialize(L);
#if LUA_VERSION_NUM == 501
  luaL_register(L, LMPROF_NAME, lmproflib);
#elif LUA_VERSION_NUM >= 502 && LUA_VERSION_NUM <= 504
  luaL_newlib(L, lmproflib);
#else
  #error unsupported Lua version
#endif
  return 1;
}

#if defined(__cplusplus)
}
#endif
/* }================================================================== */



// static void LuaInit(lua_State* L)
// {
//     int top = lua_gettop(L);
// 
//     // Register lua names
//     luaL_register(L, MODULE_NAME, lmproflib);
// 
//     lua_pop(L, 1);
//     assert(top == lua_gettop(L));
// }

dmExtension::Result AppInitializeMyExtension(dmExtension::AppParams* params)
{
    dmLogInfo("AppInitializeMyExtension\n");
    return dmExtension::RESULT_OK;
}

dmExtension::Result InitializeMyExtension(dmExtension::Params* params)
{
    // Init Lua
    // LuaInit(params->m_L);
    luaopen_lmprof(params->m_L);
    lua_pop(params->m_L, 1);
    dmLogInfo("Registered %s Extension\n", MODULE_NAME);
    return dmExtension::RESULT_OK;
}

dmExtension::Result AppFinalizeMyExtension(dmExtension::AppParams* params)
{
    dmLogInfo("AppFinalizeMyExtension\n");
    return dmExtension::RESULT_OK;
}

dmExtension::Result FinalizeMyExtension(dmExtension::Params* params)
{
    dmLogInfo("FinalizeMyExtension\n");
    return dmExtension::RESULT_OK;
}

// dmExtension::Result OnUpdateMyExtension(dmExtension::Params* params)
// {
//     dmLogInfo("OnUpdateMyExtension\n");
//     return dmExtension::RESULT_OK;
// }


// Defold SDK uses a macro for setting up extension entry points:
//
// DM_DECLARE_EXTENSION(symbol, name, app_init, app_final, init, update, on_event, final)

// MyExtension is the C++ symbol that holds all relevant extension data.
// It must match the name field in the `ext.manifest`
DM_DECLARE_EXTENSION(lmprof, LIB_NAME, AppInitializeMyExtension, AppFinalizeMyExtension, InitializeMyExtension, NULL, NULL, FinalizeMyExtension)

