/*
** $Id: lmprof_state.h $
** Library profiling definitions and state API.
**
** See Copyright Notice in lmprof_lib.h
*/
#ifndef lmprof_state_h
#define lmprof_state_h

#include <stdint.h>

#include "lmprof_conf.h"

#define LMPROF_LMPROF_STATE_METATABLE "lmprof_profiler_metatable"

/* Profiling Modes. */
#define LMPROF_MODE_NONE           0x0
#define LMPROF_MODE_TIME           0x1 /* TIME: The time between successive start/stop operations. */
#define LMPROF_MODE_INSTRUMENT     0x2 /* GRAPH: Track the time between successive lua_Hook calls, creating a call-graph */
#define LMPROF_MODE_SAMPLE         0x4 /* GRAPH/TRACE_EVENT: Generate LUA_MASKCOUNT TraceEvents */
#define LMPROF_MODE_MEMORY         0x8 /* GRAPH/TRACE_EVENT: Track the memory usage between successive lua_Hook calls. */
#define LMPROF_MODE_TRACE         0x10 /* TRACE_EVENT: Generate events compatible with DevTools */
#define LMPROF_MODE_LINE          0x20 /* TRACE_EVENT: Generate LUA_MASKLINE TraceEvents */
#define LMPROF_MODE_SINGLE_THREAD 0x40 /* ALL: Single thread profiling */
#define LMPROF_MODE_EXT_CALLBACK  0x80 /* Uses 'trace' callback interface, profiler created from C API. */

/* Uses the 'trace' interface */
#define LMPROF_CALLBACK_MASK (LMPROF_MODE_TRACE | LMPROF_MODE_EXT_CALLBACK)

/* Valid modes exposed to Lua scripts. */
#define LMPROF_LUA_MODE_MASK (LMPROF_MODE_TIME | LMPROF_MODE_INSTRUMENT | LMPROF_MODE_SAMPLE | LMPROF_MODE_MEMORY | LMPROF_MODE_TRACE)

/* Internal profiler flags */
#define LMPROF_STATE_NONE            0x0
#define LMPROF_STATE_PERSISTENT      0x1 /* A userdata instance */
#define LMPROF_STATE_SETTING_UP      0x2 /* Profiler is being initialized (lmprof_initialize_profiler) */
#define LMPROF_STATE_RUNNING         0x4 /* Profiler is collecting statistics. */
#define LMPROF_STATE_ERROR           0x8 /* Profiler in an error'd state */
#define LMPROF_STATE_IGNORE_ALLOC   0x10 /* Ignore allocator internals (often used in internal profiling functions) */
#define LMPROF_STATE_IGNORE_CALL    0x20 /* Ignore luaD_hook of next function (often used when starting/stopping) */
#define LMPROF_STATE_GC_WAS_RUNNING 0x40 /* Garbage collector state prior to profiling */
#define LMPROF_STATE_PAUSED         0x80 /* See: lmprof_pause_execution */

/* Profiler configuration */
#define LMPROF_OPT_NONE               0x0
#define LMPROF_OPT_GC_DISABLE         0x1 /* Disable the Lua garbage collector while in a profiled state. Technically, scripts can re-enable the GC while collecting. */
#define LMPROF_OPT_CLOCK_INIT         0x2 /* (Re-) initialize clock sampler on profiler instantiation. */
#define LMPROF_OPT_CLOCK_MICRO        0x4 /* Output microseconds. Chrome: Output nanosecond timings as if they were microseconds. */
#define LMPROF_OPT_INSTRUCTION_COUNT  0x8 /* Generate LUA_HOOKCOUNT events after the interpreter executes every count instructions */
#define LMPROF_OPT_LOAD_STACK        0x10 /* Use 'lua_getstack' to populate a lmprof_Stack (with its traceback) on instantiation */
#define LMPROF_OPT_STACK_MISMATCH    0x20 /* Allow start/stop to be called at different stack levels. */
#define LMPROF_OPT_COMPRESS_GRAPH    0x40 /* p_id is defined by the parents f_id; otherwise the parents record id */
#define LMPROF_OPT_GC_COUNT_INIT     0x80 /* Include garbage collector statistics (LUA_GCCOUNT[B]) on profiler init */

