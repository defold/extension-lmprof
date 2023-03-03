/*
** $Id: lmprof.h $
** Internal Profiling Definitions
** See Copyright Notice in lmprof_lib.h
*/
#ifndef lmprof_h
#define lmprof_h

#include "lmprof_conf.h"
#include "lmprof_state.h"

#include "collections/lmprof_stack.h"
#include "collections/lmprof_record.h"

#define LMPROF "lmprof"
#define LMPROF_PROFILER_SINGLETON "lmprof_singleton"

/* LMPROF_SUBTABLE Fields */
#define LMPROF_DEBUG_HOOK 1
#define LMPROF_FLAGS 2
#define LMPROF_THRESHOLD 3
#define LMPROF_HOOK_COUNT 4
#define LMPROF_HASHTABLE_SIZE 5
#define LMPROF_THREAD_COUNTER 6
#define LMPROF_PROCESS 7
#define LMPROF_PROFILE_NAME 8
#define LMPROF_URL 9
#define LMPROF_PAGE_LIMIT 10
#define LMPROF_COUNTERS_FREQ 11

/* Tables */
#define LMPROF_TAB_FUNC_IGNORE 12
#define LMPROF_TAB_THREAD_NAMES 13
#define LMPROF_TAB_THREAD_IDS 14
#define LMPROF_TAB_THREAD_STACKS 15

#define TRACE_EVENT_COUNTER_FREQ 20 /* Default UpdateCounters output frequency */
#define TRACE_EVENT_DEFAULT_PAGE_LIMIT 0 /* Maximum amount of pages in bytes (zero = infinite) */
#define TRACE_EVENT_DEFAULT_THRESHOLD 1 /* Default compression threshold: microseconds */
#define TRACE_EVENT_DEFAULT_NAME "" /* Default frame/FADE name */
#define TRACE_EVENT_DEFAULT_URL "" /* Default frame/FADE url */

/*
@@ LMPROF_BUILTIN: When enabled include/expose internal Lua headers lobject.h,
**  lgc.h, and lstate.h to improve, or expand, profiler functionality. For
**  example:
**
**  1. lmprof_record can reference the LClosure::p instead of creating an
**    aggregate identifier that hashes the fields of an activation record.
**
**  2. When a profiler has 'started', it becomes possible to iterate over the
**    current object space, searching for coroutines, and preallocate any data
**    structures used for profiling (e.g., call stacks).
**
** Note for ELF builds of Lua will use __attribute__((visibility("hidden"))) so
** only struct definitions and macros *should* be used.
*/
#if defined(LMPROF_BUILTIN)
LUA_EXTERN_BEGIN
  #include <lobject.h>
  #include <lgc.h>
  #include <lstate.h>
LUA_EXTERN_END

  /* Backwards compatible macros for iterating over the over the object space. */
  #if LUA_VERSION_NUM >= 503
    #define T_NEXT(p) (p)->next
    #define T_TYPE(p) (p)->tt
    #if !defined(LUA_VTHREAD) /* LUA_VERSION_NUM == 503 */
      #define LUA_VTHREAD LUA_TTHREAD
    #endif
  #else
    #define T_NEXT(p) (p)->gch.next
    #define T_TYPE(p) (p)->gch.tt
    #define LUA_VTHREAD LUA_TTHREAD
  #endif

  /* Traverse over a "collectible" list, looking for all active Lua threads */
  #define EACH_THREAD(I, C, ...)      \
    LUA_MLM_BEGIN                     \
    GCObject *p;                      \
    for (p = (I); p; p = T_NEXT(p)) { \
      if (T_TYPE(p) == LUA_VTHREAD) { \
        (C)(gco2th(p), __VA_ARGS__);  \
      }                               \
    }                                 \
    LUA_MLM_END

#endif

/*
** Generate a profiling error. This function is an extension of lua_error/luaL_error
** and is expected to never return.
*/
LUAI_FUNC int lmprof_error(lua_State *L, lmprof_State *st, const char *fmt, ...);

