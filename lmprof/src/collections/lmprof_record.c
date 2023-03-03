/*
** $Id: lmprof_record.c $
** Activation Record API
** See Copyright Notice in lmprof_lib.h
*/
#define LUA_CORE

#include <string.h>

#include "../lmprof_conf.h"

#include "lmprof_record.h"

/* lua_State active function changed from int to struct in 5.2 */
#if LUA_VERSION_NUM > 501
  #define LUA_CALLINFO_NULL l_nullptr
#else
  #define LUA_CALLINFO_NULL 0
#endif

/*
** The hashing function used to generate (hopefully) unique identifiers for
** activation record fields.
**
@@ LMPROF_USE_STRHASH: Use Lua's luaS_hash implementation. Otherwise, use
**  jenkins-one-at-a-time.
*/
#if !defined(LMPROF_BUILTIN) || LUA_VERSION_NUM == 501
#if defined(LMPROF_USE_STRHASH)
#define LMPROF_NAME_HASH(S) lmprof_Shash((S), strlen((S)), 0)
static lu_addr lmprof_Shash(const char *str, size_t l, lu_addr seed) {
  lu_addr h = seed ^ l_pcast(lu_addr, l);
  for (; l > 0; l--)
    h ^= ((h << 5) + (h >> 2) + l_cast(unsigned char, str[l - 1]));
  return h;
}
#else
#define LMPROF_NAME_HASH(S) lmprof_jenkins((S), 0)
static lu_addr lmprof_jenkins(const char *str, lu_addr seed) {
  lu_addr hash = seed;
  for (; *str; ++str) {
    hash += l_cast(unsigned char, *str);
    hash += (hash << 10);
    hash ^= (hash >> 6);
  }

  hash += (hash << 3);
  hash ^= (hash >> 11);
  hash += (hash << 15);
  return hash;
}
#endif
#endif

#if LUA_VERSION_NUM == 501
/*
** Copies the element at index fromidx into the valid index toidx, replacing the
** value at that position. Values at other positions are not affected.
*/
static void lua_copy(lua_State *L, int from, int to) {
  int abs_to = lua_absindex(L, to);
  luaL_checkstack(L, 1, "lua_copy: not enough stack slots");
  lua_pushvalue(L, from);
  lua_replace(L, abs_to);
}
#endif

/*
** Search for 'objidx' in table at index -1. ('objidx' must be an absolute
** index). Returning 1 + string at top if it found a good name.
**
** SOURCE: lauxlib.c
*/
static int findfield(lua_State *L, int objidx, int level) {
  if (level == 0 || !lua_istable(L, -1))
    return 0; /* not found */

  lua_pushnil(L); /* start 'next' loop */
  while (lua_next(L, -2)) { /* for each pair in table */
    if (lua_type(L, -2) == LUA_TSTRING) { /* ignore non-string keys */
      if (lua_rawequal(L, objidx, -1)) { /* found object? */
        lua_pop(L, 1); /* remove value (but keep name) */
        return 1;
      }
      else if (findfield(L, objidx, level - 1)) { /* try recursively */
#if LUA_VERSION_NUM >= 504
        /* stack: lib_name, lib_table, field_name (top) */
        lua_pushliteral(L, "."); /* place '.' between the two names */
        lua_replace(L, -3); /* (in the slot occupied by table) */
        lua_concat(L, 3); /* lib_name.field_name */
        return 1;
#else
        lua_remove(L, -2); /* remove table (but keep name) */
        lua_pushliteral(L, ".");
        lua_insert(L, -2); /* place '.' between the two names */
        lua_concat(L, 3);
        return 1;
#endif
      }
    }
    lua_pop(L, 1); /* remove value */
  }
  return 0; /* not found */
}

