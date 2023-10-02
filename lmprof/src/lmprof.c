/*
** $Id: lmprof.c $
** Internal profiling definitions/helper-functions.
** See Copyright Notice in lmprof_lib.h
**
** @NOTE: This file could be merged with lmprof_lib.c. To keep logic organized:
**  - lmprof_lib.c deals with library functions and the interfaces supplied to
**      lua_sethook.
**  - lmprof.c supplements lmprof_lib.c and focuses on the profiler state,
**      configuration, and persisting data, i.e., throwing things into the
**      registry table for the duration of a profile.
*/


#include <string.h>

#include "lmprof_conf.h"

#include "collections/lmprof_record.h"
#include "collections/lmprof_hash.h"

#include "lmprof_state.h"
#include "lmprof.h"
#include "lmprof_lib.h"

/* int type used by Lua for lua_rawgeti/lua_seti operations. */
#if LUA_VERSION_NUM >= 503
typedef lua_Integer lmprof_rawgeti_t;
#else
typedef int lmprof_rawgeti_t;
#endif

#if LUA_VERSION_NUM < 502
/*
** Ensure that the value t[name], where t is the value at index idx, is a table,
** and pushes that table onto the stack. Returns true if it finds a previous
** table there and false if it creates a new table.
*/
static int luaL_getsubtable(lua_State *L, int idx, const char *fname) {
  lua_getfield(L, idx, fname);
  if (lua_istable(L, -1)) {
    return 1; /* table already there */
  }
  else {
    lua_pop(L, 1); /* remove previous result */
    idx = lua_absindex(L, idx);
    lua_newtable(L);
    lua_pushvalue(L, -1); /* copy to be left at top */
    lua_setfield(L, idx, fname); /* assign new table to field */
    return 0; /* false, because did not find table there */
  }
}
#endif

/* Get a subtable within the registry table */
static void lmprof_getlibtable(lua_State *L, lmprof_rawgeti_t name) {
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LMPROF); /* [..., library_table] */
  lua_rawgeti(L, -1, name); /* [..., library_table, subtable] */
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1); /* [..., library_table] */
    lua_newtable(L); /* [..., library_table, newtable] */
    lua_pushvalue(L, -1); /* [..., library_table, newtable, newtable] */
    lua_rawseti(L, -3, name); /* [..., library_table, newtable] */

    /* initialize it... */
  }
  lua_remove(L, -2); /* [..., subtable] */
}

/* Fetch a lua_Int from the registry table */
static lua_Integer lmprof_getlibi(lua_State *L, lmprof_rawgeti_t key, lua_Integer opt) {
  lua_Integer result;
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LMPROF); /* [..., library_table] */
  lua_rawgeti(L, -1, key); /* [..., library_table, value] */
  result = luaL_optinteger(L, -1, opt);
  lua_pop(L, 2);
  return result;
}

/* Push a integer into the registry table at the specified key */
static void lmprof_setlibi(lua_State *L, lmprof_rawgeti_t key, lua_Integer value) {
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LMPROF); /* [..., library_table] */
  lua_pushinteger(L, value);
  lua_rawseti(L, -2, key);
  lua_pop(L, 1);
}

/* Push a string into the registry table at the specified key */
static void lmprof_setlibs(lua_State *L, lmprof_rawgeti_t key, const char *value) {
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LMPROF); /* [..., library_table] */
  lua_pushstring(L, value != l_nullptr ? value : "");
  lua_rawseti(L, -2, key);
  lua_pop(L, 1);
}

/* Fetch a field from the registry table*/
static void lmprof_getlibfield(lua_State *L, lmprof_rawgeti_t key) {
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LMPROF); /* [..., library_table] */
  lua_rawgeti(L, -1, key); /* [..., library_table, value] */
  lua_remove(L, -2); /* [..., value] */
}

/* Set a field within the registry table */
static void lmprof_setlibfield(lua_State *L, lmprof_rawgeti_t key) {
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LMPROF); /* [..., value, library_table] */
  lua_pushvalue(L, -2); /* [..., value, library_table, value] */
  lua_rawseti(L, -2, key); /* [..., value, library_table] */
  lua_pop(L, 2);
}

/*
** {==================================================================
**  Allocator Intermediate
** ===================================================================
*/

LUA_API void *lmprof_malloc(lmprof_Alloc *alloc, size_t size) {
  return alloc->f(alloc->ud, l_nullptr, 0, size);
}

LUA_API void *lmprof_realloc(lmprof_Alloc *alloc, void *p, size_t osize, size_t nsize) {
  return alloc->f(alloc->ud, p, osize, nsize);
}

LUA_API void *lmprof_free(lmprof_Alloc *alloc, void *p, size_t size) {
  return alloc->f(alloc->ud, p, size, 0);
}

LUA_API char *lmprof_strdup(lmprof_Alloc *alloc, const char *source, size_t len) {
  char *str = l_nullptr;

  len = (len == 0) ? strlen(source) : len;
  if ((str = l_pcast(char *, lmprof_malloc(alloc, len + 1))) != l_nullptr) {
    memcpy(str, source, len);
    str[len] = '\0';
  }

  return str;
}

LUA_API char *lmprof_strdup_free(lmprof_Alloc *alloc, const char *source, size_t len) {
  len = (len == 0) ? strlen(source) : len;
  lmprof_free(alloc, (void *)source, len + 1);
  return l_nullptr;
}

/* }================================================================== */

/*
** {==================================================================
** Clock
** ===================================================================
*/

#define LUA_SYS_CLOCK /* os_nanotime enabled */
#define LUA_SYS_RDTSC /* os_rdtsc & os_rdtscp enabled */
#if defined(LUA_USE_POSIX)
  #include <unistd.h>
#endif

#if defined(__cplusplus) && __cplusplus >= 201103L
  #include <chrono>
/* https://docs.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps */
#elif defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
static LARGE_INTEGER _winQuery;
#elif defined(__APPLE__)
  #include <sys/time.h>
#elif defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0
  #ifdef _POSIX_MONOTONIC_CLOCK
    #define HAVE_CLOCK_GETTIME
    #include <time.h>
  #else
    #include <sys/time.h>
    #warning "A nanosecond resolution clock is not available; falling back to gettimeofday()..."
  #endif
#else
  #error "A nanosecond resolution clock is not available."
#endif


#if defined(_MSC_VER) && defined(_M_X64)
  #include <intrin.h>
#elif defined(__GNUC__) && defined(__has_include) && __has_include(<x86intrin.h>) && !defined(__aarch64__)
  #include <x86intrin.h>
#else
  #undef LUA_SYS_RDTSC
#endif

LUA_API void lmprof_clock_init(void) {
#if defined(__cplusplus) && __cplusplus >= 201103L
#elif defined(_WIN32)
  QueryPerformanceFrequency(&_winQuery);
#endif
}

LUA_API lu_time lmprof_clock_sample(void) {
#if defined(__cplusplus) && __cplusplus >= 201103L
  auto since_epoch = std::chrono::high_resolution_clock::now().time_since_epoch();
  #if LUA_32BITS
  auto micros = std::chrono::duration_cast<std::chrono::microseconds>(since_epoch);
  return l_cast(lu_time, micros.count());
  #else
  auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch);
  return l_cast(lu_time, nanos.count());
  #endif
#elif defined(_WIN32)
  LARGE_INTEGER now;
  /* Technically may return false. If so, expect explosions elsewhere. */
  QueryPerformanceCounter(&now);
  #if LUA_32BITS
  return l_cast(lu_time, (1000000L * now.QuadPart) / _winQuery.QuadPart);
  #else
  return l_cast(lu_time, (1000000000L * now.QuadPart) / _winQuery.QuadPart);
  #endif
