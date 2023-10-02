local M = {}

function M.stop(filename, path, remove_c_calls)
    path = path and path or ""
    filename = filename and filename or "mem.json"
    local fullpath = path..filename
    if not _G.profilerlmprof:stop(fullpath) then
        error("Failure!")
    end
    print("Result of profiling has been saved on disk: "..fullpath)

    if remove_c_calls then
        local report_postprocess = require("lmprof.scripts.report_postprocess")
        filename = "no_c_calls_"..filename
        report_postprocess.filter_out_lines_with(fullpath, filename, {
            -- "%? %[C%]", -- remove "? [C]"
            -- "%(for generator%) %[C%]", -- remove "(for generator) [C]"
            -- "self %[C%]" -- remove "self [C]"
            "%[C%]" -- remove all "[C]" callse
        })
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
