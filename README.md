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

* Open Google Chrome
* Open DevTools -> `Performance`
* turn on the `Memory` checkbox:

<img width="1125" alt="image" src="https://user-images.githubusercontent.com/2209596/222743207-e987d7df-020e-43da-8bf3-d2c49e6b84ce.png">

* Click `Load profile...` and choose your `json`

<img width="741" alt="image" src="https://user-images.githubusercontent.com/2209596/222743828-cc3d8b80-a64c-4e35-ac5d-2bc8a1d4f2ec.png">

<img width="1123" alt="image" src="https://user-images.githubusercontent.com/2209596/222744110-456458bc-d644-4972-8831-a4c35133bda4.png">

### Chrome Performance notes

Recent Chrome versions filter unknown event names in browser-shaped traces. lmprof exports a generic trace so custom Lua scopes are visible in the Performance flame chart.

After loading a trace, expand the main thread track and zoom into the recorded range. The top-level task may be shown as `RunTask` or `Long Task`; lmprof scopes are nested under it. Typical scopes include:

* `Main`
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

Many scopes are very short, often microseconds, so they may not show labels until you zoom in. Use the Performance panel search to find a scope name such as `draw [C]` or `update`.

When memory profiling is enabled, Chrome also shows `UpdateCounters` events and the memory graph. Seeing only `UpdateCounters` and `Long Task` usually means the trace was exported as a browser trace instead of a generic trace, or the view is zoomed out too far.

Also, use [stat.py](https://github.com/defold/extension-lmprof/blob/master/stat.py) to see calls statistics.

>stat.py my.json

---
If you have any issues, questions or suggestions please [create an issue](https://github.com/defold/extension-lmprof/issues)