/*
** Search for a name for a function in all loaded modules
**
** SOURCE: lauxlib.c
*/
static int pushglobalfuncname(lua_State *L, lua_Debug *ar) {
  const int top = lua_gettop(L);

  lua_getinfo(L, DEBUG_FUNCTION, ar); /* push function */
#if LUA_VERSION_NUM == 501
  lua_pushvalue(L, LUA_GLOBALSINDEX);
#else
  lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
#endif

  if (findfield(L, top + 1, 2)) {
    const char *name = lua_tostring(L, -1);
    /* @TODO: If LUA_GNAME ever changes, strncmp needs to change */
    if (strncmp(name, LUA_GNAME ".", 3) == 0) { /* name start with '_G.'? */
      lua_pushstring(L, name + 3); /* push name without prefix */
      lua_remove(L, -2); /* remove original name */
    }
    lua_copy(L, -1, top + 1); /* copy name to proper place */
    lua_settop(L, top + 1); /* remove table "loaded" an name copy */
    return 1;
  }
  else {
    lua_settop(L, top); /* remove function and global table */
    return 0;
  }
}

/* SOURCE: lauxlib.c */
static const char *getfuncinfo(lua_State *L, lua_Debug *ar, size_t *len) {
  const int top = lua_gettop(L);
  if (ar->name == l_nullptr)
    lua_getinfo(L, DEBUG_NAME, ar);

  /*
  ** It is assumed the passed lua_Debug is already populated. Avoid the spurious
  ** function-call overhead.
  */
  if (*ar->namewhat != '\0') { /* is there a name? */
    lua_pushstring(L, ar->name);
    if (ar->linedefined > 0)
      lua_pushfstring(L, " (%s:%d)", ar->short_src, ar->linedefined);
    else
      lua_pushfstring(L, " %s", ar->short_src);
  }
  else if (*ar->what == 'm') { /* main? */
    lua_pushliteral(L, LMPROF_RECORD_NAME_MAIN);
    lua_pushfstring(L, " (%s)", ar->short_src);
  }
  else if (*ar->what == 'C') {
    if (pushglobalfuncname(L, ar)) {
      lua_pushstring(L, lua_tostring(L, -1));
      lua_remove(L, -2); /* remove name */
    }
    else {
      lua_pushliteral(L, LMPROF_RECORD_NAME_UNKNOWN);
    }
    lua_pushfstring(L, " %s", ar->short_src);
  }
  else {
    lua_pushliteral(L, LMPROF_RECORD_NAME_UNKNOWN);
    lua_pushfstring(L, " (%s:%d:%i)", ar->short_src, ar->linedefined, ar->currentline);
  }

  lua_concat(L, lua_gettop(L) - top);
  return lua_tolstring(L, -1, len); /* do not pop the string - only after use */
}

static char *_recordstrdup(lmprof_Alloc *alloc, const char *name, size_t len) {
  if (name != l_nullptr) {
    const size_t nameLen = (len == 0) ? strlen(name) : len;
    char *result = lmprof_strdup(alloc, name, nameLen);
    return lmprof_record_sanitize(result, nameLen);
  }
  return l_nullptr;
}

/* SOURCE: lauxlib.c */
LUA_API void lua_pushfuncname(lua_State *L, lua_Debug *ar) {
  if (pushglobalfuncname(L, ar)) { /* try first a global name */
    lua_pushfstring(L, "function '%s'", lua_tostring(L, -1));
    lua_remove(L, -2); /* remove name */
  }
  else if (*ar->namewhat != '\0') /* is there a name from code? */
    lua_pushfstring(L, "%s '%s'", ar->namewhat, ar->name); /* use it */
  else if (*ar->what == 'm') /* main? */
    lua_pushliteral(L, LMPROF_RECORD_NAME_MAIN);
  else if (*ar->what != 'C') /* for Lua functions, use <file:line> */
    lua_pushfstring(L, "function <%s:%d>", ar->short_src, ar->linedefined);
  else /* nothing left... */
    lua_pushliteral(L, LMPROF_RECORD_NAME_UNKNOWN);
}

