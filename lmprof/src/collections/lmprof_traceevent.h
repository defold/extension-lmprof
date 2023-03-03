/*
** $Id: lmprof_traceevent.h $
** TraceEvent API.
** See Copyright Notice in lmprof_lib.h
*/
#ifndef lmprof_traceevent_h
#define lmprof_traceevent_h

#include "../lmprof_conf.h"

#include "lmprof_record.h"

/* Error Codes */
#define TRACE_EVENT_OK 0 /* Can never change */
#define TRACE_EVENT_ERRARG 1 /* Invalid arguments */
#define TRACE_EVENT_ERRPAGEFULL 2 /* Page buffers is full */
#define TRACE_EVENT_ERRMEM 3 /* Could not allocate compression stack */
#define TRACE_EVENT_ERRSTACKFULL 4 /* Profiler in invalid state: max stack sized reached */
#define TRACE_EVENT_ERRSTACKEMPTY 5 /* Profiler in invalid state: push without a pop */
#define TRACE_EVENT_ERRPROCESS 6 /* Process mismatch */
#define TRACE_EVENT_ERRTHREAD 7 /* Thread mismatch */
#define TRACE_EVENT_ERRFUNCINFO 8 /* Function info mismatch */

LUA_API const char *traceevent_strerror(int traceevent_errno);

/* Thread Handling */
#define LMPROF_PROCESS_MAIN 1
#define LMPROF_THREAD_BROWSER 1
#define LMPROF_THREAD_SAMPLE_TIMELINE 2 /* Unique timeline for displaying 'sampling' events in a flamegraph */
#define LMPROF_THREAD_OFFSET(X) ((lua_Integer)(X) + LMPROF_THREAD_SAMPLE_TIMELINE + 1)

/* Profile Stack information. */
typedef struct TraceEventStackInstance {
  lmprof_Record *record; /* active function */
  struct TraceEvent *begin_event; /* associated BEGIN_* operation if one exists */
  lmprof_EventMeasurement call;
} TraceEventStackInstance;

/*
** {==================================================================
** Event Buffering
** ===================================================================
*/

/* TraceEventType categorization */
#define op_frame(op) ((op) == BEGIN_FRAME || (op) == END_FRAME)
#define op_routine(op) ((op) == BEGIN_ROUTINE || (op) == END_ROUTINE)
#define op_event(op) ((op) == ENTER_SCOPE || (op) == EXIT_SCOPE || (op) == IGNORE_SCOPE)
#define op_adjust(op) (op_event(op) || op_frame(op) || op_routine(op) || ((op) == LINE_SCOPE || (op) == SAMPLE_EVENT))

typedef enum TraceEventType {
  BEGIN_FRAME, /* timeline.frame */
  END_FRAME, /* timeline.frame */
  BEGIN_ROUTINE, /* [C] coroutine.resume */
  END_ROUTINE, /* [C] coroutine.yield */
  ENTER_SCOPE, /* Begin lua*_call*/
  EXIT_SCOPE, /* End lua*_call*/
  LINE_SCOPE, /* Execution of a new line of Lua code */
  SAMPLE_EVENT, /* Execution of a preset number of Lua instructions */
  PROCESS, /* Process create */
  THREAD, /* Thread create */
  IGNORE_SCOPE /* Ignored event */
} TraceEventType;

typedef struct TraceEvent {
  TraceEventType op;
  lmprof_EventMeasurement call;
  union Data {
    struct {
      const lmprof_FunctionInfo *info;
      struct TraceEvent *sibling;
      struct TraceEvent *lines;
      unsigned char flags;
    } event;
    /* A linked-list of all line events related to the function in reverse order */
    struct {
      const lmprof_FunctionInfo *info;
      struct TraceEvent *previous;
      struct TraceEvent *next;
      int line; /* line number */
    } line;
    struct {
      struct TraceEvent *next; /* next sample event used to compute durations */
    } sample;
    struct {
      size_t frame;
    } frame;
    struct {
      char *name;
      size_t nameLen;
    } process;
    /* @TODO: Base64 encoded screenshot */
  } data;
} TraceEvent;

typedef struct TraceEventPage {
  size_t count; /* available number of profiling events. */
  struct TraceEventPage *next; /* next linked page. */
  struct TraceEvent event_array[1]; /* array of trace events*/
} TraceEventPage;

