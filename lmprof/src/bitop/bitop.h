#ifndef BITOP_H
#define BITOP_H

extern "C" {
	#include <dmsdk/lua/lua.h>
}

#define LUA_BITOP_VERSION   "1.0.2"

LUALIB_API int luaopen_bit(lua_State *L);

#endif /* BITOP_H */