/* SOURCE: lauxlib.c */
LUA_API int lua_lastlevel(lua_State *L) {
  lua_Debug debug;
  int li = 1, le = 1;
  while (lua_getstack(L, le, &debug)) { /* find an upper bound */
    li = le;
    le *= 2;
  }

  while (li < le) { /* do a binary search */
    int m = (li + le) / 2;
    if (lua_getstack(L, m, &debug))
      li = m + 1;
    else
      le = m;
  }
  return le - 1;
}

/* SOURCE: lcorolib.auxstatus */
LUA_API int lua_auxstatus(lua_State *co) {
  switch (lua_status(co)) {
    case LUA_YIELD:
      return CO_STATUS_YIELD;
    case LUA_OK: {
      lua_Debug ar;
      if (lua_getstack(co, 0, &ar)) /* does it have frames? */
        return CO_STATUS_NORM; /* it is running */
      else if (lua_gettop(co) == 0)
        return CO_STATUS_DEAD;
      else /* initial state */
        return CO_STATUS_YIELD;
    }
    default: { /* some error occurred */
      return CO_STATUS_DEAD;
    }
  }
}

/*
** {==================================================================
** FunctionInfo
** ===================================================================
*/

/* Helper function to zero-out a lua_Debug struct. */
static void luadebug_clear(lua_Debug *debug) {
  debug->event = 0;
  debug->i_ci = LUA_CALLINFO_NULL;
  debug->name = l_nullptr;
  debug->namewhat = l_nullptr;
  debug->what = l_nullptr;
  debug->source = l_nullptr;
  debug->currentline = 0;
  debug->linedefined = 0;
  debug->lastlinedefined = 0;
  debug->nups = 0;
#if LUA_VERSION_NUM >= 502
  debug->nparams = 0;
  debug->isvararg = 0;
  debug->istailcall = 0;
#endif
#if LUA_VERSION_NUM >= 504
  debug->srclen = 0;
  debug->ftransfer = 0;
  debug->ntransfer = 0;
#endif
}

/* Cleanup lmprof_FunctionInfo allocation function. */
static int funcinfo_finalize(lua_State *L) {
  lmprof_Record *record = l_pcast(lmprof_Record *, luaL_checkudata(L, 1, LMPROF_RECORD_METATABLE));
  if (record != l_nullptr) {
    lmprof_Alloc l_alloc;
    l_alloc.f = lua_getallocf(L, &l_alloc.ud);
    lmprof_record_clear(&l_alloc, record);
  }
  return 0;
}

LUA_API void lmprof_record_initialize(lua_State *L) {
  static const luaL_Reg metameth[] = {
    { "__gc", funcinfo_finalize },
    { "__close", funcinfo_finalize },
    { l_nullptr, l_nullptr }
  };

  if (luaL_newmetatable(L, LMPROF_RECORD_METATABLE)) {
#if LUA_VERSION_NUM == 501
    luaL_register(L, l_nullptr, metameth);
#else
    luaL_setfuncs(L, metameth, 0);
#endif
  }
  lua_pop(L, 1); /* pop metatable */
}

LUA_API lmprof_Record *lmprof_record_new(lua_State *L) {
  lmprof_Record *record = l_pcast(lmprof_Record *, lmprof_newuserdata(L, sizeof(lmprof_Record)));
  if (record != l_nullptr) { /* [..., f_table, function, userdata] */
    memset(l_pcast(void *, record), 0, sizeof(lmprof_Record));

    BITFIELD_SET(record->info.event, LMPROF_RECORD_USERDATA);
#if LUA_VERSION_NUM == 501
    luaL_getmetatable(L, LMPROF_RECORD_METATABLE);
    lua_setmetatable(L, -2);
#else
    luaL_setmetatable(L, LMPROF_RECORD_METATABLE);
#endif
  }
  return record;
}

