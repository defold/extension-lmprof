local M = {}

local function is_absolute_path(filename)
    if type(filename) ~= "string" then
        return false
    end
    return filename:sub(1, 1) == "/" or filename:sub(1, 1) == "\\" or filename:match("^%a:[/\\]") ~= nil
end

local function path_join(path, filename)
    if path == nil or path == "" or is_absolute_path(filename) then
        return filename
    end

    local separator = path:sub(-1)
    if separator == "/" or separator == "\\" then
        return path..filename
    end

    return path.."/"..filename
end

local function project_id()
    if type(sys) == "table" then
        local getters = { "get_config_string", "get_config" }
        for _, getter in ipairs(getters) do
            if type(sys[getter]) == "function" then
                local ok, value = pcall(sys[getter], "project.title", "lmprof")
                if ok and type(value) == "string" and value ~= "" then
                    return value
                end
            end
        end
    end
    return "lmprof"
end

local function save_path(filename)
    if is_absolute_path(filename) then
        return filename
    end
    if type(sys) == "table" and type(sys.get_save_file) == "function" then
        return sys.get_save_file(project_id(), filename)
    end
    return filename
end

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
            outputs[i] = path == nil and save_path(name) or path_join(path, name)
        end
        return outputs
    end
    return path == nil and save_path(filename) or path_join(path, filename)
end

local function no_c_calls_path(filename)
    return filename:gsub("([^/\\]+)$", "no_c_calls_%1")
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
