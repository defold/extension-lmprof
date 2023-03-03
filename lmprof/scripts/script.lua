--[[
    lmprof CLI tool.

@USAGE
    ./lua scripts/script.lua --input=scripts/test/2html.lua --output=docs/graph_2html.lua \
        --memory --micro

    lua.exe script.lua --input=codegen.lua --args="inp/natives_global.lua lua" \
        --output=out.json --memory --trace --compress=1000 --memory --counter_freq=256

@LICENSE
    See Copyright Notice in lmprof_lib.h
--]]
lmprof = lmprof--require('lmprof') or lmprof -- @NOTE: LUA_PATH

--[[ Default arguments --]]
function PrintUsage(errorMessage)
    local usageMessage = [[
script [--input] [--output] [--format] [--path] [--args] [-h | --help]
  [-t | --time] [-m | --memory] [-e | --trace] [-l | --lines] [-s | --sample] [--single_thread]
  [--micro] [--compress_graph] [--load_stack] [--mismatch] [--line_freq] [--ignore_yield] [--gc_count] [-g | --disable_gc] [-i | --instructions]
  [-p | --process] [-f | --draw_frame] [-c | --compress] [--split] [--tracing] [--page_limit] [--name] [--url]
  [--callgrind] [--pepper] [--json] [--sort] [--csv] [--show_lines] [-v | --verbose]

[INPUT]
  --input: input script file
  --output: optional output location (default: STDOUT)
  --format: format/reduce an already generated profiler output.
  --path: Additional package.searchpath string for module searching ('export LUA_PATH' alternative).
  --args: Command line arguments to forward to the input script file.
  --help: help!

[MODES]
  [Instrumenting]:
    --time: Enable 'base' timing.
    --memory Enable 'base' Lua allocator tracking.
    --trace: Enable DevTools timeline profiling.
    --lines: Enable line frequency (or line event) profiling.

  [Sampling]:
    --sample: Enable Lua instruction sampling ('time' and 'memory' cannot be enabled.)

[OPTIONS]
  [Base]:
    --micro: Ensure times are measured on a microsecond scale.
    --compress_graph: A 'compressed' graph representation: all instances of the same function are accumulated in one node.
    --load_stack: Populate each profile stack with its traceback on instantiation.
    --mismatch: allow start/stop mismatching (i.e., empty profile stacks) during execution.
    --line_freq: Create a frequency list of line-executions for each profiled Lua function (graph instrumentation)
    --ignore_yield: Ignore all coroutine.yield() records.
    --gc_count: Include LUA_GCCOUNT (the amount of memory in use by Lua) information on profiler initialization.
    --disable_gc: Disable the Lua garbage collector for the duration of the profile.
    --instructions=count: Number of Lua instructions to execute before generating a 'sampling' event.
    --calibrate: perform a calibration, i.e., determine an estimation, preferably an underestimation, of the Lua function call overhead.
    --output_string: Output a formatted Lua string instead of writing (antithesis to LMPROF_FILE_API)

  [TraceEvent]:
    --process=value: Synthetic process ID.
    --compress=time: Suppress Trace Event records with durations less than 'time'
    --draw_frame: Enable BeginFrame support for trace events (i.e., each 'frame' corresponds to execution of a single coroutine between resume/yields)
    --split: Output a unique thread ids for each thread in the chromium output; Otherwise, all events use the main thread.
    --tracing: Output a format compatible with chrome://tracing/.
    --page_limit: TraceEvent buffer size in bytes
    --counter_freq: Frequency of 'UpdateCounters' event generation.
    --name: Synthetic 'TracingStartedInBrowser' Name.
    --url: Synthetic 'TracingStartedInBrowser' URL.

  [Output]:
    --callgrind: Callgrind compatible layout
    --pepper: Pepperfish style layout (optional, default: false)
    --json: Write 'base' profiling output as formatted JSON.
    --sort: result sorting algorithm [count, size, time] (optional, default: count)
    --csv: Comma-separated flat output
    --show_lines: Show line frequencies in generated output.
    --verbose: Output debug/verbose strings
]]

    print(usageMessage)
    if errorMessage then
        error(tostring(errorMessage))
    end
end

