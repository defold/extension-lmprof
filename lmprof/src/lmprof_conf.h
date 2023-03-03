/*
** $Id: lmprof_conf.h $
** Shared type and Lua compatibility definitions.
** See Copyright Notice in lmprof_lib.h
*/
#ifndef lmprof_conf_h
#define lmprof_conf_h

#if defined(LUA_COMPILED_AS_HPP)
  #define LMPROF_LUA_LINKAGE "C++"
#else
  #define LMPROF_LUA_LINKAGE "C"
#endif

/*
** Lua 5.1 does not bundle the lua.hpp header by default, although, most package
** managers will contain the header.
*/
#if !defined(LUA_EXTERN_BEGIN)
#if defined(__cplusplus)
  #define LUA_EXTERN_BEGIN extern LMPROF_LUA_LINKAGE {
  #define LUA_EXTERN_END }
#else
  #define LUA_EXTERN_BEGIN
  #define LUA_EXTERN_END
#endif
#endif

LUA_EXTERN_BEGIN
extern "C" {
  #include <dmsdk/lua/lua.h>
  #include <dmsdk/lua/lauxlib.h>
  // #include <dmsdk/lua/lualib.h>
}
LUA_EXTERN_END

/* Compiler-specific forceinline definition */
#if !defined(LUA_INLINE)
  #ifdef _MSC_VER
    #define LUA_INLINE __forceinline
  #elif __has_attribute(__always_inline__)
    #define LUA_INLINE inline __attribute__((__always_inline__))
  #else
    #define LUA_INLINE inline
  #endif
#endif

/* Multi-line macro definition */
#if !defined(LUA_MLM_BEGIN)
  #ifdef _MSC_VER
    #define LUA_MLM_BEGIN do {
    #define LUA_MLM_END                 \
      __pragma(warning(push))           \
      __pragma(warning(disable : 4127)) \
      }                                 \
      while (0)                         \
      __pragma(warning(pop))
  #else
    #define LUA_MLM_BEGIN do {
    #define LUA_MLM_END \
      }                 \
      while (0)
  #endif
#endif

/*
** {==================================================================
** Profiler Type Info
** ===================================================================
*/

/*
@@ LUA_32BITS enables Lua with 32-bit integers and 32-bit floats. If undefined,
** it is assumed the underlying timer has nanosecond resolution (or access to
** the time-stamp counter/RDTSC) and lu_time is large enough to accommodate that
** measurement of time.
*/
#if !defined(LUA_32BITS)
  #define LUA_32BITS 0
#endif

/*
@@ lu_addr: Type used to represent addresses and function identifiers.
@@ lu_time: Type used to represent times and changes-in-time.
@@ lu_size: Type used to represent byte allocations/memory-tracking.
@@ PRIluADDR: lu_addr format string.
@@ PRIluTIME: lu_time format string.
@@ PRIluTIME: lu_size format string.
*/
#if !LUA_32BITS
  #include <stdint.h>
  #include <inttypes.h>
  typedef uintptr_t lu_addr;
  typedef uint64_t lu_time;
  typedef size_t lu_size;
  typedef ptrdiff_t lu_sizediff;

  #define PRIluADDR PRIxPTR
  #define PRIluTIME PRIu64
  #define PRIluSIZE "zu"
  #define PRIluSIZEDIFF "td"

  /* Time conversions. */
  #define LU_TIME_NANO(T) ((T))
  #define LU_TIME_MICRO(T) ((T) / 1000)
  #define LU_TIME_MILLI(T) ((T) / 1000000)
#else
  typedef size_t lu_addr;
  typedef size_t lu_time;
  typedef size_t lu_size;
  typedef ptrdiff_t lu_sizediff;

  #define PRIluADDR "zu"
  #define PRIluTIME "zu"
  #define PRIluSIZE "zu"
  #define PRIluSIZEDIFF "td"

  #define LU_TIME_NANO(T) ((T)*1000)
  #define LU_TIME_MICRO(T) ((T))
  #define LU_TIME_MILLI(T) ((T) / 1000)
