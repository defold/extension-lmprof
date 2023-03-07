# lmprof native extension for Defold Game Engine

[Original readme](https://github.com/defold/extension-lmprof/blob/master/ORIGINAL_README.md)

## Installation
To use this library in your Defold project, add the needed version URL to your `game.project` dependencies from [Releases](https://github.com/defold/extension-lmprof/releases)

<img width="401" alt="image" src="https://user-images.githubusercontent.com/2209596/202223571-c77f0304-5202-4314-869d-7a90bbeec5ec.png">


## Example

```lua
lmprof.set_option("gc_count", true)
lmprof.set_option("mismatch", true)
lmprof.set_option("micro", true)
lmprof.set_option("compress_graph", true)
lmprof.set_option("compress", true)
lmprof.set_option("threshold", 100) -- 1000 microseconds = 1 ms
_G.profilerlmprof = lmprof.create("instrument", "memory", "trace"):calibrate()

_G.profilerlmprof:start()
_G.profilerlmprof:begin_frame()

do_something_mem_heavy()

if not _G.profilerlmprof:stop("mem.json") then
	error("Failure!")
end
print("Result of memory profiling has saved on disk.")

local report_postprocess = require("lmprof.scripts.report_postprocess")
report_postprocess.filter_out_lines_with(fullpath, "chrome.json", {
    -- "%? %[C%]", -- remove "? [C]"
    -- "%(for generator%) %[C%]", -- remove "(for generator) [C]"
    -- "self %[C%]" -- remove "self [C]"
    "%[C%]" -- remove all "[C]" callse
})
os.exit()



function update(self, dt)
    if _G.profilerlmprof:get_state("running") then
        _G.profilerlmprof:end_frame()
        _G.profilerlmprof:begin_frame()
    end
end
```

## Viewer

* Open Google Chrome
* Instruments -> Preformance
* turn on `Memory` checkbox:

<img width="1125" alt="image" src="https://user-images.githubusercontent.com/2209596/222743207-e987d7df-020e-43da-8bf3-d2c49e6b84ce.png">

* Click `Load profile...` and choose your `json`

<img width="741" alt="image" src="https://user-images.githubusercontent.com/2209596/222743828-cc3d8b80-a64c-4e35-ac5d-2bc8a1d4f2ec.png">

<img width="1123" alt="image" src="https://user-images.githubusercontent.com/2209596/222744110-456458bc-d644-4972-8831-a4c35133bda4.png">

---
If you have any issues, questions or suggestions please [create an issue](https://github.com/defold/extension-lmprof/issues)