/* Linked list of fixed-sizes pages. */
typedef struct TraceEventTimeline {
  lmprof_Alloc *page_allocator;
  size_t pageCount; /* Number of active pages */
  size_t pageLimit; /* Maximum number of allocated pages */
  size_t frameCount; /* Number of beginframe calls */
  lu_time baseTime; /* Base time subtracted from all TraceEvent instances */
  struct TraceEventPage *head;
  struct TraceEventPage *curr;
} TraceEventTimeline;

/* Iterator interface */
typedef void (*TraceEventIterator)(TraceEventTimeline *, TraceEvent *, const void *);

/* Create a new ProfilerEvent buffer */
LUA_API TraceEventPage *traceevent_new(lmprof_Alloc *alloc);

/* Clear all data within a page buffer. */
LUA_API void traceevent_free(lmprof_Alloc *alloc, TraceEventPage *buffer);

/*
** Create a new ProfilerEvent list that stores ProfilingEvent's in fixed sized
** allocated blocks.
*/
LUA_API TraceEventTimeline *timeline_new(lmprof_Alloc *alloc, size_t pageLimit);

/* Free all memory associated with the chrome list */
LUA_API void timeline_free(TraceEventTimeline *list);

/* Return the default TraceEventPage size  */
LUA_API size_t timeline_page_size(void);

/* Number of TraceEvent instances that can fit within a single TraceEventPage */
LUA_API size_t timeline_event_array_size(void);

/* Return buffer usage: a value between 0.0 and 1.0 */
LUA_API double timeline_usage(const TraceEventTimeline *list);

/* Return true if the trace event list can buffer "N" additional trace events */
LUA_API int timeline_canbuffer(const TraceEventTimeline *list, size_t n);

/* Traverse through each profiling event, invoking 'cb' for each TraceEvent in the list. */
LUA_API void timeline_foreach(TraceEventTimeline *list, TraceEventIterator cb, void *args);

/*
** Subtract 'baseTime' from all events.
**
** @NOTE:Ideally, this functionality would be included in the event handlers,
** but has been separated for experimentation.
*/
LUA_API void timeline_adjust(TraceEventTimeline *list);

/* Options for event compression. */
typedef struct TraceEventCompressOpts {
  lmprof_EventProcess id; /* Only compress events from a specific process/thread, 0 for all */
  lu_time threshold; /* Event timing threshold, 0 for no threshold */
} TraceEventCompressOpts;

/*
** Iterate through the current list of buffered profile events and pair together
** push/pop events, the time between events (i.e., total execution of the
** function record) is below some threshold, swap the states to those events
** as ignored.
**
** In experimentation that can reduce the size of the resultant JSON by multiple
** factors (> 5 with 2html).
**
** @RETURN An error code; CHROME_OK on success.
*/
LUA_API int timeline_compress(TraceEventTimeline *list, TraceEventCompressOpts opts);

/* }================================================================== */

/*
** {==================================================================
** Event Collection
** ===================================================================
*/

/*
** Append a PROCESS event, i.e., a notification that a new Lua state, or
** collection of Lua states under a shared identifier, is being profiled.
*/
LUA_API int traceevent_metadata_process(TraceEventTimeline *list, lua_Integer process, const char *name);

/* Append a THREAD event. i.e., a notification that a new Lua state is being profiled. */
LUA_API int traceevent_metadata_thread(TraceEventTimeline *list, lmprof_EventProcess process, const char *name);

/* Append push a BEGIN_FRAME/END_FRAME trace event */
LUA_API int traceevent_beginframe(TraceEventTimeline *list, lmprof_EventMeasurement unit);
LUA_API int traceevent_endframe(TraceEventTimeline *list, lmprof_EventMeasurement unit);

/* Append a BEGIN_ROUTINE/END_ROUTINE TraceEvent  */
LUA_API int traceevent_beginroutine(TraceEventTimeline *list, lmprof_EventMeasurement unit);
LUA_API int traceevent_endroutine(TraceEventTimeline *list, lmprof_EventMeasurement unit);

/* Append a ENTER_SCOPE/EXIT_SCOPE TraceEvent  */
LUA_API int traceevent_enterscope(TraceEventTimeline *list, TraceEventStackInstance *inst);
LUA_API int traceevent_exitscope(TraceEventTimeline *list, TraceEventStackInstance *inst);

/* Append a SAMPLE_EVENT/LINE_SCOPE TraceEvent */
LUA_API int traceevent_sample(TraceEventTimeline *list, TraceEventStackInstance *inst, lmprof_EventMeasurement unit, int currentline);

/* }================================================================== */

#endif