#endif

/* A profiling measurement unit. */
typedef struct lmprof_EventUnit {
  lu_time time; /* Execution time */
  lu_size allocated; /* Number of bytes allocated */
  lu_size deallocated; /* Number of bytes deallocated */
} lmprof_EventUnit;

/*
** Process and Thread identifiers. A "process" will represent a global_State or
** collection of threads with a shared identifier. A "thread" will represent a
** unique integer identifier for a given lua_State.
*/
typedef struct lmprof_EventProcess {
  lua_Integer pid;
  lua_Integer tid;
} lmprof_EventProcess;

/* TraceEvent stack measurement */
typedef struct lmprof_EventMeasurement {
  lmprof_EventProcess proc; /* process information */
  lmprof_EventUnit s; /* profiling measurement */
  lu_time overhead; /* Total accumulated error/profiling overhead */
} lmprof_EventMeasurement;

/*
** Macros for the largest time and size values. Generally, these values should
** only matter for 32-bit builds.
*/
#if defined(__cplusplus)
  #define MAX_LU_TIME std::numeric_limits<lu_time>::max()
  #define MAX_LU_SIZE std::numeric_limits<lu_size>::max()
#else
  #define MAX_LU_TIME ((lu_time)(~(lu_time)0))
  #define MAX_LU_SIZE ((lu_size)(~(lu_size)0))
#endif

/* When compiling for C++, include macros for common warnings, e.g., -Wold-style-cast. */
#if defined(__cplusplus)
  #define l_nullptr nullptr /* @TODO: C++98? */
  #define l_cast(t, exp) static_cast<t>(exp)
  #define l_pcast(t, exp) reinterpret_cast<t>(exp)
#else
  #define l_nullptr NULL
  #define l_cast(t, exp) ((t)(exp))
  #define l_pcast(t, exp) ((t)(exp))
#endif

/* }================================================================== */

/*
** {==================================================================
**  Lua Utilities
** ===================================================================
*/

/* Backwards compatible config macros */
#if LUA_VERSION_NUM >= 501 && LUA_VERSION_NUM <= 503
  #define LUA_GNAME "_G"
  #define LUA_LOADED_TABLE "_LOADED"
  #define LUA_OK 0
#endif

/* macro to avoid warnings about unused variables */
#if !defined(UNUSED)
  #define UNUSED(x) ((void)(x))
#endif

#if LUA_VERSION_NUM <= 502
  #define LUA_INTEGER_FMT "%td"
  #if defined(_WIN32) && !defined(_WIN32_WCE)
    #define LUA_USE_WINDOWS
  #endif
#endif

/* lua_absindex introduced in Lua5.2 */
#if LUA_VERSION_NUM < 502
  #define lua_absindex(L, i) ((i) > 0 || (i) <= LUA_REGISTRYINDEX ? (i) : lua_gettop(L) + (i) + 1)
#endif

/* Lua 5.4 changed the definition of lua_newuserdata; no upvalues required */
#if LUA_VERSION_NUM == 504
  #define lmprof_newuserdata(L, s) lua_newuserdatauv((L), (s), 0)
#elif LUA_VERSION_NUM >= 501 && LUA_VERSION_NUM <= 503
  #define lmprof_newuserdata(L, s) lua_newuserdata((L), (s))
#else
  #error unsupported Lua version
#endif

#if !defined(luaL_settabss)
  #define luaL_settabss(L, K, V) \
    LUA_MLM_BEGIN                \
    lua_pushstring((L), (V));    \
    lua_setfield((L), -2, (K));  \
    LUA_MLM_END

  #define luaL_settabsi(L, K, V) \
    LUA_MLM_BEGIN                \
    lua_pushinteger((L), (V));   \
    lua_setfield((L), -2, (K));  \
    LUA_MLM_END

  #define luaL_settabsn(L, K, V) \
    LUA_MLM_BEGIN                \
    lua_pushnumber((L), (V));    \
    lua_setfield((L), -2, (K));  \
    LUA_MLM_END

  #define luaL_settabsb(L, K, V) \
    LUA_MLM_BEGIN                \
    lua_pushboolean((L), (V));   \
    lua_setfield((L), -2, (K));  \
    LUA_MLM_END