#elif defined(HAVE_CLOCK_GETTIME)
  struct timespec spec;
  if (clock_gettime(CLOCK_MONOTONIC, &spec) == 0) {
  #if LUA_32BITS
    return l_cast(lu_time, (spec.tv_sec * 1000000L) + (spec.tv_nsec / 1000L));
  #else
    return l_cast(lu_time, (spec.tv_sec * 1000000000L) + spec.tv_nsec);
  #endif
  }
  return l_cast(lu_time, 0);
#else
  struct timeval spec;
  if (gettimeofday(&spec, NULL) >= 0) {
  #if LUA_32BITS
    return l_cast(lu_time, (spec.tv_sec * 1000000L) + spec.tv_usec);
  #else
    return l_cast(lu_time, (spec.tv_sec * 1000000000L) + (spec.tv_usec * 1000L));
  #endif
  }
  return l_cast(lu_time, 0);
#endif
}

LUA_API lu_time lmprof_clock_rdtsc(void) {
#if !defined(LUA_SYS_RDTSC)
  return l_cast(lu_time, 0);
#elif defined(LMPROF_RDTSCP)
  static uint64_t dummy;
  return l_cast(lu_time, __rdtscp(&dummy));
#else
  return l_cast(lu_time, __rdtsc());
#endif
}

/* }================================================================== */

/*
** {==================================================================
** State
** ===================================================================
*/

int lmprof_error(lua_State *L, lmprof_State *st, const char *fmt, ...) {
  va_list argp;
  BITFIELD_SET(st->state, LMPROF_STATE_ERROR | LMPROF_STATE_IGNORE_ALLOC);
  if (st->on_error) {
    st->on_error(L, st);
  }

  va_start(argp, fmt);
  luaL_where(L, 1);
  lua_pushvfstring(L, fmt, argp);
  va_end(argp);
  lua_concat(L, 2);
  return lua_error(L);
}

int lmprof_initialize_state(lua_State *L, lmprof_State *st, uint32_t mode, lmprof_Error error) {
  st->time = LUA_TIME;
  st->on_error = error;
  st->mode = mode;
  st->conf = 0;
  st->state = 0;

  st->hook.alloc.f = l_nullptr;
  st->hook.alloc.ud = l_nullptr;
  st->hook.yield = l_nullptr;
  st->hook.l_hook = l_nullptr;
  st->hook.flags = 0;
  st->hook.line_count = 0;

  st->thread.main = l_nullptr;
  st->thread.mainproc.pid = LMPROF_PROCESS_MAIN;
  st->thread.mainproc.tid = LMPROF_THREAD_OFFSET(0);
  st->thread.state = l_nullptr;
  st->thread.call_stack = l_nullptr;
  st->thread.stack_count = 0;
  st->thread.r.overhead = 0;
  st->thread.r.proc = st->thread.mainproc;
  unit_clear(&st->thread.r.s);

  /* Required changes in Lua to allow memory profiling in a single-threaded state */
  if (BITFIELD_TEST(st->mode, LMPROF_MODE_SINGLE_THREAD))
    BITFIELD_CLEAR(st->mode, LMPROF_MODE_MEMORY);

  st->i.mask_count = 0;
  st->i.instr_count = 0;
  st->i.hash_size = 0;
  st->i.calibration = 0;

  st->i.url = l_nullptr;
  st->i.name = l_nullptr;
  st->i.pageLimit = 0;
  st->i.counterFrequency = 0;
  st->i.event_threshold = 0;

  st->i.record_count = 0;
  st->i.hash = l_nullptr;
  if (BITFIELD_TEST(st->mode, LMPROF_CALLBACK_MASK)) {
    st->i.trace.arg = l_nullptr;
    st->i.trace.free = l_nullptr;
    st->i.trace.scope = l_nullptr;
    st->i.trace.sample = l_nullptr;
  }

  if (L != l_nullptr) { /* Populate globally defined values*/
    const char *str = l_nullptr;
    lua_State *mainthread = l_nullptr;

    luaL_checkstack(L, 4, __FUNCTION__);
#if defined(LMPROF_BUILTIN)
    mainthread = G(L)->mainthread;
#elif LUA_VERSION_NUM >= 502
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD); /* [..., MAINTHREAD] */
    if ((mainthread = lua_tothread(L, -1)) == l_nullptr)
      mainthread = L;

    lua_pop(L, 1);
#else
    mainthread = L;
#endif

    st->conf = l_cast(uint32_t, lmprof_getlibi(L, LMPROF_FLAGS, LMPROF_OPT_DEFAULT));
    st->hook.alloc.f = lua_getallocf(L, &st->hook.alloc.ud);

    st->thread.mainproc.pid = lmprof_getlibi(L, LMPROF_PROCESS, LMPROF_PROCESS_MAIN);
    st->thread.mainproc.tid = LMPROF_THREAD_OFFSET(0);
    if ((L == mainthread || luaL_verify_thread(mainthread)) && BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_LAYOUT_SPLIT))
      st->thread.mainproc.tid = lmprof_thread_identifier(mainthread);

    st->thread.r.proc = st->thread.mainproc;

    /* Attempt to copy a reference to coroutine.yield() to generate */
    lua_getglobal(L, "coroutine"); /* [..., coroutine] */
    if (lua_istable(L, -1)) {
      lua_pushliteral(L, "yield"); /* [..., coroutine, "yield"] */
      lua_gettable(L, -2); /* [..., debug, coroutine.yield] */
      if (lua_iscfunction(L, -1)) {
        st->hook.yield = lua_tocfunction(L, -1);
      }

      lua_pop(L, 2);
    }
    else {
      lua_pop(L, 1);
    }

    /* Initialize interface structures */
    st->i.pageLimit = lmprof_getlibi(L, LMPROF_PAGE_LIMIT, 0);
    st->i.counterFrequency = lmprof_getlibi(L, LMPROF_COUNTERS_FREQ, TRACE_EVENT_COUNTER_FREQ);
    st->i.hash_size = l_cast(size_t, lmprof_getlibi(L, LMPROF_HASHTABLE_SIZE, LMPROF_HASH_SIZE));
    st->i.event_threshold = l_cast(lu_time, lmprof_getlibi(L, LMPROF_THRESHOLD, TRACE_EVENT_DEFAULT_THRESHOLD));
    st->i.mask_count = l_cast(int, lmprof_getlibi(L, LMPROF_HOOK_COUNT, 0));
    st->i.calibration = 0;
    st->i.instr_count = 0;

    lmprof_getlibfield(L, LMPROF_URL); /* [..., url] */
    if (lua_type(L, -1) == LUA_TSTRING && (str = lua_tostring(L, -1)) != l_nullptr)
      st->i.url = lmprof_strdup(&st->hook.alloc, str, 0);

    lmprof_getlibfield(L, LMPROF_PROFILE_NAME); /* [..., url, name] */
    if (lua_type(L, -1) == LUA_TSTRING && (str = lua_tostring(L, -1)) != l_nullptr)
      st->i.name = lmprof_strdup(&st->hook.alloc, str, 0);

    lua_pop(L, 2);
    BITFIELD_SET(st->state, LMPROF_STATE_IGNORE_CALL);
  }

  return LUA_OK;
}

