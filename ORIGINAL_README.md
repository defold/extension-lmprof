# lmprof
A flexible instrumenting and sampling library for Lua 5.1, Lua 5.2, Lua 5.3, and Lua 5.4 and extension of the [Lua Debug Interface](https://www.lua.org/manual/5.4/manual.html#4.7).

> In Development

## Overview
The goal of this library is to create a flexible and relatively efficient profiling framework for Lua that builds upon the [Lua Debug Interface](https://www.lua.org/manual/5.4/manual.html#4.7). Providing tools and APIs focused on the collection, aggregation, measurement, and representation of activation records.

The current iteration of the library supports two forms of profiling: Graph Profiling and Event Profiling. See [docs](docs/) for example output(s).

### Graph Profiling
The creation of graphs that relate activation records and measurements about them, e.g., [Pepperfish](http://lua-users.org/wiki/PepperfishProfiler) and [Callgrind](http://kcachegrind.sourceforge.net/html/CallgrindFormat.html). An instrumenting graph profiler follows the general outline:

1. A [call stack](src/collections/lmprof_stack.h) maintains a sequence of instances: an activation record plus local statistics, e.g., time of `LUA_HOOKCALL/LUA_HOOKRET`, [Lua allocator](https://www.lua.org/manual/5.4/manual.html#lua_Alloc) use, tail-call information, etc.

  - Each coroutine is allocated its own profiler stack. The `single_thread` profile mode can be used to prevent multiple coroutines ('thread' used interchangeably in this library) from being profiled.

  - A sampling profiler based on `LUA_MASKCOUNT` rebuilds the [Lua stack](https://www.lua.org/manual/5.4/manual.html#lua_getstack) on each callback. Note, `LUA_MASKCOUNT` events only happen while the interpreter is executing a Lua function.

  - Specific profiler configurations may also use the line hook, `LUA_MASKLINE`, to generate frequency charts, etc.

1. Call instances are [aggregated](src/collections/lmprof_record.h) into a collection of profile records: an 'aggregated' activation record plus statistics about all function instances that share the same activation, e.g., time spent in the function, amount of memory allocated while in the function, time spent in the children of the function, etc.

  - The label/names of 'aggregated' activation record are resolved as the first non tail-call call that provides information (e.g., name/namewhat) about the function being profiled.

  - Various techniques exist for aggregating functions: shared `GCObject` pointers, Lua closure pointers, hashing the fields of `lauxlib.pushfuncname`, function call site (e.g., parent line number) information, etc. See the `LMPROF_BUILTIN` build option.

1. Profile records are maintained in a parent/child [structure](src/collections/lmprof_hash.h). That structure can be [formatted](src/lmprof_report.h) into a Lua table, a [string](https://www.lua.org/manual/5.4/manual.html#pdf-load) representation of the table, or written to file as a Lua [script](https://www.lua.org/manual/5.4/manual.html#pdf-dofile).

  - Future iterations may alleviate this condition, e.g., a graph structure per coroutine.

### Event Profiling
Creating *events* that can be used to generate flamegraphs, e.g., [devtools-frontend](https://github.com/ChromeDevTools/devtools-frontend) and [speedscope](https://github.com/jlfwong/speedscope).

1. A call stack maintains `CallFrame` data and tailcall information of the activation record. Depending on its context callframes are formatted and buffered as [Trace Events](src/collections/lmprof_traceevent.h) for the duration of the profile.

1. Trace Events are post-processed, e.g., (1) times shifted to zero to avoid potential down-casting issues; and (2) `LUA_HOOKCALL/LUA_HOOKRET` event pairings that fall under a certain execution threshold are optionally suppressed.

1. Trace Events can then be formatted into a Lua table or application specific representation, e.g., a DevTools compatible JSON.

## Documentation
The exported API is broken down into four categories: **Configuration**, **Profiling**, **Miscellaneous**, and **Local State**. See **Developer Notes** for implementation details/caveats.

### Configuration
```lua
-- Set a default profiling/configuration flag.
--
-- Options:
--  General Options: [BOOL]
--    'micro' - Ensure times are measured on a microsecond scale. Note, see
--      lmprof.time_unit() for the base measurement of time.
--    'disable_gc' - Disable the Lua garbage collector for the duration of the
--      profile.
--    'gc_count' - Include LUA_GCCOUNT (the amount of memory in use by Lua)
--      information on profiler initialization. Note, this value will not
--      include memory managed by external C libraries that use lua_getallocf.
--    'reinit_clock' - Reinitialize, e.g., QueryPerformanceFrequency, the
--      profiler clock prior to profiling.
--    'mismatch' - allow call stack mismatching, i.e., start/stop not called in
--      same scope.
--    'load_stack' - Populate each profile stack with its traceback on
--      instantiation. Note, this option is closely related to mismatch.
--    'line_freq' - Create a frequency list of line-executions for each profiled
--      Lua function (graph instrumentation; requires "line" mode).
--    'compress_graph' - When enabled a lmprof_Record instance will represent
--      all activations of the same function, i.e., a function can have multiple
--      parent records. Otherwise, each record represents a single function
--      instance, i.e., a single parent/child relationship.
--
--      If the 'lines' mode is enabled, lmprof_Record instances will include
--      line number of the callsite in the parent/child association. This option
--      closely resembles the V8 CpuProfilingMode enumerated type
--    'output_string' - Output a string representation of the formatted output.
--        GRAPH - A Lua table; see '_G.load'
--        TRACEEVENT - A formatted JSON string.
--
--  General Options: [INTEGER]
--    'instructions' - Number of Lua instructions to execute before generating a
--      'sampling' event: LUA_MASKCOUNT.
--    'hash_size' - Default number of buckets in the hash (graph) table (limited
--      to 1031).
--
--  Trace Event Options: [BOOL]
--    'compress' - Suppress Trace Event records with durations less than the
--      provided 'threshold'. Unit of time defined by 'lmprof.time_unit'.
--    'ignore_yield' - Ignore all coroutine.yield() records. This option
--      requires the coroutine.yield global definition to exist.
--    'draw_frame' - Enable BeginFrame support for trace events, i.e., each
--      'frame' corresponds to length of a coroutines execution; the time
--      between successive resume/yield calls.
--    'split' - Output a unique thread ids for each thread in the chromium
--      output; Otherwise, all events use the main thread and stack elements are
--      artificially pushed/popped when the coroutine resume/yields.
--    'tracing' - Output a format compatible with chrome://tracing/.
--
--  Trace Event Options: [INTEGER]
--    'process' - Synthetic Trace Event process ID.
--    'counter_freq' - Frequency of 'UpdateCounters' event generation.
--    'page_limit' - Trace Event buffer size, i.e., maximum pages amount in
--      bytes (zero = infinite)
--    'threshold' - Trace Event suppression threshold in microseconds, requires
--      'compress' to be enabled.
--
--  Trace Event Options: [STRING]
--    'name' - Synthetic 'TracingStartedInBrowser' Name.
--    'url' - Synthetic 'TracingStartedInBrowser' URL.
value = lmprof.get_option(option)

-- Set a global encoding/decoding option; see lmprof.get_option.
lmprof.set_option(option, value)

-- Return the 'base' timing unit (prior to any option switches)
--    'nano' - Nanoseconds
--    'micro' - Microseconds
--    'rdtsc' - Processor time stamp (tickcount).
--    'Krdtsc' - Process time stamp in thousands.
-- This value is compile-time dependent, i.e., 32-bit builds force microsecond
-- measurements, '-DLMPROF_RDTSC' force the usage of the RDTSC instruction.
value = lmprof.time_unit()

-- Pushes two booleans onto the stack, has_file_io and requires_output, and
-- returns 2.
--
--   'has_file_io' - True if the profiler is configured for File/IO; false o.w.
--   'requires_output' - All 'output_path' parameters are required even if
--      has_file_io is disabled.
--
-- @SEE LMPROF_FILE_API and LMPROF_DISABLE_OUTPUT_PATH build options.
value = lmprof.has_io()
```

### Profiling
```lua
-- start(...): start a profiler instance. Returning a profiler userdata (local
--  state) on success, nil otherwise.
--
-- This function registers the profiler singleton in the registry and may
-- (depending on profiling mode) override (wrap) the active Lua allocator. While
-- a profiler is active debug.sethook and lua_setallocf should not be used. Any
-- calls to lmprof.start will throw an error.
--
-- Modes:  The profiling configuration arguments are passed in as strings:
--  "time" - [TIME] The time between successive start/stop operations.
--
--  "instrument" - [GRAPH] Measurements between successive lua_Hook calls and
--    track the relationships between functions.
--
--  "trace" - [TRACE] Generate events compatible with DevTools
--
--  "single_thread" - Single thread profiling. Ignore all threads except the one
--    that invoked 'start'.
--
--  "memory" - Enable memory profiling by hooking the Lua allocator.
--    GRAPH: Track the memory usage between successive lua_Hook calls.
--    TRACE: Generate 'UpdateCounters' Trace Events between successive lua_Hook
--      calls.
--
--  "sample" - Enable sampling: the Lua interpreter generating events after it
--    executes a fixed number of instructions.
--
--    GRAPH: When "instrument" or "memory" are not enabled, this will dump the
--      thread that generated the LUA_HOOKCOUNT and attempt to build the graph
--      structure from the "leaves" upwards. Note, this will only do "raw"
--      counts.
--
--    TRACE: Only enabled for "single_thread" profiling. Will generate an
--      additional "timeline" of EvaluateScript events that correspond to the
--      duration of the 'sample', i.e., execution of X number of instructions.
--
--  "lines" - Enable line instrumentation: the Lua interpreter generating events
--    whenever it is about to start the execution of a new line of code, or when
--    it jumps back in the code.
--
--    GRAPH: 'Aggregated' activation records are formed by hashing a
--                <function_id, parent_id, parent_line>
--      triple. When the 'line_freq' option is enabled: each aggregated
--      activation will also record a line-execution frequency list (presuming
--      the record is a Lua function).
--
--    TRACE: Generates additional timeline events. Note, this function should
--      never be enabled.
--
state = lmprof.start(...)

-- stop([output_path]): Stop the profiler singleton and build its results. If no
--  output_path is supplied a formatted Lua table or string is returned.
--  Otherwise, the success of the IO (true/false) is returned.
--
--  lmprof_report.h contains the specifications for the different "result"
--  structures depending on the profile mode.
--
--  output_path - a file-path string where the formatted results are written.
--    For 'graph' profiling, the generated output is a Lua compatible table that
--    can be loaded with 'require' or 'dofile'. Meanwhile, 'trace' profiling
--    will generate a JSON file.
--
-- *NOTE*: output_path requires LMPROF_FILE_API to be enabled (see 'has_io').
result = lmprof.stop([output_path])

-- quit(): Preempt any active profiler state without reporting its results.
lmprof.quit()

-- Additional profiling inputs.
--
-- lmprof_profile_X(input, output_path, ...): Profile the 'input' the object.
-- Placing the result of the profiling (see lmprof_stop) onto the stack. Note,
-- all trailing arguments correspond to its mode (see lmprof_start).
--
--  output_path - Optional filepath to write formatted results, see 'stop'. All
--    other arguments correspond to its mode. This function require an explicit
--    'nil' output_path parameter to denote the result is to be returned as a
--    Lua object.
--
-- *NOTE*: output_path requires LMPROF_FILE_API to be enabled (see 'has_io').

-- file(input_path, output_path, ...):
--
-- Profile the script at the given path.
result = lmprof.file(input_path, output_path, ...)

-- string(input, output_path, ...):
--
-- Profile the given input script (loading with luaL_loadstring).
result = lmprof.string(input, output_path, ...)

-- func(input, output_path, ...):
--
-- profile the given function argument (invoking with lua_pcall).
result = lmprof.func(input, output_path, ...)
```

### Miscellaneous
Additional library functions used to supplement the core profiling functions above.

```lua
-- Generate an artificial timeline.frame.BeginFrame trace event
lmprof.begin_frame()

-- Generate an artificial timeline.frame.ActivateLayerTree trace event
lmprof.end_frame()
```

##### Ignore/Suppress Table
A registry subtable that maintains references to functions that are to be suppressed in the generated profile output. As these values are stored in the registry only global functions, library functions, or functions stored in upvals should be 'ignored'.

The current implementation was designed for the Trace Event API in mind: allowing it to ignore the output of specific (and useless) function records.

```lua
-- Register all function arguments as 'ignored'; returning nothing.
lmprof.ignore(...)

-- Unregister/Unignore all 'ignored' function arguments; returning nothing.
lmprof.unignore(...)

--  Check whether the (function-)arguments are listed as ignored. Returning
-- true/false for each argument passed to the function.
... = lmprof.is_ignored(...)
```

##### Label Table
A registry subtable that associates a Lua thread with a script or user supplied label. When a profiler instance is stopped the status of each labeled thread is queried: removing 'dead' coroutines to ensure proper garbage collection of the thread object.

```lua
-- get_name(): Return the name/label associated with the active lua_State. If a
-- name has not been associated with the state: attempt to derive one from
-- 'lua_getstack' by brute-forcing the largest 'level' value. Ideally this is
-- the first function after base_ci.
name = lmprof.get_name()

-- set_name(label): Associate a name/label with the active lua_State.
lmprof.set_name(name)
```

### Local State
A locally configurable profiling userdata (also returned by `lmprof.start`) copying all global options on instantiation. For Lua 5.4, this userdata may be configured as to-be-closed to ensure that a profiler can never exit a scope without it automatically stopping (to-be-closed caveats apply).

```lua
-- create: Create a profiler instance with a default 'mode' configuration.
state = lmprof.create(...)
```

#### Local Methods
```lua
-- start: See lmprof.start()
self = state:start()

-- stop: See lmprof.stop()
result = state:stop([output_path])

-- quit: See lmprof.quit()
state:quit()

-- get_option: See lmprof.get_option
value = state:get_option(option)

-- See lmprof.get_option.
self = state:set_option(option, value)

-- See lmprof.file
result = state:file(input_path[, output_path], ...)

-- See lmprof.string
result = state:string(input[, output_path], ...)

-- See lmprof.func
result = state:func(input[, output_path], ...)

-- See lmprof.begin_frame()
self = state:begin_frame()

-- See lmprof.end_frame()
self = state:end_frame()

-- calibrate: Perform a calibration, i.e., determine an estimation, preferably
-- an underestimation, of the Lua function call overhead. The result can be used
-- to increase profiling precision.
self = state:calibrate()

-- Get a profiling state flag. Available options:
--  'running' - Profiler is active and collecting statistics.
--  'error' - Profiler in an irrecoverable state.
--  'restore_gc' - Garbage collector was running to profiling and the state must
--    be restored.
value = state:get_state(opt)

-- get_mode: Return all profile mode strings active on this profiler, e.g.,
-- instrumental, memory, trace, etc.
... = state:get_mode(mode)

-- set_mode: Change the profiling mode of the given profiler state, e.g.,
-- state:set_mode('trace', 'instrument')
self = state:set_mode([...])
```

#### Example
```lua
-- Create a reusable state with its own local configuration
local profiler = lmprof.create("instrument", "memory")
    :set_option("load_stack", true)
    :set_option("mismatch", true)
    :set_option("compress_graph", true)
    :calibrate()

-- Profile something; writing graph to 'something.out'
profiler:start()
something()
if not profiler:stop("something.out") then
    error("Failure!")
end

-- Another way to profile something.
local result_2 = profiler:func(something)

-- Change the mode to 'time' and time how long 'something' takes to execute.
print(profiler:set_mode("time"):func(something))
```

## Building
A CMake project that builds the shared library is included. See `cmake -LAH` or [cmake-gui](https://cmake.org/runningcmake/) for the complete list of build options. For development purposes a Makefile (derivative of the makefile used by Lua) is also provided.

```bash
# Create build directory:
└> mkdir -p build ; cd build
└> cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release ..

# Using a custom Lua build (Unix). When using Windows, -DLUA_LIBRARIES= must also
# be defined for custom Lua paths. Otherwise, CMake will default to 'FindLua'
└> cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DLUA_INCLUDE_DIR=${LUA_DIR} ..

# Build
└> make
```

### Compile Options
- **LUA\_BIT32**: Library compiled for i386 (Linux only).
- **LUA\_COMPILED\_AS\_HPP**: Library compiled for Lua with C++ linkage.
- **LUA\_INCLUDE\_TEST**: Build with `LUA_USER_H="ltests.h"`.
- **LMPROF\_BUILTIN**: Link against internal Lua headers.
- **LMPROF\_FILE\_API**: Enable the usage of `luaL_loadfile` and other `stdio.h` functions. Otherwise, the Lua runtime is in charge of all IO and serialization.
- **LMPROF\_RDTSC**: Use the 'Read Time-Stamp Counter' instruction for 'timing' rather than any OS-specific high resolution performance-counter, e.g., [QueryPerformanceCounter](https://docs.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter).
- **LMPROF\_STACK\_SIZE**: Maximum size of each coroutines profiler stack.
- **LMPROF\_HASH\_SIZE**: Default bucket count in a hash table.
- **LMPROF\_HASH\_SPLITMIX**: Enable a splitmix inspired hashing function for the hash table.
- **LMPROF\_USE\_STRHASH**: Use luaS_hash. Otherwise, the default Jenkins one_at_a_time.
- **LMPROF\_RAW\_CALIBRATION**: Do not post-process the calibration overhead. By default the calibration data is halved to ensure most potential variability is accounted for.
- **TRACE\_EVENT\_PAGE\_SIZE**: The default TraceEventPage size.

## Usage
Each example assumes the lmprof library is in the same directory as the Lua executable. All referenced scripts are from [scripts](scripts/), with [script.lua](scripts/script.lua) being a command-line tool for operating the profiler as an independent shared module and [graph.lua](scripts/graph.lua) being a general formatting tool for "Base" profiling.

```bash
## Reminder: 'export LUA_PATH'

## Help: Print all options
./lua scripts/script.lua --help

## Profile and display a flat representation of the profile (sorted by call count: default)
## Note: --path="scripts/?.lua" can be used in lieu of LUA_PATH
./lua scripts/script.lua --input=scripts/test/2html.lua --memory

# Generate a frontend.chrome-dev.tools instrumentation that removes all function
# calls shorter than 500 nanoseconds while disabling the garbage collector
./lua scripts/script.lua --input=scripts/test/2html.lua --output=out.json --trace --memory --disable_gc --compress=500

# Generate a frontend.chrome-dev.tools instrumentation that:
#   1. --args: Passes the arguments "inp/natives_global.lua lua" to codegen.lua;
#   2. --output: Writes a DevTools compatible json to out.json;
#   3. --memory: Generate UpdateCounters events (memory usage);
#   4. --counter_freq: Only output every 256th UpdateCounter event (one is generated before/after every function call)
#   5. --compress: Remove all events whose execution time is less than one microsecond.
lua.exe script.lua --input=codegen.lua --args="inp/natives_global.lua lua" --output=out.json --memory --trace --compress=1000 --memory --counter_freq=256
```

## Developer Notes

### Planned Features
1. [cpuprofile-fileformat](https://gperftools.github.io/gperftools/cpuprofile-fileformat.html) support.
1. [perf.data](https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/perf.data-file-format.txt) support.
1. [DevTools Protocol](https://github.com/ChromeDevTools/devtools-protocol/blob/master/json/js_protocol.json) support, e.g., "ProfileNode" and other sampling features that can be mapped to Lua.
1. [fx::ProfilerComponent](https://github.com/citizenfx/fivem/blob/master/code/components/citizen-scripting-core/src/Profiler.cpp) support and [compact](https://github.com/msgpack/msgpack-c) binary representation of `TraceEventTimeline`.

### TODO
Ordered by priority.
1. Update example outputs in [docs](docs/): most are outdated.
1. Path-based `lmprof.ignore`: ignore a function and all of its descendants.
1. Experiment with Lua core profiling, e.g., use `LUA_USER_H`, `lua_lock`, `lua_unlock`, and `__FUNCTION__` to generate
profiler events. Fairly unobtrusive and **should** only require compile time Lua flags (hopefully?).
1. Include an API function to emulate v8's `CpuProfilingStatus`, e.g., `lmprof_check_can_profile` that returns a status code.
1. Include configuration options similar to v8's `CpuProfilingLoggingMode` and `CpuProfilingNamingMode`.
1. Handle RDTSC reset and `lu_time` overflows (especially on 32bit builds).
1. Refactor. See the note in the header of lmprof.c.
1. Rename bitfield macros to follow Lua's naming convention: a common prefix (`l_` or `lua`) for all exported declarations.
1. Improve [Callgrind Format Specification](https://valgrind.org/docs/manual/cl-format.html) support, e.g., multi-threaded layouts.
1. Experiment with non-uniform sampling, e.g., dynamically estimate a `LUA_MASKCOUNT` value that estimates sampling uniformly in the 'time' domain (instead of instructions). An alternative approach would to use OS-specific timers/signals to inform when sampling should occur (similar to LuaJIT).
1. Casting from uint64_t (time/size measurement counters) to lua_Integer creates potential down-casting issues: traceevent_adjust and OPT_CLOCK_MICRO exist as potential solutions.
1. Encoded screenshot support: a Lua table (or another simple linked pager) of base64 encoded strings with optional limits on the amount of data that can be buffered.

## Sources & Acknowledgments:
1. [Lua](https://www.lua.org/versions.html).
1. [lmprof](https://github.com/pmusa/lmprof): Original implementation from Pablo Musa.
1. [PepperfishProfiler](http://lua-users.org/wiki/PepperfishProfiler): Reference style of formatted output.
1. [Callgrind Format Specification](https://valgrind.org/docs/manual/cl-format.html): Reference.
1. [Trace Event Format](https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU): Original trace event specification and V8 [profiler specification](https://github.com/v8/v8/blob/44bd8fd7/src/inspector/js_protocol.json#L1399).
1. [devtools-frontend](https://github.com/ChromeDevTools/devtools-frontend): Chrome DevTools client and webapp; modified version used in generating images in [docs](docs/).

## License
lmprof is distributed under the terms of the [MIT license](https://opensource.org/licenses/mit-license.html); see [lmprof_lib.h](src/lmprof_lib.h)