#endif

/* Utility debugging macro */
#if defined(_DEBUG) || defined(LMPROF_FORCE_LOGGER)
  #include <stdio.h>
  #define LMPROF_HAS_LOGGER 1
  #define LMPROF_LOG(f_, ...)             \
    LUA_MLM_BEGIN                         \
    fprintf(stderr, (f_), ##__VA_ARGS__); \
    fflush(stderr);                       \
    LUA_MLM_END
#else
  #define LMPROF_HAS_LOGGER 0
  #define LMPROF_LOG(f_, ...) \
    LUA_MLM_BEGIN             \
    LUA_MLM_END
#endif

#if defined(__cplusplus) /* POD Zero */
  #define LMPROF_ZERO_STRUCT {}
#else
  #define LMPROF_ZERO_STRUCT {0}
#endif

/* 'some useful bit tricks' from lgc.h */
#define BITFIELD_TEST(x, m) ((x) & (m))
#define BITFIELD_CLEAR(x, m) ((x) &= ~(m))
#define BITFIELD_SET(x, m) ((x) |= (m))
#define BITFIELD_FLIP(x, m) ((x) ^= (m))
#define BITFIELD_CHANGE(x, m, b) ((x) ^= (-(!!(b)) ^ (x)) & (m))
#define BITFIELD_IS(x, m) (BITFIELD_TEST(x, m) == (m))

/* }================================================================== */

/*
** {==================================================================
**  Allocator Intermediate
** ===================================================================
*/

/*
** A cache of the initial allocator function and opaque pointer from lua_State;
** allowing use of the original Lua allocator while profiling.
*/
typedef struct lmprof_Alloc {
  lua_Alloc f;
  void *ud;
} lmprof_Alloc;

/* malloc: allocates size bytes and returns a pointer to the allocated memory */
LUA_API void *lmprof_malloc(lmprof_Alloc *alloc, size_t size);

/* realloc: change the size of the memory block pointed to by ptr to size bytes */
LUA_API void *lmprof_realloc(lmprof_Alloc *alloc, void *p, size_t osize, size_t nsize);

/*
** free: free the memory space pointed to by ptr, which must have been returned
** by a previous call to malloc or realloc.
*/
LUA_API void *lmprof_free(lmprof_Alloc *alloc, void *p, size_t size);

/* A strdup implementation that uses the Lua allocator to duplicate memory. */
LUA_API char *lmprof_strdup(lmprof_Alloc *alloc, const char *source, size_t len);

/*
** Free a string allocated by lmprof_strdup.
**
** The Lua allocator requires the current size of the 'pointer' (although the
** default implementation ignores this value and just forwards the pointer to
** free/realloc). Therefore, its assumed the length of the string is identical
** to the number of bytes allocated. **Use with caution**.
*/
LUA_API char *lmprof_strdup_free(lmprof_Alloc *alloc, const char *source, size_t len);

/* }================================================================== */

/*
** {==================================================================
**  Timer
** ===================================================================
*/

/* Initialize, or reinitialize, the timer (or performance counter) */
LUA_API void lmprof_clock_init(void);

/* Sample the current value of the performance counter (units of time unspecified) */
LUA_API lu_time lmprof_clock_sample(void);

/*
** Sample the rdtsc instruction, returning the process time stamp (cycle
** references since last reset)
*/
LUA_API lu_time lmprof_clock_rdtsc(void);

/* Return the difference between two time values; taking overflow into account */
static LUA_INLINE lu_time lmprof_clock_diff(lu_time start, lu_time end) {
  return start <= end ? (end - start) : (start - end);
}

/* }================================================================== */

/*
#if defined(ltests_h)
  #error "ltests.h redefines LUAI_MAXSTACK & LUA_REGISTRYINDEX"
#endif
*/

#endif