int lmprof_clear_state(lua_State *L, lmprof_State *st) {
  UNUSED(L);
  if (st->i.hash != l_nullptr) {
    lmprof_hash_destroy(&st->hook.alloc, st->i.hash);
    st->i.hash = l_nullptr;
  }

  /* The bits from 'lmprof_initialize_state' that still require reset */
  if (BITFIELD_TEST(st->state, LMPROF_STATE_PERSISTENT)) {
    st->thread.state = l_nullptr;
    st->thread.call_stack = l_nullptr;
    st->thread.stack_count = 0;
    st->thread.r.overhead = 0;
    st->thread.r.proc = st->thread.mainproc;
    unit_clear(&st->thread.r.s);
  }
  else { /* Deallocate any allocated pointers*/
    if (st->i.name != l_nullptr) {
      lmprof_strdup_free(&st->hook.alloc, st->i.name, 0);
      st->i.name = l_nullptr;
    }

    if (st->i.url != l_nullptr) {
      lmprof_strdup_free(&st->hook.alloc, st->i.url, 0);
      st->i.url = l_nullptr;
    }

    /* Reset all data to default state to prevent pointer leaks. */
    lmprof_initialize_state(l_nullptr, st, 0, l_nullptr);
  }

  return LUA_OK;
}

void lmprof_initialize_thread(lua_State *L, lmprof_State *st, lua_State *ignore) {
  if (ignore != L && luaL_verify_thread(L) && lua_gethook(L) != st->hook.l_hook) {
    /*
    ** Preallocate the thread stack if instrumentation is enabled. The thread
    ** id (proc.tid) must be temporarily updated. For 'sampling' individual
    ** call stacks should never be allocated.
    */
    const lua_Integer c_id = st->thread.r.proc.tid;
    if (BITFIELD_TEST(st->mode, LMPROF_MODE_INSTRUMENT)) {
      const lmprof_Stack *stack = lmprof_thread_stacktable_get(L, st);
      if (BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_LAYOUT_SPLIT))
        st->thread.r.proc.tid = stack->thread_identifier;
    }

    lua_sethook(L, st->hook.l_hook, st->hook.flags, st->hook.line_count);
    st->thread.r.proc.tid = c_id;
  }
}

void lmprof_clear_thread(lua_State *L, lmprof_State *st, lua_State *ignore) {
  if (ignore != L && lua_gethook(L) == st->hook.l_hook) {
    /* Remove the profile stack associated with the given lua_State. */
    luaL_checkstack(L, 5, __FUNCTION__);
    lmprof_getlibtable(L, LMPROF_TAB_THREAD_STACKS); /* [..., thread_stacks] */

    /*
    ** If the profiler stack is a light-userdata, manage that memory.
    **
    ** @NOTE Future-proofing for if/when allocated profiler stacks no longer
    **  become managed by Lua.
    */
    lua_pushthread(L); /* [..., thread_stacks, thread] */
    lua_rawget(L, -2); /* [..., thread_stacks, stack] */
    if (lua_islightuserdata(L, -1)) {
      lmprof_Stack *stack = l_pcast(lmprof_Stack *, lua_touserdata(L, -1));
      lmprof_stack_light_free(&st->hook.alloc, stack);
    }

    lua_pushthread(L); /* [..., thread_stacks, stack, thread] */
    lua_pushnil(L); /* [..., thread_stacks, stack, thread, nil] */
    lua_rawset(L, -4); /* [..., thread_stacks, stack] */
    lua_pop(L, 2);

    lua_sethook(L, l_nullptr, 0, 0);
  }
}

/* }================================================================== */

/*
** {==================================================================
** Singleton
** ===================================================================
*/

/*
** @TODO: Experiment with lua_rawgetp/lua_rawsetp to store the active profiling
**  state in the registry.
*/
#define REGISTRY_GET_SINGLETON(L) lua_getfield((L), LUA_REGISTRYINDEX, LMPROF_PROFILER_SINGLETON)
#define REGISTRY_SET_SINGLETON(L) lua_setfield((L), LUA_REGISTRYINDEX, LMPROF_PROFILER_SINGLETON)

/*
** Clear all threads and associated identifiers of 'dead' threads.
**
** 'state' - The active profiler state. If non-null perform an internal garbage
**  collection step: removing any dead threads and associated data from the
**  registry.
*/
static void lmprof_thread_info_gc(lua_State *L, lmprof_State *state);

/* Clear and deallocate all lua_State and profile stack associations. */
static void lmprof_thread_stacktable_free(lua_State *L, int idx);

/* Clear all lua_State and profile stack associations. */
static LUA_INLINE void lmprof_thread_stacktable_clear(lua_State *L) {
  lmprof_getlibtable(L, LMPROF_TAB_THREAD_STACKS); /* [..., thread_stacks] */
  lmprof_thread_stacktable_free(L, -1);
  lua_pop(L, 1);
}

/* "debug.sethook" override when the profiler state is active. */
static int sethook_error(lua_State *L) {
  return luaL_error(L, "Cannot debug.sethook when profiling!");
}

/*
** Hook debug.sethook() to, ideally, prevent overriding the Lua hook while in a
** profiled state; obviously a debug.sethook upvalue bypasses this.
*/
static void lmprof_hook_debug(lua_State *L, int reset) {
  luaL_checkstack(L, 5, __FUNCTION__);
  lua_getglobal(L, "debug"); /* [..., debug] */
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }

  lua_pushliteral(L, "sethook"); /* [..., debug, "sethook"] */
  if (reset) { /* reset initial debug.sethook function. */
    lmprof_getlibfield(L, LMPROF_DEBUG_HOOK); /* [..., debug, "sethook", previous_hook] */
    if (lua_isfunction(L, -1)) {
      lua_settable(L, -3); /* [..., debug] */
      lua_pop(L, 1);
    }
    else {
      lua_pop(L, 3);
    }

    /* clear cached debug.sethook in registry table. */
    lua_pushnil(L);
    lmprof_setlibfield(L, LMPROF_DEBUG_HOOK);
  }
  else { /* take debug.sethook and store it in the registry table. */
    lua_gettable(L, -2); /* [..., debug, debug.sethook] */
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LMPROF); /* [..., debug, debug.sethook, library_table] */

    /* Ensure a debug.sethook not already hooked */
    lua_rawgeti(L, -1, LMPROF_DEBUG_HOOK); /* [..., debug, debug.sethook, library_table, previous_hook] */
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1); /* [..., debug, debug.sethook, library_table] */
      lua_pushvalue(L, -2); /* [..., debug, debug.sethook, library_table, debug.sethook] */
      lua_rawseti(L, -2, LMPROF_DEBUG_HOOK); /* [..., debug, debug.sethook, library_table] */
      lua_pop(L, 2); /* [..., debug] */

      /* replace debug.sethook function. */
      lua_pushcfunction(L, (lua_CFunction)sethook_error);
      lua_setfield(L, -2, "sethook");
      lua_pop(L, 1); /* debug global */
    }
    else {
      LMPROF_LOG("Could not replace debug.sethook!\n");
      lua_pop(L, 4);
    }
  }
}

LUA_API lmprof_State *lmprof_singleton(lua_State *L) {
  lmprof_State *st = l_nullptr;
  REGISTRY_GET_SINGLETON(L);
  st = l_pcast(lmprof_State *, lua_touserdata(L, -1));

  lua_pop(L, 1);
  return st;
}

int lmprof_check_can_profile(lua_State *L) {
  if (lmprof_singleton(L) != l_nullptr)
    return luaL_error(L, "calling lmprof start function twice");
  else if (lua_gethook(L) != l_nullptr)
    return luaL_error(L, "cannot safely debug.sethook"); /* will be replacing debug.sethook, avoid it. */
  return 0;
}

