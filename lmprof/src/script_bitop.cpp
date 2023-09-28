// Copyright 2020-2023 The Defold Foundation
// Copyright 2014-2020 King
// Copyright 2009-2014 Ragnar Svensson, Christian Murray
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
// 
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
// 
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "bitop/bitop.h"

namespace dmScript
{
    void InitializeBitop(lua_State* L)
    {
        int top = lua_gettop(L);

        luaopen_bit(L);

        int stack = lua_gettop(L);

        // Above call leaves a table and a number on the stack which will not
        // be needed for anything.
        lua_pop(L, stack - top);
    }
}
