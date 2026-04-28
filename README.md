# lmprof native extension for Defold Game Engine

[Original readme](https://github.com/defold/extension-lmprof/blob/master/ORIGINAL_README.md)

Vanilla Lua 5.1 version is available here: https://github.com/defold/extension-lmprof/tree/lua-5.1

## Installation
To use this library in your Defold project, add the needed version URL to your `game.project` dependencies from [Releases](https://github.com/defold/extension-lmprof/releases)

<img width="401" alt="image" src="https://user-images.githubusercontent.com/2209596/202223571-c77f0304-5202-4314-869d-7a90bbeec5ec.png">


## Example

Add `lmprof.script` component to your main collection.
Specify properties:
<img width="884" alt="image" src="https://github.com/defold/extension-lmprof/assets/2209596/73317193-b21a-4db8-b30f-111fdfb635f2">


## Viewer

lmprof saves Perfetto binary traces by default. The helper script writes `mem.perfetto-trace` to Defold's app save directory and prints the full path in the console. Custom relative filenames are saved there too; absolute filenames are used as-is. Pass an empty second argument, for example `lmprof_helper.stop(nil, "")`, if you explicitly want paths to stay relative to the process working directory. Use a filename ending in `.tracy` to write a native Tracy capture instead.

To compare exporters from the same profiling session, pass multiple filenames:

```lua
lmprof_helper.stop({ "mem.perfetto-trace", "mem.json", "mem.tracy" })
```

The command-line helper also accepts multiple outputs:

```sh
lua lmprof/scripts/script.lua --input=example.lua --trace --memory --outputs=mem.perfetto-trace,mem.json,mem.tracy
```

* Open [ui.perfetto.dev](https://ui.perfetto.dev)
* Choose `Open trace file`
* Load the generated `.perfetto-trace` file

After loading a trace, expand the main thread track and zoom into the recorded range. The top-level task is shown as `RunTask`; lmprof scopes are nested under it. Typical scopes include:

* `update (lmprof/scripts/lmprof_helper.lua:49)`
* `get_state [C]`
* `begin_frame [C]`
* `end_frame [C]`
* `? (builtins/render/default.render_script:194:%i)`
* `set_camera_world (builtins/render/default.render_script:140)`
* `set_view [C]`
* `set_projection [C]`
* `set_viewport [C]`
* `enable_state [C]`
* `disable_state [C]`
* `draw [C]`

Many scopes are very short, often microseconds, so they may not show labels until you zoom in. Use search to find a scope name such as `draw [C]` or `update`.

When memory profiling is enabled, the trace includes an `UpdateCounters LuaMemory` counter track.

### Tracy output

If you prefer the Tracy profiler, pass an output filename ending in `.tracy`. The native capture includes lmprof CPU zones, frame markers, thread names, and the `LuaMemory` plot when memory profiling is enabled.

### JSON output

If you need Chrome Trace Event JSON for older tools, pass an output filename ending in `.json`. JSON traces can be opened in Chrome DevTools `Performance` or processed by [stat.py](https://github.com/defold/extension-lmprof/blob/master/stat.py).

>stat.py my.json

Perfetto's `traceconv` converts Perfetto protobuf traces to Chrome JSON, but it does not convert Chrome JSON back to a TrackEvent protobuf trace. If you need binary Perfetto output for comparison, write `.perfetto-trace` and `.json` from the same lmprof run.

---
If you have any issues, questions or suggestions please [create an issue](https://github.com/defold/extension-lmprof/issues)
