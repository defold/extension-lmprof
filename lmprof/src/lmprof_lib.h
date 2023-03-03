/*
** $Id: lmprof_lib.h $
** See Copyright Notice at the end of this file
*/
#ifndef lmprof_lib_h
#define lmprof_lib_h

extern "C" {
  #include <dmsdk/lua/lua.h>
  // #include <dmsdk/lua/lauxlib.h>
  // #include <dmsdk/lua/lualib.h>
}

#define LMPROF_FILE_API
#define LMPROF_NAME "lmprof"
#define LMPROF_VERSION "lmprof 0.1.14"
#define LMPROF_COPYRIGHT "Copyright (C) 2021, Gottfried Leibniz"
#define LMPROF_DESCRIPTION "A Lua Profiler"
#if !defined(LUAMOD_API)
  #define LUAMOD_API LUALIB_API
#endif

#if defined(__cplusplus)
extern "C" {
#endif

#define LUA_LMPROF_LIBNAME "lmprof"
LUAMOD_API int luaopen_lmprof(lua_State *L);

/*
** {==================================================================
** Module
** ===================================================================
*/

/*
** create(...): Create a profiler instance.
**
** Each userdata instance maintains its own local configuration. In addition,
** for Lua 5.4 this userdata can be configured as to-be-closed to ensure that a
** profiler can never exit a scope without it automatically(*) stopping itself.
**
** (*) to-be-closed conditions apply.
*/
LUALIB_API int lmprof_create(lua_State *L);

/*
** start: start a profiler instance. Pushing the profiler userdata onto the
**  stack on success, nil otherwise.
**
** This function registers the profiler singleton in the registry and may
** (depending on profiling mode) override (wrap) the active Lua allocator. While
** a profiler is active debug.sethook and lua_setallocf should not be used. Any
** calls to lmprof.start will throw an error.
**
** (*) While a profiler is active debug.sethook and lua_setallocf should not be
**  used. Any calls to lmprof.start will throw an error.
**
** Modes:  The profiling configuration arguments are passed in as strings:
**  "time" - [TIME] The time between successive start/stop operations.
**
**  "instrument" - [GRAPH] Measurements between successive lua_Hook calls and
**      track the relationships between functions.
**
**  "trace" - [TRACE] Generate events compatible with DevTools
**
**  "single_thread" - Single thread profiling. Ignore all threads except the one
**    that invoked 'start'.
**
**  "memory" - Enable memory profiling by hooking the Lua allocator.
**    GRAPH: Track the memory usage between successive lua_Hook calls.
**    TRACE: Generate 'UpdateCounters' Trace Events.
**
**  "sample" - Enable sampling: the Lua interpreter generating events after it
**    executes a fixed number of instructions.
**
**    GRAPH: When "instrument" or "memory" are not enabled, this will dump the
**      thread that generated the LUA_HOOKCOUNT and attempt to build the graph
**      structure from the "leaves" upwards. Note, this will only do "raw"
**      counts.
**
**    TRACE: Only enabled for "single_thread" profiling. Will generate an
**      additional "timeline" of EvaluateScript events that correspond to the
**      duration of the 'sample', i.e., execution of X number of instructions.
**
**  "lines" - Enable line instrumentation: the Lua interpreter generating events
**    whenever it is about to start the execution of a new line of code, or when
**    it jumps back in the code.
**
**    GRAPH: 'Aggregated' activation records are formed by hashing a
**                <function_id, parent_id, parent_line>
**      triple. When the 'line_freq' option is enabled: each aggregated
**      activation will also record a line-execution frequency list (presuming
**      the record is a Lua function).
**
**    TRACE: Generates additional timeline events. Note, this function should
**      never be enabled.
*/
LUALIB_API int lmprof_start(lua_State *L);

/*
** stop([output_path]): Stop the profiler singleton and build its results. If no
**  output_path is supplied a formatted Lua table or string is returned.
**  Otherwise, the success of the IO (true/false) is returned.
**
**  lmprof_report.h contains the specifications for the different "result"
**  structures depending on the profile mode.
**
**  output_path - a file-path string where the formatted results are written.
**    For 'graph' profiling, the generated output is a Lua compatible table that
**    can be loaded with 'require' or 'dofile'. Meanwhile, 'trace' profiling
**    will generate a JSON file.
**
** @NOTE: output_path requires LMPROF_FILE_API to be enabled (see 'has_io').
*/
LUALIB_API int lmprof_stop(lua_State *L);

/* quit(): Preempt any active profiler state without reporting its results. */
LUALIB_API int lmprof_quit(lua_State *L);

/*
** Additional profiling inputs.
**
** lmprof_profile_X(input, output_path, ...): Profile the 'input' the object.
** Placing the result of the profiling (see lmprof_stop) onto the stack. Note,
** all trailing arguments correspond to its mode (see lmprof_start).
**
**  output_path - Optional filepath to write formatted results, see 'stop'. All
**    other arguments correspond to its mode. This function require an explicit
**    'nil' output_path parameter to denote the result is to be returned as a
**    Lua object.
**
** @NOTE: output_path requires LMPROF_FILE_API to be enabled (see 'has_io').
*/

/*
** file(input_path, output_path, ...):
**
** Profile the script at the given path.
*/
LUALIB_API int lmprof_profile_file(lua_State *L);

/*
** string(input, output_path, ...):
**
** Profile the given input script (loading with luaL_loadstring).
*/
LUALIB_API int lmprof_profile_string(lua_State *L);

/*
** func(input, output_path, ...):
**
** profile the given function argument (invoking with lua_pcall).
*/
LUALIB_API int lmprof_profile_function(lua_State *L);

/*
** Set a default profiling/configuration flag.
**
** Options:
**  General Options: [BOOL]
**    'micro' - Ensure times are measured on a microsecond scale.
**    'disable_gc' - Disable the Lua garbage collector for the duration of the
**      profile.
**    'gc_count' - Include LUA_GCCOUNT (the amount of memory in use by Lua)
**      information on profiler initialization. Note, this value will not
**      include memory managed by external C libraries that use lua_getallocf.
**    'reinit_clock' - Reinitialize, e.g., QueryPerformanceFrequency, the
**      profiler clock prior to profiling.
**    'mismatch' - allow call stack mismatching, i.e., start/stop not called in
**      same scope.
**    'load_stack' - Populate each profile stack with its traceback on
**      instantiation. Note, this option is closely related to mismatch.
**    'line_freq' - Create a frequency list of line-executions for each profiled
**      Lua function (graph instrumentation; requires "line" mode).
**    'compress_graph' - When enabled a lmprof_Record instance will represent
**      all activations of the same function, i.e., a function can have multiple
**      parent records. Otherwise, each record represents a single function
**      instance, i.e., a single parent/child relationship.
**
**      If the 'lines' mode is enabled, lmprof_Record instances will include
**      line number of the callsite in the parent/child association. This option
**      closely resembles the V8 CpuProfilingMode enumerated type
**    'output_string' - Output a string representation of the formatted output.
**        GRAPH - A Lua table; see '_G.load'
**        TRACEEVENT - A formatted JSON string.
**
**  General Options: [INTEGER]
**    'instructions' - Number of Lua instructions to execute before generating a
**      'sampling' event: LUA_MASKCOUNT.
**    'hash_size' - Default number of buckets in the hash (graph) table (limited
**      to 1031).
**
**  Trace Event Options: [BOOL]
**    'compress' - Suppress Trace Event records with durations less than the
**      provided 'threshold'. Unit of time defined by lmprof_get_timeunit.
**    'ignore_yield' - Ignore all coroutine.yield() records. This option
**      requires the coroutine.yield global definition to exist.
**    'draw_frame' - Enable BeginFrame support for trace events, i.e., each
**      'frame' corresponds to length of a coroutines execution; the time
**      between successive resume/yield calls.
**    'split' - Output a unique thread ids for each thread in the chromium
**      output; Otherwise, all events use the main thread and stack elements are
**      artificially pushed/popped when the coroutine resume/yields.
**    'tracing' - Output a format compatible with chrome://tracing/.
**
**  Trace Event Options: [INTEGER]
**    'process' - Synthetic Trace Event process ID.
**    'counter_freq' - Frequency of 'UpdateCounters' event generation.
**    'page_limit' - TraceEvent buffer size, i.e., maximum pages amount in
**      bytes (zero = infinite)
**    'threshold' - TraceEvent suppression threshold in microseconds, requires
**      'compress' to be enabled.
**
**  Trace Event Options: [STRING]
**    'name' - Synthetic Trace Event 'TracingStartedInBrowser' Name.
**    'url' - Synthetic Trace Event 'TracingStartedInBrowser' URL.
**
*/
LUALIB_API int lmprof_set_option(lua_State *L);
LUALIB_API int lmprof_get_option(lua_State *L);

/*
** time_unit(): Push the 'base' timing unit (compile-time defined) label onto
**  the stack; returning 1.
**    'nano' - Nanoseconds
**    'micro' - Microseconds
**    'rdtsc' - Processor time stamp (tickcount).
**    'Krdtsc' - Process time stamp in thousands.
** This value is compile-time dependent, i.e., 32-bit builds force microsecond
** measurements and '-DLMPROF_RDTSC' forces the usage of the RDTSC instruction.
*/
LUALIB_API int lmprof_get_timeunit(lua_State *L);

/*
** Pushes two booleans onto the stack, has_file_io and requires_output, and
** returns 2.
**
**   'has_file_io' - True if the profiler is configured for File/IO; false o.w.
**   'requires_output' - All 'output_path' parameters are required even if
**      has_file_io is disabled.
**
** @SEE LMPROF_FILE_API and LMPROF_DISABLE_OUTPUT_PATH build options.
*/
LUALIB_API int lmprof_get_has_io(lua_State *L);

/*
** Ignore Table: A registry table that contains references to functions that are
** to be suppressed in generated profile output. As these objects are stored in
** the registry, only global functions, library functions, or functions stored
** in upvals should be 'ignored'.
**
** The initial implementation was designed for the Trace Event API in mind.
** Allowing it to skip the output of specific (and useless) function records.
**
** @TODO: A path based ignore function: ignore measuring any execution path
** rooted at an ignored function.
*/

/*
** ignore(...): Register all function arguments as 'ignored'. Returning nothing.
*/
LUALIB_API int lmprof_ignored_function_add(lua_State *L);

/* unignored(...): Unregister/Unignore all 'ignored' function arguments.
**  Returning nothing.
*/
LUALIB_API int lmprof_ignored_function_remove(lua_State *L);

/*
** is_ignored(...): Check whether the (function-)arguments are listed as ignored.
** Pushing true/false onto the stack for each argument passed to the function.
*/
LUALIB_API int lmprof_is_ignored_function(lua_State *L);

/*
** MODE SPECIFIC
*/

/*
** Name Table: A registry subtable that associates a Lua thread with a script or
** user supplied label.
**
** When a profiler instance is stopped the status of each thread in the table is
** queried: removing 'dead' coroutines to ensure proper garbage collection of
** the object.
*/

/*
** get_name(): Find any name/label associated with the active lua_State. Pushing
** it onto the stack.
**
** If a name has not been associated with the state: attempt to derive one from
** 'lua_getstack' by brute-forcing the largest 'level' value. Ideally this is
** the first function after base_ci.
*/
LUALIB_API int lmprof_get_name(lua_State *L);

/* set_name(name): Associate a name/label with the active lua_State. */
LUALIB_API int lmprof_set_name(lua_State *L);

/* Generate an artificial timeline.frame.BeginFrame trace event */
LUALIB_API int lchrome_trace_event_beginframe(lua_State *L);

/* Generate an artificial timeline.frame.ActivateLayerTree trace event */
LUALIB_API int lchrome_trace_event_endframe(lua_State *L);

/* }================================================================== */

#if defined(__cplusplus)
}
#endif

/******************************************************************************
* lmprof
* Copyright (C) 2021 - gottfriedleibniz
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/

#endif