LUA_API void lmprof_record_clear(lmprof_Alloc *alloc, lmprof_Record *record) {
  if (record->info.name != l_nullptr)
    lmprof_strdup_free(alloc, record->info.name, 0);
  if (record->info.source) {
#if LUA_VERSION_NUM >= 504
    const size_t srclen = record->info.srclen;
#else
    const size_t srclen = strlen(record->info.source);
#endif
    lmprof_strdup_free(alloc, record->info.source, srclen);
  }

  record->info.name = l_nullptr;
  record->info.source = l_nullptr;
  if (record->graph.line_freq != l_nullptr) {
    const int length = record->graph.line_freq_size;
    lmprof_free(alloc, l_pcast(void *, record->graph.line_freq), length * sizeof(size_t));
    record->graph.line_freq = l_nullptr;
    record->graph.line_freq_size = 0;
  }

  luadebug_clear(&record->info);
}

/* @NOTE: See LMPROF_BUILTIN definition in lmprof.h */
#if defined(LMPROF_BUILTIN)
LUA_EXTERN_BEGIN
  #include <lobject.h>
  #include <lstate.h>
LUA_EXTERN_END

  /* Simplified callinfo to base object conversion */
  #if LUA_VERSION_NUM == 501
    #define ttypetag ttype
    #define LUA_CALLINFO(L, i_ci) ((L)->base_ci + (i_ci))
  #elif LUA_VERSION_NUM == 502
    #define ttypetag ttypenv
    #define LUA_CALLINFO(L, i_ci) (i_ci)
  #elif LUA_VERSION_NUM == 503
    #define ttypetag ttype
    #define LUA_CALLINFO(L, i_ci) (i_ci)
  #elif LUA_VERSION_NUM == 504
    #define LUA_CALLINFO(L, i_ci) (i_ci)
  #endif

  /* Variant 'tags' introduced Lua 5.4, backport those macros. */
  #if LUA_VERSION_NUM >= 501 && LUA_VERSION_NUM <= 503
    #define s2v(o) ((o))
    #define LUA_VLCF LUA_TLCF
    #define LUA_VCCL LUA_TCCL
    #define LUA_VLCL LUA_TLCL
    #if LUA_VERSION_NUM == 501
      #define LUA_TLCL (LUA_TFUNCTION | (0 << 4)) /* Lua closure */
      #define LUA_TLCF (LUA_TFUNCTION | (1 << 4)) /* light C function */
      #define LUA_TCCL (LUA_TFUNCTION | (2 << 4)) /* C closure */
    #endif
  #endif

