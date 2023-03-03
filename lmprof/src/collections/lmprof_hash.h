/*
** $Id: lmprof_hash.h $
**
** A simple hashtable implementation for representing <parent, child>
** relationships between functions (i.e., parent invoking child).
**
** See Copyright Notice in lmprof_lib.h
*/
#ifndef lmprof_hash_h
#define lmprof_hash_h

#include "../lmprof_conf.h"

/*
@@ LMPROF_HASH_MAXSIZE: The maximum allowable size of the hash table. This value
** is used to simplify hash operations that allocate intermediate data.
*/
#define LMPROF_HASH_MAXSIZE 1031

/*
@@ LMPROF_HASH_SIZE: Default number of buckets in a hash table. The upper limit
** is defined by LMPROF_HASH_MAXSIZE.
*/
#if !defined(LMPROF_HASH_SIZE)
  #define LMPROF_HASH_SIZE 257
#endif

#if LMPROF_HASH_SIZE > LMPROF_HASH_MAXSIZE
  #error "Invalid Hash size!"
#endif

/*
** Separately chained bucket, i.e., a list of profiler records that share the
** same index.
*/
typedef struct lmprof_HashBucket {
  struct lmprof_Record *record; /* Function Details & Statistics */
  struct lmprof_HashBucket *next; /* Bucket next */
} lmprof_HashBucket;

/* Hashtable implemented as an array of chained hash buckets */
typedef struct lmprof_Hash {
  size_t bucket_count;
  lmprof_HashBucket *buckets[1];
} lmprof_Hash;

/* Create a new hash table, returning NULL on error. */
LUA_API lmprof_Hash *lmprof_hash_create(lmprof_Alloc *alloc, size_t bucket_count);

/* Destroy & free a hashtable instance. */
LUA_API void lmprof_hash_destroy(lmprof_Alloc *alloc, lmprof_Hash *hash);

/* Create an identifier that mixes the functions identifier with its parent. */
LUA_API lu_addr lmprof_hash_identifier(lu_addr fid, lu_addr pid, int p_currentline);

/*
** Fetch the function record associated with the function/parent relationship,
** or NULL if this hashtable contains no mapping for said functions.
*/
LUA_API struct lmprof_Record *lmprof_hash_get(lmprof_Hash *h, lu_addr fid, lu_addr pid, int p_currentline);

/*
** Inserts the function record into the hash table, i.e., associates the
** function & parent identifier with the function record.
*/
LUA_API int lmprof_hash_insert(lmprof_Alloc *alloc, lmprof_Hash *h, struct lmprof_Record *record);

/*
** Clear all records (i.e., lstrace_clear) contained within all hashbuckets of
** the provided hashtable.
*/
LUA_API void lmprof_hash_clear_statistics(lmprof_Hash *h);

/*
** {==================================================================
** Formatting
** ===================================================================
*/

/* Iterator interface */
typedef int (*lmprof_hash_Callback)(lua_State *, struct lmprof_Record *, const void *);

/*
** Traverse through each hash bucket of the provided hashtable and invoking "cb"
** for each stack instance in the stack.
*/
LUA_API void lmprof_hash_report(lua_State *L, lmprof_Hash *h, lmprof_hash_Callback cb, const void *args);

/*
** Push a table on top of the Lua stack that contains some summary statistics
** of the hash buckets & hashing algorithm.
*/
LUA_API int lmprof_hash_debug(lua_State *L, lmprof_Hash *h);

/* }================================================================== */

#endif
