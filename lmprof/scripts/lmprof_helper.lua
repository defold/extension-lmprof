local M = {}

local function is_json(filename)
    return type(filename) == "string" and filename:lower():sub(-5) == ".json"
end

local function output_label(output)
    if type(output) == "table" then
        return table.concat(output, ", ")
    end
    return output
end

local function output_paths(filename, path)
    filename = filename and filename or "mem.perfetto-trace"
    if type(filename) == "table" then
        local outputs = {}
        for i, name in ipairs(filename) do
            outputs[i] = path..name
        end
        return outputs
    end
    return path..filename
end

local function no_c_calls_path(filename)
    return filename:gsub("([^/]+)$", "no_c_calls_%1")
end

local function remove_c_calls_from_json(filename)
    local report_postprocess = require("lmprof.scripts.report_postprocess")
    report_postprocess.filter_out_lines_with(filename, no_c_calls_path(filename), {
        -- "%? %[C%]", -- remove "? [C]"
        -- "%(for generator%) %[C%]", -- remove "(for generator) [C]"
        -- "self %[C%]" -- remove "self [C]"
        "%[C%]" -- remove all "[C]" calls
    })
end

function M.stop(filename, path, remove_c_calls)
    path = path and path or ""
    local fullpath = output_paths(filename, path)
    if not _G.profilerlmprof:stop(fullpath) then
        error("Failure!")
    end
    print("Result of profiling has been saved on disk: "..output_label(fullpath))

    if remove_c_calls and type(fullpath) == "table" then
        local processed = false
        for _, output in ipairs(fullpath) do
            if is_json(output) then
                remove_c_calls_from_json(output)
                processed = true
            end
        end
        if not processed then
            print("remove_c_calls is only supported for .json trace output")
        end
    elseif remove_c_calls and is_json(fullpath) then
        remove_c_calls_from_json(fullpath)
    elseif remove_c_calls then
        print("remove_c_calls is only supported for .json trace output")
    end
end

function M.start()
    if _G.profilerlmprof then
        print("Start record!")
        _G.profilerlmprof:start()
        _G.profilerlmprof:begin_frame()
        return true
    else
        error("lmprof isn't inited")
    end
end

function M.init()
    if lmprof then
        lmprof.set_option("gc_count", true)
        lmprof.set_option("mismatch", true)
        lmprof.set_option("micro", true)
        lmprof.set_option("compress_graph", true)
        lmprof.set_option("compress", true)
        lmprof.set_option("threshold", 100) -- 1000 microseconds = 1 ms
        _G.profilerlmprof = lmprof.create("instrument", "memory", "trace"):calibrate()
    else
        error("lmprof isn't installed https://github.com/defold/extension-lmprof")
    end
end

function M.update()
    if lmprof and _G.profilerlmprof:get_state("running") then
        _G.profilerlmprof:end_frame()
        _G.profilerlmprof:begin_frame()
    end
end

return M