int lmprof_register_singleton(lua_State *L, int idx) {
  if (lmprof_singleton(L) == l_nullptr) {
    lua_pushvalue(L, lua_absindex(L, idx));
    REGISTRY_SET_SINGLETON(L);

    lmprof_hook_debug(L, 0); /* Assume profiled state: cache current debug.hook */

    lmprof_thread_stacktable_clear(L);
    lmprof_thread_info_gc(L, l_nullptr);
    return 1;
  }
  return 0;
}

void lmprof_clear_singleton(lua_State *L) {
  luaL_checkstack(L, 4, __FUNCTION__);
  lua_pushnil(L);
  REGISTRY_SET_SINGLETON(L);

  lmprof_hook_debug(L, 1);

  lmprof_thread_stacktable_clear(L);
  lmprof_thread_info_gc(L, l_nullptr);
}

int lmprof_verify_singleton(lua_State *L, lmprof_State *st) {
  luaL_checkstack(L, 3, __FUNCTION__);
  if (lmprof_singleton(L) != st) /* Singleton has changed */
    return -1;

#if LUA_VERSION_NUM >= 502
  /* gc state was toggled, and inconsistent with profiler state. */
  if ((BITFIELD_TEST(st->state, LMPROF_STATE_GC_WAS_RUNNING) ? 1 : 0) != lua_gc(L, LUA_GCISRUNNING, 0))
    return -2;
#endif

  /* debug.sethook() has been overridden, rawset or the function was cached. */
  if (st->hook.l_hook != l_nullptr) {
    lua_getglobal(L, "debug"); /* [..., debug] */
    if (lua_istable(L, -1)) {
      lua_pushliteral(L, "sethook"); /* [..., debug, "sethook"] */
      lua_gettable(L, -2); /* [..., debug, debug.sethook] */
      if (lua_tocfunction(L, -1) != (lua_CFunction)sethook_error) {
        lua_pop(L, 2);
        return -4;
      }
      lua_pop(L, 2);
    }
    else {
      lua_pop(L, 1);
      return -3;
    }
  }

  return LUA_OK; /* everything is valid */
}

/* }================================================================== */

/*
** {==================================================================
**  Threading
** ===================================================================
*/

/*
@@ LMPROF_STACK_GC: Number of additionally profiled coroutines before invoking
**  an internal garbage collection step. This handles use-cases where *many*
**  short-lived coroutines are created during the duration of a profile.
**
** @TODO: Make runtime configurable.
*/
#if !defined(LMPROF_STACK_GC)
  #define LMPROF_STACK_GC 64
#endif

/*
** If the trace event API is enabled, generate a 'fake' event denoting that the
** profiler is collecting garbage.
*/
static void lmprof_trace_gc_event(lua_State *L, lmprof_State *st, int begin_gc) {
  if (BITFIELD_TEST(st->mode, LMPROF_CALLBACK_MASK)) {
    int lmprof_errno = LUA_OK;
    lmprof_StackInst *inst = l_nullptr;

    st->thread.r.s.time = LMPROF_TIME(st);
    if (begin_gc) {
      lmprof_Record *record = lmprof_fetch_record(L, st, l_nullptr, LMPROF_RECORD_ID_GC, LMPROF_RECORD_ID_ROOT, 0);

      inst = lmprof_stack_event_push(st->thread.call_stack, record, &st->thread.r, 0);
      if ((lmprof_errno = st->i.trace.scope(L, st, inst, 1)) != LUA_OK) {
        lmprof_error(L, st, "Error: %s", traceevent_strerror(lmprof_errno));
      }
    }
    else {
      inst = lmprof_stack_pop(st->thread.call_stack);
      inst->trace.call = st->thread.r;
      if ((lmprof_errno = st->i.trace.scope(L, st, inst, 0)) != LUA_OK) {
        lmprof_error(L, st, "Error: %s", traceevent_strerror(lmprof_errno));
      }
    }
  }
}

/*
** @TODO: This function is 'pure' profiler overhead; attempt to trim its
** execution time/complexity.
*/
lmprof_Stack *lmprof_thread_stacktable_get(lua_State *L, lmprof_State *st) {
  lmprof_Stack *stack = l_nullptr;
  lua_Integer thread_identifier;
  const char callback_api = BITFIELD_TEST(st->mode, LMPROF_CALLBACK_MASK) != 0;
  /* luaL_checkstack(L, 4, __FUNCTION__); */
#if defined(_DEBUG)
  if (!BITFIELD_TEST(st->mode, LMPROF_MODE_INSTRUMENT))
    LMPROF_LOG("Fetching stacktable when not instrumenting\n");
#endif

  lmprof_getlibtable(L, LMPROF_TAB_THREAD_STACKS); /* [..., thread_stacks] */
  lua_pushthread(L); /* [..., thread_stacks, thread] */
  lua_rawget(L, -2); /* [..., thread_stacks, stack] */
  if (lua_isuserdata(L, -1)) {
    stack = l_pcast(lmprof_Stack *, lua_touserdata(L, -1));
    if (stack != l_nullptr) {
      lua_pop(L, 2);
      return stack;
    }
  }

  lua_pop(L, 1); /* [..., thread_stacks] */

  /*
  ** Track the number of allocated profiler stacks. Performing a manual garbage
  ** collection when that threshold is passed.
  **
  ** @NOTE Enabling this feature has the potential to distort "memory" profiling
  ** as lmprof_thread_identifier_clear may trigger a Lua GC step/cycle.
  **
  **  If the flag LMPROF_STATE_IGNORE_ALLOC is enabled then all deallocations,
  **  including ones script generated, will not be accounted for. Otherwise,
  **  the amount 'deallocated' will include some profiling overhead.
  **
  **  The amount of 'overhead' can be minimized by having the profiler stacks
  **  be light-userdata and then enabling 'LMPROF_STATE_IGNORE_ALLOC' for that
  **  deallocation.
  */
  st->thread.stack_count++;
  if (!BITFIELD_TEST(st->state, LMPROF_STATE_SETTING_UP)) {
    if ((st->thread.stack_count % LMPROF_STACK_GC) == 0) {
      LMPROF_LOG("[%s] Garbage collection\n", __FUNCTION__);

      lmprof_trace_gc_event(L, st, 1);
      lmprof_thread_info_gc(L, st);
#if LUA_VERSION_NUM >= 503
      if (lua_gc(L, LUA_GCISRUNNING, 0))
        lua_gc(L, LUA_GCCOLLECT, 0);
#endif
      lmprof_trace_gc_event(L, st, 0);
    }
  }

  thread_identifier = lmprof_thread_identifier(L);
  stack = lmprof_stack_light_new(&st->hook.alloc, thread_identifier, callback_api);

  lua_pushthread(L); /* [..., thread_stacks, thread] */
  lua_pushlightuserdata(L, l_pcast(void *, stack));  /* [..., thread_stacks, thread, stack] */
  if (stack != l_nullptr) {
    lmprof_Record *record = l_nullptr;

    lua_rawset(L, -3); /* [..., thread_stacks] */
    lua_pop(L, 1);
    stack->instr_last = st->thread.r.s.time;

    /*
    ** Push root record onto stack, the root record should exist on the stack
    ** for the duration of the profile.
    */
    record = lmprof_fetch_record(L, st, l_nullptr, LMPROF_RECORD_ID_ROOT, LMPROF_RECORD_ID_ROOT, 0);
    if (callback_api) {
      lmprof_stack_event_push(stack, record, &st->thread.r, 0);
    }
    else {
      lmprof_stack_measured_push(stack, record, &st->thread.r.s, 0);
    }

    /* Populate the thread with its current traceback. */
    if (BITFIELD_TEST(st->conf, LMPROF_OPT_LOAD_STACK)) {
      int level = 0, last_line = 0;
      lu_addr last_fid = LMPROF_RECORD_ID_ROOT;
      if (!callback_api && BITFIELD_TEST(st->conf, LMPROF_OPT_COMPRESS_GRAPH))
        last_fid = record->r_id;

      /* Populate the thread with its current traceback. */
      for (level = lua_lastlevel(L); level >= 0; --level) {
        lua_Debug debug = LMPROF_ZERO_STRUCT;
        char istailcall = 0;

        lu_addr fid = LMPROF_RECORD_ID_UNKNOWN;
        lua_Debug *stack_debug = l_nullptr;
        if (lua_getstack(L, level, &debug)) {
          stack_debug = &debug;
          fid = lmprof_record_id(L, &debug, BITFIELD_TEST(st->conf, LMPROF_OPT_GC_DISABLE), l_nullptr);
#if LUA_VERSION_NUM > 501
          if (lua_getinfo(L, DEBUG_TAIL, &debug))
            istailcall = debug.istailcall != 0;
#endif
        }

        record = lmprof_fetch_record(L, st, stack_debug, fid, last_fid, last_line);
        if (callback_api) {
          lmprof_stack_event_push(stack, record, &st->thread.r, istailcall);
        }
        else {
          lmprof_stack_measured_push(stack, record, &st->thread.r.s, istailcall);
        }

        last_fid = BITFIELD_TEST(st->conf, LMPROF_OPT_COMPRESS_GRAPH) ? fid : record->r_id;
        last_line = (debug.currentline >= 0) ? debug.currentline : 0;
      }
    }
  }
  else {
    lua_pop(L, 2);
  }

  return stack;
}