/*
** This approach uses the 'CClosure' and 'LClosure' definitions to generate
** unique identifiers for an activation record.
*/
LUA_API lu_addr lmprof_record_id(lua_State *L, lua_Debug *ar, int gc_disabled, lua_CFunction *result) {
  lu_addr function = 0;
  if (ar->i_ci != LUA_CALLINFO_NULL) {
    const TValue *o = s2v(LUA_CALLINFO(L, ar->i_ci)->func);
    lua_assert(ttisfunction(o));

    lua_getinfo(L, DEBUG_IMMUTABLE_NO_NAME, ar);
    ar->namewhat = "";
    ar->name = l_nullptr;

  #if LUA_VERSION_NUM == 501
    if (clvalue(o)->c.isC) {
      function = l_pcast(lu_addr, clvalue(o)->c.f);
      if (result != l_nullptr)
        *result = clvalue(o)->c.f;
    }
    else {
      function = l_pcast(lu_addr, clvalue(o)->l.p);
    }
  #else
    switch (ttypetag(o)) {
      case LUA_VLCF:
        function = l_pcast(lu_addr, fvalue(o));
        if (result != l_nullptr)
          *result = fvalue(o);
        break;
      case LUA_VCCL:
        function = l_pcast(lu_addr, clCvalue(o)->f);
        break;
      case LUA_VLCL:
        function = l_pcast(lu_addr, clLvalue(o)->p);
        break;
      default:
        function = iscollectable(o) ? l_pcast(lu_addr, gcvalue(o)) : LMPROF_RECORD_ID_UNKNOWN;
        break;
    }
  #endif
  }
  else {
  #if LUA_VERSION_NUM == 501 /* info_tailcall */
    lua_getinfo(L, DEBUG_IMMUTABLE, ar);
    function = LMPROF_NAME_HASH(ar->source);
  #endif
  }

  UNUSED(gc_disabled);
  return function;
}
#else
/*
** This approach uses lua_getinfo to generate a unique identifier for a given
** activation record.
*/
LUA_API lu_addr lmprof_record_id(lua_State *L, lua_Debug *ar, int gc_disabled, lua_CFunction *result) {
  lu_addr hash = 0, function = 0;
  lua_CFunction cfunction = l_nullptr;
  if (ar->i_ci == LUA_CALLINFO_NULL) {
  #if LUA_VERSION_NUM == 501 /* info_tailcall */
    lua_getinfo(L, DEBUG_FUNCTION DEBUG_IMMUTABLE, ar); /* [..., function] */
    return LMPROF_NAME_HASH(ar->source);
  #else
    return 0;
  #endif
  }

  lua_getinfo(L, DEBUG_FUNCTION DEBUG_IMMUTABLE_NO_NAME, ar); /* [..., function] */
  ar->namewhat = "";
  ar->name = l_nullptr;

  cfunction = lua_tocfunction(L, -1);
  if (cfunction != l_nullptr) {
    function = l_pcast(lu_addr, cfunction);
    if (result != l_nullptr)
      *result = cfunction;
  }
  else if (gc_disabled)
    function = l_pcast(lu_addr, lua_topointer(L, -1));
  else
    function = 0xDEAD;

  lua_pop(L, 1);
  if (ar->what == l_nullptr || *(ar->what) == 'C')
    hash = function;
  else if (*(ar->what) == 'm') { /* main? */
    /*
    ** hash = LMPROF_NAME_HASH(LUA_RECORD_MAIN);
    ** hash = hash ^ LMPROF_NAME_HASH(ar.short_src);
    */
    hash = LMPROF_RECORD_ID_MAIN;
  }
  /*
  ** When using ar->source ensure the string is a 'literal' or a filename.
  ** Otherwise, any 'loaded' function will have its source code hashed for each
  ** function identifier.
  */
  else if (ar->source != l_nullptr && (*(ar->source) == '=' || *(ar->source) == '@')) {
    hash = hash ^ LMPROF_NAME_HASH(ar->source);
    if (ar->linedefined > 0)
      hash += ar->linedefined;
  }
  /*
  ** Otherwise, use the hash of 'short_src' that is enhanced by other fields to
  ** produce more reliable function identifiers.
  */
  else {
    const char *name = LMPROF_RECORD_NAME_UNKNOWN;
    lua_getinfo(L, DEBUG_NAME, ar);
    if (*(ar->what) != '\0') /* is there a name? */
      name = ar->name == l_nullptr ? ar->what : ar->name;

    hash = LMPROF_NAME_HASH(name);
    hash = LMPROF_NAME_HASH(ar->short_src);
    if (ar->linedefined > 0)
      hash += ar->linedefined;
  }

  return hash;
}
#endif

LUA_API void lmprof_record_function(lua_State *L, lua_Debug *ar, lu_addr fid) {
  if (ar == l_nullptr)
    lua_pushinteger(L, l_cast(lua_Integer, fid));
  else if (!lua_getinfo(L, DEBUG_FUNCTION, ar))
    luaL_error(L, "Could not fetch function information");
}

LUA_API void lmprof_record_clear_graph_statistics(lmprof_Record *record) {
  record->graph.count = 0;
  unit_clear(&record->graph.node);
  unit_clear(&record->graph.path);
}

