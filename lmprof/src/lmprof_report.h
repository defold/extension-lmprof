/*
** $Id: lmprof_report.h $
** Profiling IO
** See Copyright Notice in lmprof_lib.h
*/
#ifndef lmprof_report_h
#define lmprof_report_h
#define LMPROF_FILE_API

#include <stdio.h>

#include "lmprof_conf.h"

#include "collections/lmprof_record.h"
#include "collections/lmprof_traceevent.h"

#include "lmprof_state.h"

#define LMPROF_REPORT_FAILURE -1
#define LMPROF_REPORT_DISABLED_IO -2
#define LMPROF_REPORT_UNKNOWN_TYPE -3

/* An emulation of LUA_FILEHANDLE */
#define LMPROF_IO_METATABLE "lmprof_io_metatable"

/*
** TIME_FORMAT: A numeric value, representing the time between start/stop
**  operations.
*/

/*
** GRAPH_FORMAT: A table with two fields: 'header' and 'records'.
**
** HEADER:
**  [STRING]:
**    clockid: Identifier of the unit of time: 'rdtsc', 'Krdtsc', 'micro', or
**      'nano'. This value may be different than 'lmprof_get_timeunit'.
**
**  [BOOL]:
**    instrument: True if the *time* between successive function calls is
**      profiled.
**    memory: True if the amount of memory allocated/deallocated in tracked
**      between successive function calls.
**    callback: True if the trace/callback API was used for profiling.
**      Otherwise, the 'graph' format is used.
**    single_thread: True if only a single lua_State/coroutine has been profiled.
**    overhead: True if function timings were compensated for static Lua overheads.
**    mismatch:  SEE lmprof_set_option.
**
**  [INTEGER]:
**    profile_overhead - an estimation of the profiler overhead (time).
**    calibration - an estimation of the Lua function call overhead.
**    sampler_count - Configured sampling instruction count.
**    instr_count - Total number of executed Lua instructions (correct to a
**      value within sampler_count).
**
** RECORDS: An array of profile records.
**  [STRING]:
**    id - unique global identifier.
**    func - function identifier.
**    parent - parent function identifier.
**    parent_line - Line number of the function callsite in its parent if 'line'
**      mode is enabled; zero otherwise.
**    name - Name of the given function. As functions in Lua are first-class,
**      they do not have a fixed name: some functions can be the value of
**      multiple global variables, while others stored only in a table field.
**    what - "Lua" if the function is a Lua function, "C" if it is a C function,
**      "main" if it is the main part of a chunk.
**    source - formatted record name of the function record, including line
**      number, function type, etc.
**
**  [BOOL]:
**    ignored - TODOCUMENT
**    [>= LUA 502]
**    isvararg - true if the function is a vararg function (always true for C functions)
**    istailcall - true if this function invocation was called by a tail call
*
**  [INTEGER]:
**    count - number of function calls.
**    time - total time spent within the function.
**    total_time - total time spent within the function and all of its children.
**    allocated - total amount of memory allocated within the function.
**    deallocated - total amount of memory freed while in the function, this
**      may incorporate data allocated within other functions.
**    total_allocated - total amount of memory allocated within the function and
**      all of its children.
**    total_deallocated - total amount of memory freed within the function and
**      all of its children.
**
**    linedefined - the line number where the definition of the function starts.
**    lastlinedefined - the line number where the definition of the function ends.
**    nups - the number of upvalues of the function.
**
**    [>= LUA 502]
**    nparams - the number of parameters of the function (always 0 for C functions).
**
**    [>= LUA 504]
**    ftransfer - the index on the stack of the first value being "transferred",
**      that is, parameters in a call or return values in a return.
**    ntransfer - the number of values being transferred
**
**  [TABLE]:
**    lines - An array of frequencies that correspond to the execution of each
**      of each new line of code by the Lua interpreter. The array is of size
**      'lastlinedefined - linedefined + 1'; Lines are shifted 1-adjusted by
**      subtracting 'linedefined'.
*/

/*
** TRACE_EVENT_FORMAT: A table with two fields: 'header' and 'records'
**
** HEADER: Everything in GRAPH_FORMAT plus
**  [BOOL]:
**    compress - SEE lmprof_get_option.
**
**  [INTEGER]:
**    eventsize - Size of the TraceEvent struct. (Debug)
**    eventpages - Number of TraceEvents per-page. (Debug)
**    pagesize - Size (in bytes) of each trace event 'page'.
**    pagelimit - Maximum size (in bytes) of the trace event list; zero for infinite.
**    usedpages - Number of allocated pages.
**    totalpages - Total available page count (a product of pagelimit and pagesize).
**    counterFrequency - Frequency of 'UpdateCounters' event generation to avoid
**      redundant/useless UpdateCounters events.
**    event_threshold - Duration threshold.
**
**  [NUMBER]:
**    pageusage - percent usage of the trace event list.
**
** RECORDS: The layout will not be detailed here, however, it is compatible with
**  the Chrome DevTools (timline) spec:
**    https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/preview
**  and when the table is json encoded, can be loaded into
**    https://github.com/ChromeDevTools/devtools-frontend.
**  Note, that the generated output uses some deprecated features and will
**  require change in the future and there are still many @TODO's remaining.
*/

typedef enum lmprof_ReportType {
  lTable, /* Generate an array of profiling records. */
  lFile, /* Write profiling records to file (format defined by profiling mode); requires LMPROF_FILE_API */
  lBuffer, /* Write profiling records to a luaL_Buffer; returning the string. */
} lmprof_ReportType;

/* Common header for dumping reports */
typedef struct lmprof_Report {
  lmprof_State *st;
  lmprof_ReportType type;
  union {
    /* Generate an array of profiling records. */
    struct {
      int table_index; /* Absolute index of the table on the Lua stack. */
#if LUA_VERSION_NUM >= 503
      lua_Integer array_count; /* Array index counter */
#else
      int array_count;
#endif
    } t;
    /*
    ** Write the profiling records/results to a file. The format and layout of
    ** the records depend upon the profiling mode and configuration. See
    ** documentation above.
    **
    ** @NOTE See caveats with the LMPROF_FILE_API option.
    ** @NOTE Ideally a luaL_stream would be used. However that feature was not
    **  introduced Lua 5.2.
    */
    struct {
      FILE *file;
      int delim; /* Requires delimitation on next write */
      const char *indent; /* Current indentation string (often a function of some nested depth) */
    } f;
    struct {
      luaL_Buffer buff;
      int delim;
      const char *indent;
    } b;
  };
} lmprof_Report;

/* Initialize LMPROF_IO_METATABLE */
LUA_API void lmprof_report_initialize(lua_State *L);

/*
** Generate a 'report' for the given profiler state, returning the type of the
** 'reported' object. See the format specification above.
*/
LUA_API int lmprof_report(lua_State *L, lmprof_State *st, lmprof_ReportType type, const char *file);

#endif
