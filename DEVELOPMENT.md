* Environment should be configured for building Defold game engine.
* Make a fork from https://github.com/JCash/LuaJIT/tree/v2.1-defold
(branch `v2.1-defold`)
* Apply the following fix to avoid Tail function calls: https://github.com/AGulev/LuaJIT/commit/2d4102f83f7225cdac200d79b76d724fa34c07e8
* Open https://github.com/defold/defold/blob/dev/share/ext/luajit/make-patch.sh and replace 

    * `CHANGED_REPO` with your repo
    * `BRANCH_B` with your branch name
* Run `make-patch.sh` 
* Build LuaJit libs using a regular instructions https://github.com/defold/defold/blob/dev/share/ext/luajit/README.md