LUA_API void lmprof_record_populate(lua_State *L, lmprof_Alloc *alloc, lua_Debug *ar, lmprof_FunctionInfo *info) {
  lua_Debug debug = LMPROF_ZERO_STRUCT;
  debug.i_ci = ar->i_ci;
  if (lua_getinfo(L, DEBUG_IMMUTABLE DEBUG_FUNCTION, &debug)) { /* [ ..., function] */
    const char *prevName = info->name;
    const char *prevSource = info->source;
#if LUA_VERSION_NUM >= 504
    const size_t prevsrclen = info->srclen;
#else
    const size_t prevsrclen = (prevSource == l_nullptr) ? 0 : strlen(prevSource);
#endif

    luadebug_clear(info);
    if (lua_iscfunction(L, -1)) {
      BITFIELD_SET(info->event, LMPROF_RECORD_CCLOSURE);
    }

    /*
    ** copy the Lua_Debug string names, ensuring those char*'s are not garbage
    ** collected and deallocated during the profiling.
    */
    if (!BITFIELD_TEST(info->event, LMPROF_RECORD_IGNORED)) {
      const int flags = info->event;
      int updatesource = prevSource == l_nullptr;

      *info = debug;
      info->event = flags;
      info->name = prevName;
      info->source = prevSource;
#if LUA_VERSION_NUM >= 504
      info->srclen = prevsrclen;
#endif
      if (prevName == l_nullptr && debug.name != l_nullptr) {
        info->name = _recordstrdup(alloc, debug.name, 0);
        updatesource = 1;
      }

      if (updatesource || prevSource == l_nullptr) {
        size_t sourceLen = 0;
        const char *source = getfuncinfo(L, &debug, &sourceLen); /* [..., function, record_string] */
        if (prevSource != l_nullptr) {
          lmprof_strdup_free(alloc, info->source, prevsrclen);
          info->source = l_nullptr;
        }

        info->source = _recordstrdup(alloc, source, sourceLen);
#if LUA_VERSION_NUM >= 504
        info->srclen = sourceLen;
#endif
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }
  else {
    LMPROF_LOG("Invalid getinfo flags: %s\n", DEBUG_IMMUTABLE DEBUG_FUNCTION);
  }
}

LUA_API void lmprof_record_update(lua_State *L, lmprof_Alloc *alloc, lua_Debug *ar, lu_addr f_id, lmprof_FunctionInfo *info) {
  static const char *const reserved_identifiers[] = {
    LMPROF_RECORD_NAME_ROOT,
    LMPROF_RECORD_NAME_MAIN,
    LMPROF_RECORD_NAME_GC,
    LMPROF_RECORD_NAME_UNKNOWN,
    l_nullptr
  };

  if (!LMPROF_RECORD_HAS_NAME(info)) {
    if (ar == l_nullptr) { /* Ensure "synthetic" activation record is initialized */
      const char *name = reserved_identifiers[l_cast(int, f_id)];
      const size_t len = strlen(name);

      luadebug_clear(info);
      info->event = 0;
      info->what = "C";
      info->name = lmprof_strdup(alloc, name, len);
      info->source = lmprof_strdup(alloc, name, len);
#if LUA_VERSION_NUM >= 504
      info->srclen = len;
#endif
      info->i_ci = LUA_CALLINFO_NULL;
      if (f_id == LMPROF_RECORD_ID_ROOT)
        BITFIELD_SET(info->event, LMPROF_RECORD_ROOT);
    }
    else {
      lmprof_record_populate(L, alloc, ar, info);
    }
  }
}

LUA_API char *lmprof_record_sanitize(char *source, size_t len) {
  size_t i;
  for (i = 0; i < len; ++i) {
    if (source[i] == '-' && source[i + 1] == '-')
      source[i] = ' '; /* In the worst case (i + 1) == '\0' */
    /*
    ** @HACK: Avoid escape sequences when loading these strings into Lua.
    ** @TODO: Better. Remove function and integrate with 'lmprof_strdup', i.e.,
    **    count number of 'escapable' characters and include it in the length
    **    of the duplication.
    */
    else {
      switch (source[i]) {
        case '"': source[i] = '\''; break;
        case '\\': source[i] = '/'; break;
        default:
          break;
      }
    }
  }
  return source;
}

/* }================================================================== */