void lmprof_thread_stacktable_free(lua_State *L, int idx) {
  const int t_idx = lua_absindex(L, idx);

  lmprof_Alloc l_alloc;
  l_alloc.f = lua_getallocf(L, &l_alloc.ud);

  luaL_checkstack(L, 5, __FUNCTION__);
  lua_pushnil(L); /* [..., key] */
  while (lua_next(L, t_idx) != 0) { /* [..., key, value] */
    /*
    ** @NOTE Future-proofing for if/when allocated profiler stacks no longer
    **  become managed by Lua.
    */
    if (lua_islightuserdata(L, -1)) {
      lmprof_Stack *stack = l_pcast(lmprof_Stack *, lua_touserdata(L, -1));
      lmprof_stack_light_free(&l_alloc, stack);
    }

    lua_pop(L, 1); /* [table, key] */
    lua_pushvalue(L, -1); /* [table, key, key] */
    lua_pushnil(L); /* [table, key, key, nil] */
    lua_rawset(L, -4); /* [table, key] */
  }
}

static int lmprof_thread_stacktable_gc(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lmprof_thread_stacktable_free(L, 1);
  return 0;
}

void lmprof_thread_stacks_initialize(lua_State *L) {
  static const luaL_Reg stacks_metameth[] = {
    { "__gc", lmprof_thread_stacktable_gc },
    { l_nullptr, l_nullptr }
  };

  lmprof_getlibtable(L, LMPROF_TAB_THREAD_STACKS); /* [..., thread_stacks] */
#if LUA_VERSION_NUM == 501
  lua_newtable(L);
  luaL_register(L, l_nullptr, stacks_metameth);
#else
  luaL_newlib(L, stacks_metameth);
#endif
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
}

lua_Integer lmprof_thread_identifier(lua_State *L) {
  lua_Integer id = 0;

  lmprof_getlibtable(L, LMPROF_TAB_THREAD_IDS); /* [..., thread_lookup] */
  lua_pushthread(L); /* [..., thread_lookup, thread] */
  lua_rawget(L, -2); /* [..., thread_lookup, identifier] */
  if (lua_type(L, -1) != LUA_TNUMBER) {
    lua_pop(L, 1); /* [..., thread_lookup] */

    /* Fetch unique identifier */
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LMPROF); /* [..., thread_lookup, library_table] */
    lua_rawgeti(L, -1, LMPROF_THREAD_COUNTER); /* [..., thread_lookup, library_table, counter] */
    id = luaL_optinteger(L, -1, LMPROF_THREAD_OFFSET(0));
    lua_pop(L, 1); /* [..., thread_lookup, library_table] */

    /* Increment counter */
    lua_pushinteger(L, id + 1); /* [..., thread_lookup, library_table, new_counter] */
    lua_rawseti(L, -2, LMPROF_THREAD_COUNTER);
    lua_pop(L, 1); /* [..., thread_lookup] */

    lua_pushthread(L); /* [..., thread_lookup, thread] */
    lua_pushinteger(L, id); /* [..., thread_lookup, thread, identifier] */
    lua_rawset(L, -3); /* [..., thread_lookup] */
    lua_pop(L, 1);
  }
  else {
    id = lua_tointeger(L, -1);
    lua_pop(L, 2);
  }
  return id;
}

