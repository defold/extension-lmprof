local M = {}

local DEFAULT_OUTPUT = "mem.perfetto-trace"

local function output_paths(filename, path)
    if filename == nil and path == nil then
        return nil
    end

    filename = filename or DEFAULT_OUTPUT
    path = path and path or ""
    if type(filename) == "table" then
        local outputs = {}
        for i, name in ipairs(filename) do
            outputs[i] = path..name
        end
        return outputs
    end
    return path..filename
end

function M.stop(filename, path)
    _G.profilerlmprof:stop_to_files(output_paths(filename, path))
end

function M.start()
    if _G.profilerlmprof then
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