--[[ http://lua-users.org/wiki/AlternativeGetOpt --]]
function ParseArguments(arg, optionString)
    local options = setmetatable({ }, { __index = {
        String = function(self, long, short, default)
            local o = self[long] or self[short] or default
            return (type(o) == "string" and o) or nil
        end,

        Int = function(self, long, short, default)
            local o = self[long] or self[short] or default
            return (type(o) == "number" and o) or tonumber(o)
        end,

        Bool = function(self, long, short, default)
            local o = self[long] or self[short]
            if o == nil then
                o = default
            end

            if type(o) == "boolean" then
                return o
            elseif type(o) == "string" then
                return string.lower(o) ~= "false"
            end
            return default
        end,
    }})

    for k, v in ipairs(arg) do
        if v:sub(1, 2) == "--" then
            local x = v:find("=", 1, true)
            if x then
                options[v:sub(3, x - 1)] = v:sub(x + 1)
            else
                options[v:sub(3)] = true
            end
        elseif v:sub(1, 1) == "-" then
            local y = 2
            local l = v:len()
            while (y <= l) do
                local shortOption = v:sub(y, y)
                if optionString:find(shortOption, 1, true) then
                    if y < l then
                        options[shortOption] = v:sub(y + 1)
                        y = l
                    else
                        options[shortOption] = arg[k + 1]
                    end
                else
                    options[shortOption] = true
                end
                y = y + 1
            end
        end
    end
    return options
end

---------------------------------------
-------------- Execution --------------
---------------------------------------

-- This CLI tool requires lmprof to be compiled with LMPROF_FILE_API.
local has_io,_ = lmprof.has_io()
if not has_io then
    error("lmprof library must be compiled with LMPROF_FILE_API")
end

local options = ParseArguments(arg, "")
local input = options:String("input", "", nil)
local output = options:String("output", "", nil)
if options:Bool("help", "h", false) then
    PrintUsage()
    return
elseif not input then
    PrintUsage("Input Required")
    return
end

--[[ MODES: The profiling library will sanitize conflicting mode parameters. ]]
local time_mode = (options:Bool("time", "t", false) and "time") or nil
local sample_mode = (options:Bool("sample", "s", false) and "sample") or nil
local trace_mode = (options:Bool("trace", "e", false) and "trace") or nil
local memory_mode = (options:Bool("memory", "m", false) and "memory") or nil
local line_mode = (options:Bool("lines", "l", false) and "lines") or nil
local single_thread = (options:Bool("single_thread", "", false) and "single_thread") or nil
if not trace_mode and not time_mode and not sample_mode then
    time_mode = "instrument"
elseif trace_mode then
    time_mode = "instrument"
end

--[[ OPTIONS --]]
lmprof.set_option("verbose", options:Bool("verbose", "v", false))
lmprof.set_option("output_string", options:Bool("output_string", "", false))
lmprof.set_option("micro", options:Bool("micro", "", false))
lmprof.set_option("compress_graph", options:Bool("compress_graph", "", true))
lmprof.set_option("load_stack", options:Bool("load_stack", "", true))
lmprof.set_option("mismatch", options:Bool("mismatch", "", false))
lmprof.set_option("line_freq", options:Bool("line_freq", "", false))
lmprof.set_option("ignore_yield", options:Bool("ignore_yield", "", false))
lmprof.set_option("gc_count", options:Bool("gc_count", "", false))
lmprof.set_option("disable_gc", options:Bool("disable_gc", "g", false))
if sample_mode then
    lmprof.set_option("instructions", options:Int("instructions", "i", 1000))
end

lmprof.set_option("name", input)
lmprof.set_option("url", ("%s %s"):format(input, table.concat(arg, " ")))
lmprof.set_option("process", options:Int("process", "p", 1))
lmprof.set_option("draw_frame", options:Bool("draw_frame", "f", false))
lmprof.set_option("split", options:Bool("split", "", false))
lmprof.set_option("tracing", options:Bool("tracing", "", false))
if trace_mode then
    local threshold = options:Int("compress", "c", 0)
    lmprof.set_option("compress", threshold > 0)
    lmprof.set_option("threshold", threshold)
    lmprof.set_option("counter_freq", options:Int("counter_freq", "", 1))
    lmprof.set_option("page_limit", options:Int("page_limit", "", 1))
end

-- Update package path
local searchpath = options:String("path", "", nil)
if searchpath ~= nil then
    package.path = ("%s;%s"):format(package.path, searchpath)
end

-- Temporarily hook the 'arg' table for the duration of the profile.
local prevArgs = arg
arg = { }
for w in options:String("args", "", ""):gmatch("%S+") do
    arg[#arg + 1] = w
end

-- If 'output' is nil, then the expected output is a table with "header" and
-- "records" subfields. This script also uses the 'userdata' implementation
-- for calibration.
local result = nil
if options:Bool("format", "", false) then
    result = dofile(input)
elseif options:Bool("calibrate", "", false) then
    result = lmprof.create(time_mode, memory_mode, trace_mode, line_mode, sample_mode, single_thread)
        :calibrate()
        :file(input, output)
else
    result = lmprof.file(input, output, time_mode, memory_mode, trace_mode, line_mode, sample_mode, single_thread)
end

arg = prevArgs
if output then
    if result ~= nil then
        print(("Written To: %s"):format(output))
    else
        error(("Failure writing to: %s"):format(output))
    end
elseif options:Bool("json", "j", false) or trace_mode then
    if options:Bool("output_string", "", false) then
        print(result)
    else
        json = require('lmprof/scripts/dkjson')
        print(json.encode(result.records, {
            level = 0, indent = true,
            keyorder = { "cat", "name", "ph", "pid", "tid", "ts", "args",},
        }))
    end
elseif options:Bool("output_string", "", false) then
    print(result)
else
    local Graph = require('lmprof/scripts/graph')

    result.header.sort = options:String("sort", "", nil)
    result.header.csv = options:Bool("csv", "", false)
    result.header.show_lines = options:Bool("show_lines", "", false)
    if line_mode and options:Bool("line_freq", "", false) then
        result.header.show_lines = true
    end

    local graph = Graph(result.header, result.records)
    if output == nil and options:Bool("verbose", "v", false) then
        graph:Verbose(result.header)
    end

    if options:Bool("callgrind", "", false) then
        graph:Callgrind(result.header)
    elseif options:Bool("pepper", "p", false) then
        graph:Pepperfish(result.header)
    else
        graph:Flat(result.header)
    end
end
