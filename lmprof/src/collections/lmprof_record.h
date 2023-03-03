/*
** $Id: lmprof_record.h $
** See Copyright Notice in lmprof_lib.h
*/
#ifndef lmprof_record_h
#define lmprof_record_h

#include "../lmprof_conf.h"

/* lua_getinfo flags */
#define DEBUG_NAME "n"
#define DEBUG_TAIL "t"
#define DEBUG_LINE "l"
#define DEBUG_SOURCE "S"
#define DEBUG_LINES "L"
#define DEBUG_FUNCTION "f"
#define DEBUG_FUNCTION_POP ">"

/* Available lua_getinfo flags that *do not* push objects onto the stack. */
#if LUA_VERSION_NUM == 501
  #define DEBUG_IMMUTABLE "lnSu"
  #define DEBUG_IMMUTABLE_NO_NAME "lSu"
#elif LUA_VERSION_NUM == 502 || LUA_VERSION_NUM == 503
  #define DEBUG_IMMUTABLE "lnSut"
  #define DEBUG_IMMUTABLE_NO_NAME "lSut"
#elif LUA_VERSION_NUM == 504
  #define DEBUG_IMMUTABLE "lnSutr"
  #define DEBUG_IMMUTABLE_NO_NAME "lSutr"
#else
  #error unsupported Lua version
#endif

/*
** The 'event' field of lmprof_FunctionInfo is used to represent a bitfield
** with the corresponding flags:
*/
#define LMPROF_RECORD_USERDATA         0x1 /* Allocated as userdata; memory management not required */
#define LMPROF_RECORD_ROOT             0x2 /* TraceEvent has '__metaProcess'; do not report root records */
#define LMPROF_RECORD_CCLOSURE         0x4 /* Recored represents a C Closure */
#define LMPROF_RECORD_IGNORED          0x8 /* Registered as 'ignored' */
#define LMPROF_RECORD_REPORTED  0x80000000 /* A callgrind specific flag to enable name compression */

/* Record userdata metatable */
#define LMPROF_RECORD_METATABLE "lmprof_record"

/* Helper for sanitizing names */
#define LMPROF_RECORD_NAME(n, o) (((n) == l_nullptr) ? (o) : (n))
#define LMPROF_RECORD_HAS_NAME(I) ((I)->name != l_nullptr || BITFIELD_TEST((I)->event, LMPROF_RECORD_IGNORED))

/*
** An extension of lua_Debug (re-purposing the struct for simplicity) for shared
** and formatted function definitions.
*/
typedef lua_Debug lmprof_FunctionInfo;

/*
** A formatted function/activation record, i.e., an extension of lua_Debug that
** incorporates profiling statistics.
**
** The record ID (r_id) is a unique identifier used to support features like
** 'Name Compression' in callgrind.
*/
typedef struct lmprof_Record {
  lu_addr r_id; /* global record identifier (a unique value) */
  lu_addr f_id; /* address/identifier of function */
  lu_addr p_id; /* address/identifier of parent */
  int p_currentline; /* parent callsite information */
  lmprof_FunctionInfo info;

  /* Additional statistics for each profiling configuration. */
  union {
    struct {
      size_t count; /* number of function invocations */
      lmprof_EventUnit node; /* time spent 'in' the function */
      lmprof_EventUnit path; /* time spent within all functions called by this record */

      int line_freq_size;
      size_t *line_freq;
    } graph;
  };
} lmprof_Record;

/* Zero out all data associated with a function record measurement */
static LUA_INLINE void unit_clear(lmprof_EventUnit *unit) {
  unit->time = 0;
  unit->allocated = unit->deallocated = 0;
}

/* Add "source" to "dest" */
static LUA_INLINE void unit_add_to(lmprof_EventUnit *dest, const lmprof_EventUnit *source) {
  dest->time += source->time;
  dest->allocated += source->allocated;
  dest->deallocated += source->deallocated;
}

/* Subtract the values of one unit (A) from another (B) and store the result in 'dest' */
static LUA_INLINE void unit_sub(lmprof_EventUnit *dest, const lmprof_EventUnit *a, const lmprof_EventUnit *b) {
  dest->time = a->time - b->time;
  dest->allocated = a->allocated - b->allocated;
  dest->deallocated = a->deallocated - b->deallocated;
}