void lmprof_thread_info_gc(lua_State *L, lmprof_State *st) {
  luaL_checkstack(L, 6, __FUNCTION__);
  lmprof_getlibtable(L, LMPROF_TAB_THREAD_NAMES); /* [..., name_table] */
  lmprof_getlibtable(L, LMPROF_TAB_THREAD_IDS); /* [..., name_table, thread_lookup] */

  lua_pushnil(L); /* [..., name_table, thread_lookup, key] */
  while (lua_next(L, -2) != 0) { /* [..., name_table, thread_lookup, key, value] */
    lua_State *co = lua_tothread(L, -2);
    lua_pop(L, 1); /* [..., name_table, thread_lookup, key] */

    /* Thread is dead, remove all supplemental profile metadata */
    if (co != l_nullptr && L != co && !luaL_verify_thread(co)) {

      /* Remove identifier from nametable */
      lua_pushvalue(L, -1);
      lua_pushnil(L); /* [..., name_table, thread_lookup, key, key, nil] */
      lua_rawset(L, -5);

      /* Remove thread from identifiers table */
      lua_pushvalue(L, -1);
      lua_pushnil(L); /* [..., name_table, thread_lookup, key, key, nil] */
      lua_rawset(L, -4);
    }
  }

  lua_pop(L, 2);

  /* Cleanup allocated profiler stacks */
  if (st != l_nullptr) {
    const uint32_t fAllocBefore = BITFIELD_TEST(st->state, LMPROF_STATE_IGNORE_ALLOC);

    lmprof_getlibtable(L, LMPROF_TAB_THREAD_STACKS); /* [..., stack_table] */
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) { /* [..., stack_table, key, value] */
      lua_State *co = lua_tothread(L, -2);
      if (co != l_nullptr && L != co && !luaL_verify_thread(co)) {
        if (lua_islightuserdata(L, -1)) {
          lmprof_Stack *stack = l_pcast(lmprof_Stack *, lua_touserdata(L, -1));

          BITFIELD_SET(st->state, LMPROF_STATE_IGNORE_ALLOC);
          lmprof_stack_light_free(&st->hook.alloc, stack);
          BITFIELD_CLEAR(st->state, LMPROF_STATE_IGNORE_ALLOC);
          BITFIELD_SET(st->state, fAllocBefore);
        }

        lua_pushvalue(L, -2); /* [..., stack_table, key, value, key] */
        lua_pushnil(L); /* [..., stack_table, key, value, key, nil] */
        lua_rawset(L, -5); /* [..., stack_table, key, value] */
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
}

LUA_API const char *lmprof_thread_name(lua_State *L, lua_Integer thread_id, const char *opt) {
  const char *name = l_nullptr;

  lmprof_getlibtable(L, LMPROF_TAB_THREAD_NAMES); /* [..., name_table] */
  lua_pushinteger(L, thread_id); /* [..., name_table, thread_identifier] */
  lua_rawget(L, -2); /* [..., name_table, name] */
  name = luaL_optstring(L, -1, opt);
  lua_pop(L, 2);
  return name;
}

void lmprof_thread_info(lua_State *L, int tab_id) {
  lmprof_getlibtable(L, tab_id); /* [..., info_table] */
}

#if defined(__cplusplus)
extern "C" {
#endif

LUALIB_API int lmprof_set_name(lua_State *L) {
  int name_idx; /* String index */
  lua_State *co; /* Thread being nammed */
  if (lua_isstring(L, 1) || lua_isnil(L, 1)) { /* String first argument: name active thread */
    co = L;
    name_idx = 1;
  }
  else if (lua_isthread(L, 1)) { /* Thread first argument: name that thread */
    luaL_checktype(L, 2, LUA_TSTRING);
    co = lua_tothread(L, 1);
    name_idx = 2;
  }
  else {
    return luaL_argerror(L, 1, "thread or string");
  }

  if (L == co || luaL_verify_thread(co)) {
    luaL_checkstack(co, 6, __FUNCTION__);
    lmprof_getlibtable(co, LMPROF_TAB_THREAD_NAMES); /* [..., name_table] */
    lua_pushinteger(co, lmprof_thread_identifier(co)); /* [..., name_table, thread_id] */
    lua_pushvalue(co, name_idx); /* [..., name_table, thread_id, name] */
    lua_rawset(co, -3); /* [..., name_table] */
    lua_pop(co, 1);
    return 0;
  }
  return luaL_argerror(L, 1, "invalid thread");
}

LUALIB_API int lmprof_get_name(lua_State *L) {
  if (lua_gettop(L) == 0 || lua_isthread(L, 1)) {
    /* Provided coroutine is invalid, return nothing. */
    lua_State *co = lua_isthread(L, 1) ? lua_tothread(L, 1) : L;
    if (co != L && !luaL_verify_thread(co))
      return 0;

    lmprof_getlibtable(L, LMPROF_TAB_THREAD_NAMES); /* [..., name_table] */
    lua_pushinteger(L, lmprof_thread_identifier(co)); /* [..., name_table, identifier] */
    lua_rawget(L, -2); /* [..., name_table, name] */
    lua_remove(L, -2); /* [..., name] */

    /* A name has not been associated with this coroutine: attempt to derive one */
    if (lua_isnil(L, -1)) {
      int i;
      lua_Debug debug = LMPROF_ZERO_STRUCT;
      for (i = 0; lua_getstack(L, i, &debug); ++i) {
      }

      if (i > 1) {
        lua_State *thread = lua_isthread(L, 1) ? lua_tothread(L, 1) : L;
        if (lua_getstack(L, i - 1, &debug) && lua_getinfo(thread, DEBUG_IMMUTABLE, &debug)) {
          lua_pop(L, 1);
          lua_pushfuncname(L, &debug);
          return 1;
        }
      }
    }
    return 1;
  }
  return 0;
}

#if defined(__cplusplus)
}
#endif
/* }================================================================== */

/*
** {==================================================================
** Functions
** ===================================================================
*/

/*
** @NOTE: This is technically an unsafe operation due to the very small
** probability of a hash collision. Technically this should be handled, however,
** that is a sacrifice willing to be made for the sake simplicity.
*/
lmprof_Record *lmprof_fetch_record(lua_State *L, lmprof_State *st, lua_Debug *ar, lu_addr fid, lu_addr pid, int p_currentline) {
  lmprof_Record *record;
  lmprof_Hash *hash = st->i.hash;
  if ((record = lmprof_hash_get(hash, fid, pid, p_currentline)) == l_nullptr) {
    /*
    ** Create a debugging record (formatted details) if current <function, parent>
    ** tuple does not exist in the hash table.
    */
    record = l_pcast(lmprof_Record *, lmprof_malloc(&st->hook.alloc, sizeof(lmprof_Record)));
    if (record == l_nullptr) {
      lmprof_error(L, st, "lmprof_record_populate allocation error");
      return l_nullptr;
    }

    memset(l_pcast(void *, record), 0, sizeof(lmprof_Record));
    record->f_id = fid;
    record->p_id = pid;
    record->r_id = st->i.record_count++;
    record->p_currentline = p_currentline;

    lmprof_record_update(L, &st->hook.alloc, ar, fid, &record->info);
    if (!lmprof_hash_insert(&st->hook.alloc, hash, record)) {
      lmprof_record_clear(&st->hook.alloc, record); /* ensure new record is free'd */
      lmprof_error(L, st, "lmprof_hash_insert error");
      return l_nullptr;
    }

    /*
    ** Update the 'ignore' field even though that feature is not used with graph
    ** profiling.
    */
    if (BITFIELD_TEST(st->conf, LMPROF_OPT_TRACE_IGNORE_YIELD) && st->hook.yield != l_nullptr && fid == (lu_addr)st->hook.yield)
      BITFIELD_SET(record->info.event, LMPROF_RECORD_IGNORED);
    else if (ar != l_nullptr) {
      lmprof_record_function(L, ar, fid); /* [..., function] */
      if (lmprof_function_is_ignored(L, -1))
        BITFIELD_SET(record->info.event, LMPROF_RECORD_IGNORED);
      lua_pop(L, 1);
    }

    /* If configured allocate a list used to store LUA_MASKCOUNT frequencies. */
    if (BITFIELD_TEST(st->mode, LMPROF_MODE_LINE | LMPROF_MODE_SAMPLE)
        && !BITFIELD_TEST(st->mode, LMPROF_CALLBACK_MASK)
        && BITFIELD_TEST(st->conf, LMPROF_OPT_LINE_FREQUENCY)
        && ar != l_nullptr && ar->linedefined > 0 && ar->lastlinedefined > 0) {

      /* @NOTE: memory managed by lmprof_record_clear */
      const int function_length = ar->lastlinedefined - ar->linedefined + 1;
      record->graph.line_freq = l_pcast(size_t *, lmprof_malloc(&st->hook.alloc, function_length * sizeof(size_t)));
      if (record->graph.line_freq != l_nullptr) {
        record->graph.line_freq_size = function_length;
        memset(l_pcast(void *, record->graph.line_freq), 0, function_length * sizeof(size_t));
      }
    }
  }
  /*
  ** Attempt the update the name/source fields of the record. This handles the
  ** case of the first interaction with a function is during a tail-call.
  */
  else {
    lmprof_record_update(L, &st->hook.alloc, ar, fid, &record->info);
  }
  return record;
}

int lmprof_function_is_ignored(lua_State *L, int idx) {
  int result = 0;
  lmprof_getlibtable(L, LMPROF_TAB_FUNC_IGNORE); /* [ ..., ignore_tab] */
  lua_pushvalue(L, (idx < 0) ? (idx - 1) : idx); /* [ ..., ignore_tab, function] */
  lua_rawget(L, -2); /* [ ..., ignore_tab, result] */
  result = lua_toboolean(L, -1);
  lua_pop(L, 2);
  return result;
}

/* Helper for adding/removing functions from the ignore table */
static int ignoretable_set(lua_State *L, int add) {
  const int top = lua_gettop(L);
  int i;

  lmprof_getlibtable(L, LMPROF_TAB_FUNC_IGNORE); /* [..., ignore_tab] */
  for (i = 1; i <= top; i++) {
    if (lua_isfunction(L, i)) {
      lua_pushvalue(L, i); /* [..., ignore_tab, function] */
      if (add)
        lua_pushboolean(L, 1); /* [..., ignore_tab, function, value] */
      else
        lua_pushnil(L);
      lua_rawset(L, -3); /* [..., ignore_tab] */
    }
  }
  lua_pop(L, 1);
  return 0;
}

#if defined(__cplusplus)
extern "C" {
#endif

LUALIB_API int lmprof_ignored_function_add(lua_State *L) {
  return ignoretable_set(L, 1);
}

LUALIB_API int lmprof_ignored_function_remove(lua_State *L) {
  return ignoretable_set(L, 0);
}

LUALIB_API int lmprof_is_ignored_function(lua_State *L) {
  const int top = lua_gettop(L);
  int i;

  lmprof_getlibtable(L, LMPROF_TAB_FUNC_IGNORE); /* [..., ignore_tab] */
  for (i = 1; i <= top; ++i) {
    if (lua_isfunction(L, i)) {
      lua_pushvalue(L, i); /* [..., ignore_tab, ..., key] */
      lua_rawget(L, top + 1); /* [..., ignore_tab, ..., value] */
    }
    else {
      lua_pushboolean(L, 0);
    }
  }

  lua_remove(L, top + 1);
  return top;
}

#if defined(__cplusplus)
}
#endif

/* }================================================================== */

/*
** {==================================================================
** Configuration
** ===================================================================
*/

#if defined(__cplusplus)
  #define EXTERN_OPT extern
#else
  #define EXTERN_OPT
#endif

EXTERN_OPT const char *const lmprof_mode_strings[] = {
  "", "time", "instrument", "memory", "trace", "lines", "sample", "single_thread", l_nullptr
};

EXTERN_OPT const uint32_t lmprof_mode_codes[] = {
  LMPROF_MODE_NONE,
  LMPROF_MODE_TIME,
  LMPROF_MODE_INSTRUMENT,
  LMPROF_MODE_MEMORY,
  LMPROF_MODE_TRACE,
  LMPROF_MODE_LINE,
  LMPROF_MODE_SAMPLE,
  LMPROF_MODE_SINGLE_THREAD
};

EXTERN_OPT const char *const lmprof_state_strings[] = {
  "persistent", "running", "error", "ignore_alloc", "ignore_next", "restore_gc", l_nullptr
};

EXTERN_OPT const uint32_t lmprof_state_codes[] = {
  LMPROF_STATE_PERSISTENT,
  LMPROF_STATE_RUNNING,
  LMPROF_STATE_ERROR,
  LMPROF_STATE_IGNORE_ALLOC,
  LMPROF_STATE_IGNORE_CALL,
  LMPROF_STATE_GC_WAS_RUNNING,
};

EXTERN_OPT const char *const lmprof_option_strings[] = {
  "disable_gc",
  "reinit_clock",
  "micro",
  "instructions",
  "load_stack",
  "mismatch",
  "compress_graph",
  "gc_count",
  "verbose",
  "output_string",
  "line_freq",
  "hash_size",
  "counter_freq",
  "ignore_yield",
  "process",
  "url",
  "name",
  "draw_frame",
  "split",
  "tracing",
  "page_limit",
  "compress",
  "threshold",
  l_nullptr
};

EXTERN_OPT const uint32_t lmprof_option_codes[] = {
  LMPROF_OPT_GC_DISABLE,
  LMPROF_OPT_CLOCK_INIT,
  LMPROF_OPT_CLOCK_MICRO,
  LMPROF_OPT_INSTRUCTION_COUNT,
  LMPROF_OPT_LOAD_STACK,
  LMPROF_OPT_STACK_MISMATCH,
  LMPROF_OPT_COMPRESS_GRAPH,
  LMPROF_OPT_GC_COUNT_INIT,
  LMPROF_OPT_REPORT_VERBOSE,
  LMPROF_OPT_REPORT_STRING,
  LMPROF_OPT_LINE_FREQUENCY,
  LMPROF_OPT_HASH_SIZE,
  LMPROF_OPT_TRACE_COUNTERS_FREQ,
  LMPROF_OPT_TRACE_IGNORE_YIELD,
  LMPROF_OPT_TRACE_PROCESS,
  LMPROF_OPT_TRACE_URL,
  LMPROF_OPT_TRACE_NAME,
  LMPROF_OPT_TRACE_DRAW_FRAME,
  LMPROF_OPT_TRACE_LAYOUT_SPLIT,
  LMPROF_OPT_TRACE_ABOUT_TRACING,
  LMPROF_OPT_TRACE_PAGELIMIT,
  LMPROF_OPT_TRACE_COMPRESS,
  LMPROF_OPT_TRACE_THRESHOLD,
};

/* Ensure only specific mask flags are set */
#define NOT_ONLY_MODE(M, MASK) (BITFIELD_TEST(M, MASK) && BITFIELD_TEST(M, ~(MASK) & LMPROF_LUA_MODE_MASK))

/*
** Parse a sequence of strings between [index, lua_gettop(L)] that correspond to
** profiling MODE_ flags. Returning the desired profiling mode configuration.
*/
uint32_t lmprof_parsemode(lua_State *L, int index, int top) {
  int i;

  /* parse function parameters: profiling flags & ... */
  uint32_t mode = lmprof_mode_codes[luaL_checkoption(L, index, "", lmprof_mode_strings)];
  for (i = index + 1; i <= top; ++i) /* at least one option is required */
    mode |= lmprof_mode_codes[luaL_checkoption(L, i, "", lmprof_mode_strings)];

  /* Sanitize Mode */
  if (BITFIELD_TEST(mode, LMPROF_LUA_MODE_MASK) == 0)
    return luaL_error(L, "Invalid profiler mode");
  /* When the 'timing' mode is specified, nothing else can be. */
  else if (NOT_ONLY_MODE(mode, LMPROF_MODE_TIME))
    return luaL_error(L, "MODE_TIME cannot be paired with other modes");
  /*
  ** For the time being "trace.sample" events are only generated when single
  ** threaded profiling is enabled. The 'sampler' callback during
  ** instrumentation mode creates another timeline that plots out the duration
  ** of each LUA_HOOKCOUNT event.
  **
  ** @TODO: The semantics of this will change once proper ProfileNode is introduced.
  */
  else if (BITFIELD_IS(mode, LMPROF_CALLBACK_MASK | LMPROF_MODE_SAMPLE) && !BITFIELD_TEST(mode, LMPROF_MODE_SINGLE_THREAD))
    return luaL_error(L, "trace + sample instrumentation is only configured for single thread profiling");
  /*
  ** Handle edge-cases when configured for sampling
  **
  ** @NOTE: If MODE_INSTRUMENT and MODE_MEMORY are set then 'MODE_SAMPLE' will
  ** count instructions. Otherwise, MODE_SAMPLE signifies to periodically sample
  ** the call stack.
  */
  else if (BITFIELD_IS(mode, LMPROF_MODE_SAMPLE | LMPROF_MODE_MEMORY) && !BITFIELD_TEST(mode, LMPROF_MODE_INSTRUMENT))
    return luaL_error(L, "memory mode cannot be enabled when in sampling mode");
  else if (BITFIELD_IS(mode, LMPROF_MODE_SAMPLE | LMPROF_MODE_LINE) && !BITFIELD_TEST(mode, LMPROF_MODE_INSTRUMENT))
    return luaL_error(L, "line mode cannot be enabled when in sampling mode");

  return mode;
}

#if defined(__cplusplus)
extern "C" {
#endif

LUALIB_API int lmprof_set_option(lua_State *L) {
  uint32_t opt = 0; /* Get current default profiler flags. */
  switch ((opt = lmprof_option_codes[luaL_checkoption(L, 1, l_nullptr, lmprof_option_strings)])) {
    case LMPROF_OPT_GC_DISABLE:
    case LMPROF_OPT_CLOCK_INIT:
    case LMPROF_OPT_CLOCK_MICRO:
    case LMPROF_OPT_LOAD_STACK:
    case LMPROF_OPT_STACK_MISMATCH:
    case LMPROF_OPT_COMPRESS_GRAPH:
    case LMPROF_OPT_GC_COUNT_INIT:
    case LMPROF_OPT_REPORT_VERBOSE:
    case LMPROF_OPT_REPORT_STRING:
    case LMPROF_OPT_LINE_FREQUENCY:
    case LMPROF_OPT_TRACE_IGNORE_YIELD:
    case LMPROF_OPT_TRACE_DRAW_FRAME:
    case LMPROF_OPT_TRACE_LAYOUT_SPLIT:
    case LMPROF_OPT_TRACE_ABOUT_TRACING:
    case LMPROF_OPT_TRACE_COMPRESS: {
      uint32_t conf = 0;
      luaL_checktype(L, 2, LUA_TBOOLEAN);

      conf = l_cast(uint32_t, lmprof_getlibi(L, LMPROF_FLAGS, LMPROF_OPT_DEFAULT));
      lmprof_setlibi(L, LMPROF_FLAGS, lua_toboolean(L, 2) ? (conf | opt) : (conf & ~opt));
      break;
    }
    case LMPROF_OPT_INSTRUCTION_COUNT: {
      const lua_Integer count = luaL_checkinteger(L, 2);
      if (count > 0) {
        lmprof_setlibi(L, LMPROF_HOOK_COUNT, count);
        break;
      }
      return luaL_error(L, "instruction count less-than/equal to zero");
    }
    case LMPROF_OPT_HASH_SIZE: {
      const lua_Integer count = luaL_checkinteger(L, 2);
      if (count >= 1 && count <= LMPROF_HASH_MAXSIZE) {
        lmprof_setlibi(L, LMPROF_HASHTABLE_SIZE, count);
        break;
      }
      return luaL_error(L, "hashtable size is less-than/equal to zero");
    }
    /*
    ** If the profiler is already running, updating the registry table will not
    ** affect/change the subsequent profile records process.
    */
    case LMPROF_OPT_TRACE_PROCESS: {
      const lua_Integer process = luaL_checkinteger(L, 2);
      /*
      ** if (process == LMPROF_PROCESS_MAIN)
      **  luaL_error("set process cannot be the main process (%I)", process);
      */
      lmprof_setlibi(L, LMPROF_PROCESS, process);
      break;
    }
    case LMPROF_OPT_TRACE_NAME:
      lmprof_setlibs(L, LMPROF_PROFILE_NAME, luaL_checkstring(L, 2));
      break;
    case LMPROF_OPT_TRACE_URL:
      lmprof_setlibs(L, LMPROF_URL, luaL_checkstring(L, 2));
      break;
    case LMPROF_OPT_TRACE_PAGELIMIT:
      lmprof_setlibi(L, LMPROF_PAGE_LIMIT, luaL_checkinteger(L, 2));
      break;
    case LMPROF_OPT_TRACE_COUNTERS_FREQ:
      lmprof_setlibi(L, LMPROF_COUNTERS_FREQ, luaL_checkinteger(L, 2));
      break;
    case LMPROF_OPT_TRACE_THRESHOLD: {
      const lua_Integer max_threshold = 1024 * 1024;
      const lua_Integer threshold = luaL_checkinteger(L, 2);
      if (0 <= threshold && threshold <= max_threshold) {
        lmprof_setlibi(L, LMPROF_THRESHOLD, threshold);
        break;
      }
      return luaL_error(L, "threshold not within [0, %d]", (int)max_threshold);
    }
    default:
      return luaL_error(L, "Invalid option %s", luaL_checkstring(L, 1));
  }
  return 0;
}

LUALIB_API int lmprof_get_option(lua_State *L) {
  const uint32_t opt = lmprof_option_codes[luaL_checkoption(L, 1, l_nullptr, lmprof_option_strings)];
  switch (opt) {
    case LMPROF_OPT_GC_DISABLE:
    case LMPROF_OPT_CLOCK_INIT:
    case LMPROF_OPT_CLOCK_MICRO:
    case LMPROF_OPT_LOAD_STACK:
    case LMPROF_OPT_STACK_MISMATCH:
    case LMPROF_OPT_COMPRESS_GRAPH:
    case LMPROF_OPT_GC_COUNT_INIT:
    case LMPROF_OPT_REPORT_VERBOSE:
    case LMPROF_OPT_REPORT_STRING:
    case LMPROF_OPT_LINE_FREQUENCY:
    case LMPROF_OPT_TRACE_IGNORE_YIELD:
    case LMPROF_OPT_TRACE_DRAW_FRAME:
    case LMPROF_OPT_TRACE_LAYOUT_SPLIT:
    case LMPROF_OPT_TRACE_ABOUT_TRACING:
    case LMPROF_OPT_TRACE_COMPRESS:
      lua_pushboolean(L, BITFIELD_TEST(lmprof_getlibi(L, LMPROF_FLAGS, 0), opt) != 0);
      break;
    case LMPROF_OPT_INSTRUCTION_COUNT:
      lmprof_getlibfield(L, LMPROF_HOOK_COUNT);
      break;
    case LMPROF_OPT_HASH_SIZE:
      lmprof_getlibfield(L, LMPROF_HOOK_COUNT);
      break;
    case LMPROF_OPT_TRACE_PROCESS:
      lmprof_getlibfield(L, LMPROF_PROCESS);
      break;
    case LMPROF_OPT_TRACE_URL:
      lmprof_getlibfield(L, LMPROF_URL);
      break;
    case LMPROF_OPT_TRACE_NAME:
      lmprof_getlibfield(L, LMPROF_PROFILE_NAME);
      break;
    case LMPROF_OPT_TRACE_PAGELIMIT:
      lmprof_getlibfield(L, LMPROF_PAGE_LIMIT);
      break;
    case LMPROF_OPT_TRACE_THRESHOLD:
      lmprof_getlibfield(L, LMPROF_THRESHOLD);
      break;
    case LMPROF_OPT_TRACE_COUNTERS_FREQ:
      lmprof_getlibfield(L, LMPROF_COUNTERS_FREQ);
      break;
    default:
      return 0;
  }
  return 1;
}

LUALIB_API int lmprof_get_timeunit(lua_State *L) {
  lua_pushstring(L, LMPROF_TIME_ID(LMPROF_OPT_DEFAULT));
  return 1;
}

LUALIB_API int lmprof_get_has_io(lua_State *L) {
#if defined(LMPROF_FILE_API)
lua_pushboolean(L, 1);LMPROF_FILE_API
  lua_pushboolean(L, 1);
#else
  lua_pushboolean(L, 0);
  #if defined(LMPROF_DISABLE_OUTPUT_PATH)
  lua_pushboolean(L, 0);
  #else
  lua_pushboolean(L, 1);
  #endif
#endif
  return 2;
}

#if defined(__cplusplus)
}
#endif

/* }================================================================== */