/*
** {==================================================================
** State
** ===================================================================
*/

/*
** Parse a sequence of strings between [index, lua_gettop(L)] that correspond to
** profiling MODE_ flags. Returning the desired profiling mode configuration.
*/
LUAI_FUNC uint32_t lmprof_parsemode(lua_State *L, int index, int top);

/* Check if the given Lua state can be profiled, throwing a lua_error on failure. */
LUAI_FUNC int lmprof_check_can_profile(lua_State *L);

/* Initialize profiler state with globally defined values */
LUAI_FUNC int lmprof_initialize_state(lua_State *L, lmprof_State *st, uint32_t mode, lmprof_Error error);

/* Reset the profiler to its initial state, deallocating any intermediate profile data. */
LUAI_FUNC int lmprof_clear_state(lua_State *L, lmprof_State *st);

/* Initialize the debug/profiling looks for the given Lua thread. */
LUAI_FUNC void lmprof_initialize_thread(lua_State *L, lmprof_State *st, lua_State *ignore);

/* Clear the debug/profiling hooks for the given Lua thread. */
LUAI_FUNC void lmprof_clear_thread(lua_State *L, lmprof_State *st, lua_State *ignore);

/* }================================================================== */

/*
** {==================================================================
** Singleton
** ===================================================================
*/

/* Register a global profiling state. Returning true on success, false otherwise. */
LUAI_FUNC int lmprof_register_singleton(lua_State *L, int idx);

/* Explicitly clear the global profiling state. */
LUAI_FUNC void lmprof_clear_singleton(lua_State *L);

/*
** Ensure no scripts have manipulated the Lua state in a way that would render
** the profiler single invalid. Returning 0 on success and an error-code otherwise.
*/
LUAI_FUNC int lmprof_verify_singleton(lua_State *L, lmprof_State *st);

/* }================================================================== */

/*
** {==================================================================
**  Threading
**
** Additional data associated with profiled lua_State instances. To ensure the
** lua_State does not get garbage collected during profiling, all data is stored
** in the registry table.
** ===================================================================
*/

/* Initialize meta-definitions for light-userdata lmprof_Stacks */
LUAI_FUNC void lmprof_thread_stacks_initialize(lua_State *L);

/*
** Return a unique integer identifier (e.g., if the garbage collector is
** disabled when profiling, the pointer can be used as an identifier) associated
** with the given lua_State. These values are primary used by lmprof_EventProcess.
*/
LUAI_FUNC lua_Integer lmprof_thread_identifier(lua_State *L);

/*
** Fetch, allocating if necessary, a 'lmprof_Stack' instance associated with the
** provided lua_State.
**
** Each state having its own profile stack ensures multiple coroutines (and the
** transfer of execution) may be profiled without issue. The previous lmprof
** implementation struggled with multiple coroutines, e.g.,
**
**    coroutine_1: "does something and yield"
**    coroutine_2: "does something then resumes couroutine_1"
**    main: resume(coroutine_1) then resume(coroutine_2).
**
** would leave the single profile stack in an invalid state.
*/
LUAI_FUNC lmprof_Stack *lmprof_thread_stacktable_get(lua_State *L, lmprof_State *st);

/* @HACK: Push a thread-info table onto the stack (to allow lua_next iteration over it) */
LUAI_FUNC void lmprof_thread_info(lua_State *L, int tab_id);

/* }================================================================== */

/*
** {==================================================================
** Functions
** ===================================================================
*/

/* Fetch a trace record. */
LUAI_FUNC lmprof_Record *lmprof_fetch_record(lua_State *L, lmprof_State *st, lua_Debug *ar, lu_addr fid, lu_addr pid, int p_currentline);

/*
** Return true if the value at the provided index is a function and is
** configured, to be ignored by the profiler.
*/
LUAI_FUNC int lmprof_function_is_ignored(lua_State *L, int idx);

/* }================================================================== */

#endif