/*
** Compute the total number of allocated bytes; returning zero if more bytes
** have been deallocated than allocated.
*/
static LUA_INLINE lu_sizediff unit_allocated(const lmprof_EventUnit *u) {
  /*
  * Data allocated before the profiler began was GC'd while profiling, causing
  * deallocated to be greater than allocated.
  *
  * Assume (or hope?) this profiler will never track enough runtime allocations
  * to cause overflow issues. 32-bit compiles will keep previous functionality
  * until 32-bit builds are focused on more.
  */
#if LUA_32BITS
  return (u->deallocated > u->allocated) ? 0 : (u->allocated - u->deallocated);
#else
  if (u->deallocated > u->allocated)
    return -l_cast(lu_sizediff, u->deallocated - u->allocated);
  return l_cast(lu_sizediff, u->allocated - u->deallocated);
#endif
}

/*
** {==================================================================
** FunctionInfo
** ===================================================================
*/

/* Reserved Function Identifiers */
#define LMPROF_RECORD_ID_ROOT 0
#define LMPROF_RECORD_ID_MAIN 1 /* 'm' */
#define LMPROF_RECORD_ID_GC 2
#define LMPROF_RECORD_ID_UNKNOWN 3 /* non collectible closures */
#define LMPROF_RESERVED_MAX (LMPROF_RECORD_ID_UNKNOWN + 1)

#define LMPROF_RECORD_NAME_MAIN "main chunk"
#define LMPROF_RECORD_NAME_ROOT "(root)"
#define LMPROF_RECORD_NAME_GC "(profiler gc)"
#define LMPROF_RECORD_NAME_UNKNOWN "?"

/* Initialize meta-definitions for lmprof_Record's */
LUA_API void lmprof_record_initialize(lua_State *L);

/* Creates and pushes on the stack a new lmprof_Record userdata */
LUA_API lmprof_Record *lmprof_record_new(lua_State *L);

/* Clear and deallocate all memory owned by the record. */
LUA_API void lmprof_record_clear(lmprof_Alloc *alloc, lmprof_Record *record);

/*
** Create an identifier for the given activation record 'ar'. If the Lua garbage
** collector is active during profiling, the identifier must be independent of
** the closure/function reference address.
**
** If the activation record references a CFunction (see lua_tocfunction) then it
** is possible to use that pointer as an identifier. In addition, any CFunction
** is returned in the 'result' pointer, allowing the profiler to preempt 'stop'
** and other API functions.
**
** This function ensures the activation record 'ar' is populated with all
** DEBUG_IMMUTABLE fields.
*/
LUA_API lu_addr lmprof_record_id(lua_State *L, lua_Debug *ar, int gc_disabled, lua_CFunction *result);

/*
** Push the function referenced by the debug record onto the stack; on NULL push
** the provided function identifier.
*/
LUA_API void lmprof_record_function(lua_State *L, lua_Debug *ar, lu_addr fid);

/* Clear all profile statistics associated with the given function record. */
LUA_API void lmprof_record_clear_graph_statistics(lmprof_Record *record);

/* Given an active function for a Lua state, populate its function info record */
LUA_API void lmprof_record_populate(lua_State *L, lmprof_Alloc *alloc, lua_Debug *ar, lmprof_FunctionInfo *info);

/*
** Update the name to a profiler record if its details are unknown or partially
** unknown.
*/
LUA_API void lmprof_record_update(lua_State *L, lmprof_Alloc *alloc, lua_Debug *ar, lu_addr f_id, lmprof_FunctionInfo *info);

/*
** Ensure the provided string is a well-formed (i.e., can be properly parsed by
** the Lua when loaded) string. The most common use case would be the multi-state
** strings beginning with --
*/
LUA_API char *lmprof_record_sanitize(char *source, size_t len);

/*
** {==================================================================
** Lua Debugging Utilities
** ===================================================================
*/

#define CO_STATUS_RUN 0 /* running */
#define CO_STATUS_YIELD 1 /* suspended */
#define CO_STATUS_NORM 2 /* 'normal' (it resumed another coroutine) */
#define CO_STATUS_DEAD 3

/* Ensure the thread is in a valid/operable state. */
#define luaL_verify_state(L) (lua_status((L)) <= LUA_YIELD)
#define luaL_verify_thread(CO) (lua_auxstatus((CO)) <= CO_STATUS_NORM)

/* @HACK: Quick hack for lmprof_get_name. */
LUA_API void lua_pushfuncname(lua_State *L, lua_Debug *ar);

/*
** @HACK: Find the 'stack depth' of the current Lua state: the largest value
**  of 'lua_getstack' that returns an actual activation record.
*/
LUA_API int lua_lastlevel(lua_State *L);

/* @HACK coroutine.status */
LUA_API int lua_auxstatus(lua_State *co);

/* }================================================================== */

#endif