#define LMPROF_OPT_REPORT_VERBOSE       0x1000 /* Include additional debug information */
#define LMPROF_OPT_REPORT_STRING        0x2000 /* Output a formatted Lua string instead of an encoded table. */
#define LMPROF_OPT_HASH_SIZE           0x40000 /* Reserved */
#define LMPROF_OPT_LINE_FREQUENCY      0x80000 /* Reserved */

#define LMPROF_OPT_TRACE_COUNTERS_FREQ   0x200000 /* Reversed: Reduce the number of UpdateCounters events */
#define LMPROF_OPT_TRACE_IGNORE_YIELD    0x400000 /* Ignore all coroutine.yield() records */
#define LMPROF_OPT_TRACE_PROCESS         0x800000 /* Reserved */
#define LMPROF_OPT_TRACE_URL            0x1000000 /* Reserved: TracingStartedInBrowser: URL */
#define LMPROF_OPT_TRACE_NAME           0x2000000 /* Reserved: TracingStartedInBrowser: Name */
#define LMPROF_OPT_TRACE_DRAW_FRAME     0x4000000 /* Include DrawFrame/Frame counters */
#define LMPROF_OPT_TRACE_LAYOUT_SPLIT   0x8000000 /* Output unique tids for each thread in the chromium output; Otherwise, all events use the main thread */
                                                  /* and stack elements are artificially pushed/popped when the co-routine resume/yields */
#define LMPROF_OPT_TRACE_ABOUT_TRACING 0x10000000 /* chrome://tracing/ */
#define LMPROF_OPT_TRACE_PAGELIMIT     0x20000000 /* Maximum size (in bytes) of trace event list (sum of all pages) */
#define LMPROF_OPT_TRACE_COMPRESS      0x40000000 /* Ignore sub-microsecond functions from output. */
#define LMPROF_OPT_TRACE_THRESHOLD     0x80000000 /* Reversed: Trace event compression threshold */

#if LUA_32BITS
  #define LMPROF_OPT_DEFAULT (LMPROF_OPT_CLOCK_INIT | LMPROF_OPT_CLOCK_MICRO | LMPROF_OPT_LOAD_STACK | LMPROF_OPT_COMPRESS_GRAPH)
#else
  #define LMPROF_OPT_DEFAULT (LMPROF_OPT_CLOCK_INIT | LMPROF_OPT_LOAD_STACK | LMPROF_OPT_COMPRESS_GRAPH)
#endif

/*
** @TODO: Throw an error when: (including lua.h)
**    (1) (LUA_VERSION_NUM >= 504) && !defined(LUAI_IS32INT) ; or
**    (2) (LUA_VERSION_NUM < 503) && (LUAI_BITSINT < 32)
**
** @TODO: Allow custom LMPROF_TIME() hooks. The framework is already in place.
*/
#if defined(LMPROF_RDTSC) || defined(LMPROF_RDTSCP)
  #define LUA_TIME lmprof_clock_rdtsc

  #define LMPROF_TIME(S) (S)->time()
  #define LMPROF_TIME_ID(O) (((O) & LMPROF_OPT_CLOCK_MICRO) ? "Krdtsc" : "rdtsc")
  #define LMPROF_TIME_ADJ(T, F) (((F) & LMPROF_OPT_CLOCK_MICRO) ? ((T) / 1000) : (T))
#else
  /* Format the time 'T' according to the profile configuration flags 'F' */
  #define LUA_TIME lmprof_clock_sample

  #define LMPROF_TIME(S) (S)->time()
  #if LUA_32BITS
    #define LMPROF_TIME_ID(O) "micro"
    #define LMPROF_TIME_ADJ(T, F) LU_TIME_MICRO(T)
  #else
    #define LMPROF_TIME_ID(O) (((O) & LMPROF_OPT_CLOCK_MICRO) ? "micro" : "nano")
    #define LMPROF_TIME_ADJ(T, F) (((F) & LMPROF_OPT_CLOCK_MICRO) ? LU_TIME_MICRO(T) : LU_TIME_NANO(T))
  #endif
#endif

/* Profiler definition. */
typedef struct lmprof_State lmprof_State;
typedef struct lmprof_StackInst lmprof_StateInst;

/* Timer hook */
typedef lu_time (*lmprof_Time)(void);

/* Resource disposal and error handling */
typedef void (*lmprof_Error)(lua_State *, lmprof_State *);

/*
** BEGIN_ROUTINE/END_ROUTINE event handler (coroutines).
**
** Returning LUA_OK on success; an error code otherwise.
*/
typedef int (*lmprof_TraceRoutine)(lua_State *, lmprof_State *, lmprof_EventProcess, int);

/*
** ENTER_SCOPE/EXIT_SCOPE event handler (functions).
**
** Returning LUA_OK on success; an error code otherwise.
*/
typedef int (*lmprof_TraceScope)(lua_State *, lmprof_State *, lmprof_StateInst *, int);

/*
** SAMPLE_EVENT/LINE_SCOPE event handling (lines/instructions).
**
** Returning LUA_OK on success and an error code otherwise.
*/
typedef int (*lmprof_TraceSample)(lua_State *, lmprof_State *, lmprof_StateInst *, int);

/* Cleanup additional arguments */
typedef void (*lmprof_TraceFree)(lua_State *, void *);

/* Structure that keeps allocation info about the running State */
struct lmprof_State {
  uint32_t mode; /* profiling flags */
  uint32_t conf; /* configuration flags */
  uint32_t state; /* state flags */
  lmprof_Time time;
  lmprof_Error on_error; /* error/invalid state handler */

  /* lua_sethook configuration. */
  struct {
    lmprof_Alloc alloc; /* structure that keeps previous Lua allocation function and ud */
    lua_CFunction yield; /* Cached reference to coroutine.yield() */

    lua_Hook l_hook; /* overridden and active hook function */
    uint32_t flags; /* LUA_MASKCALL, LUA_MASKRET, LUA_MASKLINE, LUA_MASKCOUNT: Configuration */
    int line_count; /* LUA_MASKCOUNT: Value */
  } hook;

  /* Active thread/coroutine */
  struct {
    lua_State *main; /* Thread that initialized profiling */
    lmprof_EventProcess mainproc; /* main thread & process identifiers */
    lmprof_EventMeasurement r; /* running statistics/state data */

    lua_State *state; /* Executing thread. */
    struct lmprof_Stack *call_stack; /* stack containing memory use when entering a function */
    size_t stack_count; /* Number of allocated stacks */
  } thread;

  /*
  ** Interface for fetching activation record instances according to the current
  ** profiler interface:
  **
  **  'graph': profiling uses a hash table that relates 'parent' and 'child'
  **    function identifiers. Assuming no hash collisions, the generated output
  **    can converted into a graph to relate profiling measurements.
  **
  **  'trace': profiler converts lua_Hook callbacks into a sequence of 'events'
  **    that can be formatted into something compatible with
  **    ChromeDevTools/devtools-frontend.
  */
  struct {
    /*
    ** Fields cached from the registry on instantiation. Ensuring the profiler
    ** state (or configuration) cannot be manipulated during execution unless
    ** explicitly.
    */
    int mask_count; /* LUA_MASKCOUNT value */
    size_t instr_count; /* LUA_HOOKCOUNT: Number of profiler instructions */
    size_t hash_size; /* Size of graph hashtable */
    lu_time calibration; /* Function call overhead to compensate for */

    /* TraceEvent */
    const char *url; /* TraceEvent URL */
    const char *name; /* TraceEvent Name */
    lua_Integer pageLimit; /* Maximum TraceEvent list size (in bytes) */
    lua_Integer counterFrequency; /* Reduce 'UpdateCounters' count */
    lu_time event_threshold; /* Threshold */

    /* Structures */
    lu_addr record_count; /* Number of lmprof_Record's created (used to assign unique identifiers) */
    struct lmprof_Hash *hash; /* hash table containing information of each function call */
    union {
      /* struct { } graph; */
      struct {
        void *arg; /* Shared callback arguments */
        lmprof_TraceRoutine routine; /* Change in coroutine; see LMPROF_OPT_TRACE_LAYOUT_SPLIT */
        lmprof_TraceScope scope; /* Profiler events: LUA_HOOKCALL/LUA_HOOKRET/LUA_HOOKTAILCALL */
        lmprof_TraceSample sample; /* Profiler events: LUA_MASKCOUNT | LUA_MASKLINE */
        lmprof_TraceFree free; /* Callback finalization */
      } trace;
    };
  } i; /* 'interface' */
};

/* 'startup' error codes */
#define LMPROF_STARTUP_OK              0x0
#define LMPROF_STARTUP_ERROR           0x1 /* LMPROF_STATE_ERROR is set */
#define LMPROF_STARTUP_ERROR_RUNNING   0x2 /* LMPROF_STATE_RUNNING is set*/
#define LMPROF_STARTUP_ERROR_SINGLETON 0x4 /* Another profiler is registered */

/* Return the active profiler registered in the global registry table. */
LUA_API lmprof_State *lmprof_singleton(lua_State *L);

/* Creates and pushes on the stack a new lmprof_State userdata */
LUA_API lmprof_State *lmprof_new(lua_State *L, uint32_t mode, lmprof_Error error);

/*
** Shared error handling callback. Ensuring the profiler state and all allocated
** registry data is finalized.
**
** This function also ensures that any persistent (LMPROF_STATE_PERSISTENT)
** profiler object can be used to profile again.
*/
LUA_API void lmprof_default_error(lua_State *L, lmprof_State *st);

/*
** lmprof_initialize_profiler wrapper: setup default interface/hook functions
** depending on configuration. Returning true on success; false otherwise.
**
** @NOTE: 'idx' is the stack index of the profiler state; this value is used
**  to register the profiler in the registry.
*/
LUA_API int lmprof_initialize_default(lua_State *L, lmprof_State *st, int idx);

/*
** lmprof_initialize_profiler wrapper: setup default hook functions depending on
** configuration. Returning true on success; false otherwise.
**
** @NOTE: Allows custom interface callback functions when compared to
**   lmprof_initialize_default
*/
LUA_API int lmprof_initialize_only_hooks(lua_State *L, lmprof_State *st, int idx);

/*
** Initialize the profiler state: setup debug and allocator hooks, store a
** reference to the profiler in the registry table, and apply any additional
** profiler options to Lua (e.g., LMPROF_OPT_GC_DISABLE).
**
** Returning: 'LMPROF_STARTUP_OK' on success and an error code otherwise.
*/
LUA_API int lmprof_initialize_profiler(lua_State *L, lmprof_State *st, int idx, lua_Hook fhook, lua_Alloc ahook);

/*
** Finalize the profiler state: remove debug and allocator hooks, however, leave
** the profiler reference in the registry table: ensuring the data is kept alive
** until cleanup/reporting has finished.
*
** 'pop_remaining': pop all remaining stack instances from all registered
**    profiler stacks. Note, this is not supported for the TraceEvent API as the
**    'events' are useless, e.g., 'main chunk', '(root)', etc.
*/
LUA_API void lmprof_finalize_profiler(lua_State *L, lmprof_State *st, int pop_remaining);

/*
** Shutdown the profiler state: remove it from the registry and potentially
** deallocate any additional profiler data.
*/
LUA_API void lmprof_shutdown_profiler(lua_State *L, lmprof_State *st);

/* External API */

/*
** The resume/pause API is designed for cases of multiple Lua states sharing the
** same routine/scope/sample interface.trace API.
*/

/*
** Generate EXIT_SCOPE events for each remaining function on the stack.
** Returning true if the profiler state has been paused; false otherwise. While
** the profiler is in a 'paused' state LMPROF_STATE_PAUSED will be set.
*/
LUA_API int lmprof_pause_execution(lua_State *L, lmprof_State *st);

/*
** Generate ENTER_SCOPE events for each function on the threads current
** call stack (thread.call_stack). Returning true on success; false otherwise.
*/
LUA_API int lmprof_resume_execution(lua_State *L, lmprof_State *st);

/*
** Return a user-supplied name associated with the given lua_State; returning
** an optional name when the thread has no association.
**
** @HACK: Moved from lmprof.h to allow external C APIs to access this data.
*/
LUA_API const char *lmprof_thread_name(lua_State *L, lua_Integer thread_id, const char *opt);

#endif
