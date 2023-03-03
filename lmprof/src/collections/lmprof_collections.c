/*
** $Id: lmprof_collections.c $
** Collection and Utility Implementations.
** See Copyright Notice in lmprof_lib.h
*/
#define LUA_CORE

#include <string.h>

#include "../lmprof_conf.h"

#include "lmprof_record.h"
#include "lmprof_stack.h"
#include "lmprof_hash.h"
#include "lmprof_traceevent.h"

/*
** {==================================================================
**  Stack
** ===================================================================
*/

/* Number of elements */
#define LMPROF_STACK_SIZE ((sizeof(lmprof_StackInst) * LMPROF_MAXSTACK) + offsetof(lmprof_Stack, stack))

/* Helper for initializing/zeroing an allocated stack instance. */
static LUA_INLINE lmprof_Stack *setup_stack(lmprof_Stack *s, lua_Integer id, char callback_api) {
  if (s != l_nullptr) {
    s->instr_last = 0;
    s->instr_count = 0;
    s->thread_identifier = id;
    s->callback_api = callback_api;
    lmprof_stack_clear(s); /* Ensure the profile stack is zeroed out */
  }
  return s;
}

LUA_API lmprof_Stack *lmprof_stack_new(lua_State *L, lua_Integer id, char callback_api) {
  lmprof_Stack *s = l_pcast(lmprof_Stack *, lmprof_newuserdata(L, LMPROF_STACK_SIZE));
  return setup_stack(s, id, callback_api);
}

LUA_API lmprof_Stack *lmprof_stack_light_new(lmprof_Alloc *alloc, lua_Integer id, char callback_api) {
  lmprof_Stack *s = l_pcast(lmprof_Stack *, lmprof_malloc(alloc, LMPROF_STACK_SIZE));
  return setup_stack(s, id, callback_api);
}

LUA_API void lmprof_stack_light_free(lmprof_Alloc *alloc, lmprof_Stack *stack) {
  lmprof_free(alloc, l_pcast(void *, stack), LMPROF_STACK_SIZE);
}

LUA_API void lmprof_stack_clear(lmprof_Stack *s) {
  size_t i;
  s->head = 0;
  s->size = LMPROF_MAXSTACK;
  for (i = 0; i < s->size; ++i) /* Clear the entire allocated stack */
    stack_clear_instance(s, &s->stack[i]);
}

LUA_API void stack_clear_instance(lmprof_Stack *s, lmprof_StackInst *inst) {
  if (inst == l_nullptr)
    return;

  inst->tail_call = 0;
  inst->last_line = 0;
  inst->last_line_instructions = 0;
  if (s->callback_api) {
    inst->trace.record = l_nullptr;
    inst->trace.begin_event = l_nullptr;
    inst->trace.call.overhead = 0;
    inst->trace.call.proc.pid = LMPROF_PROCESS_MAIN;
    inst->trace.call.proc.tid = LMPROF_THREAD_BROWSER;
    unit_clear(&inst->trace.call.s);
  }
  else {
    inst->graph.overhead = 0;
    inst->graph.record = l_nullptr;
    unit_clear(&inst->graph.node);
    unit_clear(&inst->graph.path);
  }
}

LUA_API lmprof_StackInst *lmprof_stack_measured_push(lmprof_Stack *s, lmprof_Record *record, lmprof_EventUnit *unit, char tail) {
  lmprof_StackInst *inst = l_nullptr;
  if (!s->callback_api && (inst = lmprof_stack_next(s, tail)) != l_nullptr) {
    inst->graph.overhead = 0;
    inst->graph.record = record;
    inst->graph.node = *unit;
    unit_clear(&inst->graph.path);
  }
  return inst;
}

LUA_API lmprof_StackInst *lmprof_stack_measured_pop(lmprof_Stack *s, lmprof_EventUnit *unit) {
  lmprof_EventUnit node;
  lmprof_StackInst *inst = &s->stack[--s->head];
  lmprof_Record *record = inst->graph.record;

  /* Total Measurement: Time/Memory since initial push */
  unit_sub(&node, unit, &inst->graph.node);
  inst->graph.node.time -= inst->graph.overhead;

  /* Path Measurement: The function & all of its children */
  unit_add_to(&record->graph.path, &node);
  if (s->head > 0) { /* increment parent path by its diff */
    s->stack[s->head - 1].graph.overhead += inst->graph.overhead;
    unit_add_to(&(s->stack[s->head - 1].graph.path), &node);
  }

  /* Record Measurement */
  record->graph.count++;
  unit_sub(&node, &node, &inst->graph.path); /* This function: total - children */
  unit_add_to(&record->graph.node, &node);
  return inst;
}

/* }================================================================== */

/*
** {==================================================================
**  Hash
** ===================================================================
*/

/* Size of the hashtable. */
#define LMPROF_SIZEOF_HASH(BC) (offsetof(lmprof_Hash, buckets) + ((BC) * sizeof(lmprof_HashBucket)))

/* Source: 'Prospecting for Hash Functions': nullprogram.com/blog/2018/07/31/ */
#if defined(LMPROF_HASH_SPLITMIX)
#if LUA_32BITS
static LUA_INLINE lu_addr splitmix(lu_addr x) { /* h2 */
  x = ((x >> 16) ^ x) * 0x45d9f3b;
  x = ((x >> 16) ^ x) * 0x45d9f3b;
  x = (x >> 16) ^ x;
  return x;
}
#else
static LUA_INLINE lu_addr splitmix(lu_addr x) { /* splitmix */
  x = (x ^ (x >> 30)) * l_cast(lu_addr, 0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * l_cast(lu_addr, 0x94d049bb133111eb);
  x = x ^ (x >> 31);
  return x;
}
#endif

static LUA_INLINE lu_addr to_identifier(lu_addr fid, lu_addr pid, int p_currentline) {
  return (splitmix(fid) ^ splitmix(pid)) + l_cast(lu_addr, p_currentline);
}
#else
/* @TODO: Consider bloom filters. */
static LUA_INLINE lu_addr to_identifier(lu_addr fid, lu_addr pid, int p_currentline) {
  const lu_addr p = (fid ^ pid);
  const lu_addr pcl = l_cast(lu_addr, p_currentline);
  return ((p >> 3) ^ (p >> 19) ^ (p & 7)) + pcl;
}
#endif

/* <Function, Parent, LineNumber> identifiers mapped to hash bucket. */
static LUA_INLINE size_t to_bucket(lu_addr fid, lu_addr pid, int p_currentline, size_t bucketCount) {
  return l_cast(size_t, to_identifier(fid, pid, p_currentline)) % bucketCount;
}

LUA_API lmprof_Hash *lmprof_hash_create(lmprof_Alloc *alloc, size_t bucket_count) {
  lmprof_Hash *h = l_pcast(lmprof_Hash *, lmprof_malloc(alloc, LMPROF_SIZEOF_HASH(bucket_count)));
  if (h != l_nullptr) {
    size_t i;

    h->bucket_count = bucket_count;
    for (i = 0; i < h->bucket_count; i++)
      h->buckets[i] = l_nullptr;
  }
  return h;
}

LUA_API void lmprof_hash_destroy(lmprof_Alloc *alloc, lmprof_Hash *hash) {
  size_t i;
  for (i = 0; i < hash->bucket_count; i++) {
    lmprof_HashBucket *bucket = hash->buckets[i];
    while (bucket != l_nullptr) {
      lmprof_HashBucket *next = bucket->next;
      lmprof_Record *record = bucket->record;
      if (record != l_nullptr && !BITFIELD_TEST(record->info.event, LMPROF_RECORD_USERDATA)) {
        lmprof_record_clear(alloc, record);
        lmprof_free(alloc, l_pcast(void *, record), sizeof(lmprof_Record));
      }

      lmprof_free(alloc, l_pcast(void *, bucket), sizeof(lmprof_HashBucket));
      bucket = next;
    }
    hash->buckets[i] = l_nullptr;
  }

  lmprof_free(alloc, l_pcast(void *, hash), LMPROF_SIZEOF_HASH(hash->bucket_count));
}

LUA_API lu_addr lmprof_hash_identifier(lu_addr fid, lu_addr pid, int p_currentline) {
  return to_identifier(fid, pid, p_currentline);
}

LUA_API lmprof_Record *lmprof_hash_get(lmprof_Hash *h, lu_addr fid, lu_addr pid, int p_currentline) {
  const size_t bucket = to_bucket(fid, pid, p_currentline, h->bucket_count);

  lmprof_HashBucket *fh = l_nullptr;
  lmprof_HashBucket *prev = l_nullptr;
  for (fh = h->buckets[bucket]; fh != l_nullptr; fh = fh->next) {
    if (fid == fh->record->f_id && pid == fh->record->p_id && p_currentline == fh->record->p_currentline) {
      /*
      ** Move the record to the beginning of the linked bucket list. Hoping for
      ** some locality in hashing accesses.
      */
      if (prev) {
        prev->next = fh->next;
        fh->next = h->buckets[bucket];
        h->buckets[bucket] = fh;
      }
      return fh->record;
    }
    prev = fh;
  }
  return l_nullptr;
}

LUA_API int lmprof_hash_insert(lmprof_Alloc *alloc, lmprof_Hash *h, lmprof_Record *record) {
  lmprof_HashBucket *val = l_pcast(lmprof_HashBucket *, lmprof_malloc(alloc, sizeof(lmprof_HashBucket)));
  if (val != l_nullptr) {
    const size_t bucket = to_bucket(record->f_id, record->p_id, record->p_currentline, h->bucket_count);
    val->record = record;
    val->next = h->buckets[bucket]; /* insert in front */
    h->buckets[bucket] = val;
    return 1;
  }
  return 0;
}

LUA_API void lmprof_hash_clear_statistics(lmprof_Hash *h) {
  size_t i;
  for (i = 0; i < h->bucket_count; i++) {
    lmprof_HashBucket *fh = l_nullptr;
    for (fh = h->buckets[i]; fh != l_nullptr; fh = fh->next) {
      lmprof_record_clear_graph_statistics(fh->record);
    }
  }
}

LUA_API void lmprof_hash_report(lua_State *L, lmprof_Hash *h, lmprof_hash_Callback cb, const void *args) {
  size_t i;
  for (i = 0; i < h->bucket_count; i++) {
    lmprof_HashBucket *fh;
    for (fh = h->buckets[i]; fh != l_nullptr; fh = fh->next) {
      const int result = cb(L, fh->record, args);
      if (result != LUA_OK) {
        LMPROF_LOG("Preempting hash iteration: <%d>\n", result);
        return;
      }
    }
  }
}

LUA_API int lmprof_hash_debug(lua_State *L, lmprof_Hash *h) {
  size_t count = 0; /* Total number of records */
  size_t nonempty = 0; /* Number of non-empty buckets */
  size_t min = 0, max = 0; /* Min/Max of non-empty buckets */
  size_t freqs[LMPROF_HASH_MAXSIZE + 1];
  double mall = 0.0, mhits = 0.0;
  double ssqall = 0.0, ssqhits = 0.0;

  size_t i;
  for (i = 0; i < h->bucket_count; i++) {
    const lmprof_HashBucket *fh = l_nullptr;

    freqs[i] = 0;
    for (fh = h->buckets[i]; fh != l_nullptr; fh = fh->next) {
      count++;
      freqs[i]++;
    }

    nonempty += (freqs[i] > 0) ? 1 : 0;
    min = (min == 0 || (freqs[i] > 0 && freqs[i] < min)) ? freqs[i] : min;
    max = (max == 0 || (freqs[i] > 0 && freqs[i] > max)) ? freqs[i] : max;
  }

  mhits = ((double)count) / ((double)nonempty);
  mall = ((double)count) / ((double)h->bucket_count);
  for (i = 0; i < h->bucket_count; i++) {
    ssqall += (((double)freqs[i] - mall) * ((double)freqs[i] - mall));
    if (freqs[i] > 0)
      ssqhits += (((double)freqs[i] - mhits) * ((double)freqs[i] - mhits));
  }

  luaL_settabsi(L, "buckets", l_cast(lua_Integer, h->bucket_count));
  luaL_settabsi(L, "used_buckets", l_cast(lua_Integer, nonempty));
  luaL_settabsi(L, "record_count", l_cast(lua_Integer, count));
  luaL_settabsn(L, "min", l_cast(lua_Number, min));
  luaL_settabsn(L, "max", l_cast(lua_Number, max));
  luaL_settabsn(L, "mean", l_cast(lua_Number, mall));
  luaL_settabsn(L, "mean_hits", l_cast(lua_Number, mhits));
  luaL_settabsn(L, "var", l_cast(lua_Number, ssqall / (double)(h->bucket_count - 1)));
  luaL_settabsn(L, "var_hits", l_cast(lua_Number, ssqhits / (double)(nonempty - 1)));
  return 1;
}

/* }================================================================== */

/*
** {==================================================================
**  TraceEventTimeline
** ===================================================================
*/

/*
@@ TRACE_EVENT_PAGE_SIZE: The default TraceEventPage size. This value should
**  be a reflection of the operating system page size (or some factor of).
*/
#if !defined(TRACE_EVENT_PAGE_SIZE)
  #define TRACE_EVENT_PAGE_SIZE 32768
/*
  #if defined(LUA_USE_POSIX)
    #include <unistd.h>
    #define TRACE_EVENT_PAGE_SIZE sysconf(_SC_PAGESIZE)
  #else
    #define TRACE_EVENT_PAGE_SIZE 32768
  #endif
*/
#endif

/* Number of TraceEvent instances that can fit within a single TraceEventPage */
#define TRACE_EVENT_SIZE_ARRAY ((TRACE_EVENT_PAGE_SIZE - offsetof(TraceEventPage, event_array)) / sizeof(TraceEventPage))

LUA_API const char *traceevent_strerror(int traceevent_errno) {
  switch (traceevent_errno) {
    case TRACE_EVENT_ERRARG:
      return "Invalid TraceEvent list configuration";
    case TRACE_EVENT_ERRPAGEFULL:
      return "TraceEvent list is full";
    case TRACE_EVENT_ERRMEM:
      return "Could not allocate TraceEvent compression stack";
    case TRACE_EVENT_ERRSTACKFULL:
      return "Maximum ENTER_SCOPE limit reached";
    case TRACE_EVENT_ERRSTACKEMPTY:
      return "Handled EXIT_SCOPE TraceEvent without associated ENTER_SCOPE";
    case TRACE_EVENT_ERRPROCESS:
      return "Process identifier mismatch for ENTER_SCOPE/EXIT_SCOPE pairing";
    case TRACE_EVENT_ERRTHREAD:
      return "Thread identifier mismatch for ENTER_SCOPE/EXIT_SCOPE pairing";
    case TRACE_EVENT_ERRFUNCINFO:
      return "Mismatch in function information handles (yield[C] != resume[C] only accepted)";
    default:
      return "Unknown Error";
  }
}

LUA_API TraceEventPage *traceevent_new(lmprof_Alloc *alloc) {
  TraceEventPage *page = l_pcast(TraceEventPage *, lmprof_malloc(alloc, TRACE_EVENT_PAGE_SIZE));
  if (page != l_nullptr) {
    page->count = 0;
    page->next = l_nullptr;
    return page;
  }

  return l_nullptr;
}

LUA_API void traceevent_free(lmprof_Alloc *alloc, TraceEventPage *page) {
  size_t i;
  for (i = 0; i < page->count; ++i) {
    TraceEvent *event = &page->event_array[i];
    event->call.proc.pid = LMPROF_PROCESS_MAIN;
    event->call.proc.tid = LMPROF_THREAD_BROWSER;
    unit_clear(&event->call.s);
    switch (event->op) {
      case PROCESS:
      case THREAD:
        if (event->data.process.name != l_nullptr) {
          lmprof_strdup_free(alloc, event->data.process.name, event->data.process.nameLen);
          event->data.process.name = l_nullptr;
          event->data.process.nameLen = 0;
        }
        break;
      case SAMPLE_EVENT:
        event->data.sample.next = l_nullptr;
        break;
      case LINE_SCOPE:
        event->data.line.line = -1;
        event->data.line.info = l_nullptr;
        event->data.line.next = l_nullptr;
        event->data.line.previous = l_nullptr;
        break;
      case BEGIN_FRAME:
      case END_FRAME:
        event->data.frame.frame = 0;
        break;
      case BEGIN_ROUTINE:
      case END_ROUTINE:
      case ENTER_SCOPE:
      case EXIT_SCOPE:
      case IGNORE_SCOPE:
        event->data.event.flags = 0;
        event->data.event.info = l_nullptr;
        event->data.event.sibling = l_nullptr;
        event->data.event.lines = l_nullptr;
        break;
      default:
        break;
    }
  }

  lmprof_free(alloc, l_pcast(void *, page), TRACE_EVENT_PAGE_SIZE); /* Deallocate page */
}

LUA_API TraceEventTimeline *timeline_new(lmprof_Alloc *alloc, size_t pageLimit) {
  TraceEventTimeline *list = l_pcast(TraceEventTimeline *, lmprof_malloc(alloc, sizeof(TraceEventTimeline)));
  if (list != l_nullptr) {
    TraceEventPage *header = traceevent_new(alloc);
    if (header != l_nullptr) {
      list->page_allocator = alloc;
      list->pageCount = 0;
      list->pageLimit = pageLimit / TRACE_EVENT_PAGE_SIZE;
      list->frameCount = 1;
      list->head = list->curr = header;
      return list;
    }
    else { /* Could not allocate page header */
      lmprof_free(alloc, l_pcast(void *, list), sizeof(TraceEventTimeline));
      return l_nullptr;
    }
  }
  return l_nullptr;
}

LUA_API void timeline_free(TraceEventTimeline *list) {
  lmprof_Alloc *alloc = list->page_allocator;

  TraceEventPage *page = list->head;
  while (page != l_nullptr) {
    TraceEventPage *next = page->next;
    traceevent_free(alloc, page);
    page = next;
  }

  lmprof_free(alloc, l_pcast(void *, list), sizeof(TraceEventTimeline));
}

LUA_API size_t timeline_page_size(void) {
  return TRACE_EVENT_PAGE_SIZE;
}

LUA_API size_t timeline_event_array_size(void) {
  return TRACE_EVENT_SIZE_ARRAY;
}

LUA_API int timeline_canbuffer(const TraceEventTimeline *list, size_t n) {
  if (list->pageLimit != 0) { /* Remaining in page + remaining pages to allocate */
    const size_t p_avail = TRACE_EVENT_SIZE_ARRAY - list->curr->count;
    const size_t s_avail = TRACE_EVENT_SIZE_ARRAY * (list->pageLimit - list->pageCount - 1);
    return (p_avail + s_avail) <= n;
  }
  return 1; /* Infinite paging */
}

LUA_API double timeline_usage(const TraceEventTimeline *list) {
  if (list->pageCount == 0 || list->pageLimit == 0)
    return 0.0;
  else {
    const double uniform = 1.0 / (double)list->pageLimit;
    const double result = uniform * ((double)list->pageCount - 1);
    const double page = ((double)list->curr->count) / ((double)TRACE_EVENT_SIZE_ARRAY);
    return result + uniform * page;
  }
}

LUA_API void timeline_foreach(TraceEventTimeline *list, TraceEventIterator cb, void *args) {
  TraceEventPage *page = l_nullptr;
  for (page = list->head; page != l_nullptr; page = page->next) {
    size_t i;
    for (i = 0; i < page->count; ++i)
      cb(list, &page->event_array[i], args);
  }
}

/* }================================================================== */

/*
** {==================================================================
** TraceEvent: Event Collection
** ===================================================================
*/

#define FETCHPAGE(L, E)                               \
  TraceEvent *E;                                      \
  if ((event = timeline_allocpage((L))) == l_nullptr) \
    return TRACE_EVENT_ERRPAGEFULL;

/* Create a new ProfilerEvent node. */
static TraceEvent *timeline_allocpage(TraceEventTimeline *list) {
  TraceEventPage *page = list->curr;
  if (page->count == TRACE_EVENT_SIZE_ARRAY) { /* no more available events within the page */
    if (page->next != l_nullptr) { /* use recycled page */
      page = page->next;
      page->count = 0;

      list->curr = page;
    }
    /* attempt to allocate additional page */
    else if ((list->pageLimit == 0 || list->pageCount < list->pageLimit) && (page->next = traceevent_new(list->page_allocator))) {
      page = page->next;
      page->count = 0;

      list->curr = page;
      list->pageCount++;
    }
    else {
      return l_nullptr; /* failure to allocate */
    }
  }

  return &page->event_array[page->count++];
}

LUA_API int traceevent_metadata_process(TraceEventTimeline *list, lua_Integer process, const char *name) {
  FETCHPAGE(list, event);
  event->op = PROCESS;
  event->call.proc.pid = process;
  event->call.proc.tid = LMPROF_THREAD_BROWSER;
  event->data.process.nameLen = strlen(name);
  event->data.process.name = lmprof_strdup(list->page_allocator, name, event->data.process.nameLen);
  return TRACE_EVENT_OK;
}

LUA_API int traceevent_metadata_thread(TraceEventTimeline *list, lmprof_EventProcess process, const char *name) {
  FETCHPAGE(list, event);
  event->op = THREAD;
  event->call.proc = process;
  event->data.process.nameLen = strlen(name);
  event->data.process.name = lmprof_strdup(list->page_allocator, name, event->data.process.nameLen);
  return TRACE_EVENT_OK;
}

LUA_API int traceevent_beginframe(TraceEventTimeline *list, lmprof_EventMeasurement unit) {
  FETCHPAGE(list, event);
  event->op = BEGIN_FRAME;
  event->call = unit;
  event->data.frame.frame = ++list->frameCount;
  return TRACE_EVENT_OK;
}

LUA_API int traceevent_endframe(TraceEventTimeline *list, lmprof_EventMeasurement unit) {
  FETCHPAGE(list, event);
  event->op = END_FRAME;
  event->call = unit;
  event->data.frame.frame = list->frameCount;
  return TRACE_EVENT_OK;
}

LUA_API int traceevent_beginroutine(TraceEventTimeline *list, lmprof_EventMeasurement unit) {
  FETCHPAGE(list, event);
  event->op = BEGIN_ROUTINE;
  event->call = unit;
  event->data.event.flags = 0;
  event->data.event.info = l_nullptr;
  event->data.event.sibling = l_nullptr;
  event->data.event.lines = l_nullptr;
  return TRACE_EVENT_OK;
}

LUA_API int traceevent_endroutine(TraceEventTimeline *list, lmprof_EventMeasurement unit) {
  FETCHPAGE(list, event);
  event->op = END_ROUTINE;
  event->call = unit;
  event->data.event.flags = 0;
  event->data.event.info = l_nullptr;
  event->data.event.sibling = l_nullptr;
  event->data.event.lines = l_nullptr;
  return TRACE_EVENT_OK;
}

LUA_API int traceevent_enterscope(TraceEventTimeline *list, TraceEventStackInstance *inst) {
  FETCHPAGE(list, event);
  event->op = ENTER_SCOPE;
  event->call = inst->call;
  event->data.event.info = &inst->record->info;
  event->data.event.flags = 0;
  event->data.event.sibling = l_nullptr;
  event->data.event.lines = l_nullptr;

  inst->begin_event = event;
  return TRACE_EVENT_OK;
}

LUA_API int traceevent_exitscope(TraceEventTimeline *list, TraceEventStackInstance *inst) {
  FETCHPAGE(list, event);
  event->op = EXIT_SCOPE;
  event->call = inst->call;
  event->data.event.info = &inst->record->info;
  event->data.event.flags = 0;
  event->data.event.sibling = l_nullptr;
  event->data.event.lines = l_nullptr;

  if (inst->begin_event != l_nullptr) {
    inst->begin_event->data.event.sibling = event;
    event->data.event.sibling = inst->begin_event;
  }
  return TRACE_EVENT_OK;
}

LUA_API int traceevent_sample(TraceEventTimeline *list, TraceEventStackInstance *inst, lmprof_EventMeasurement unit, int line) {
  FETCHPAGE(list, event);
  if (inst->begin_event == l_nullptr) {
    LMPROF_LOG("No event sibling for sample!\n");
    return TRACE_EVENT_OK;
  }

  if (line == -1) {
    event->op = SAMPLE_EVENT;
    event->call = unit;
    event->data.sample.next = l_nullptr;
  }
  else {
    TraceEvent *lines = inst->begin_event->data.event.lines;
    event->op = LINE_SCOPE;
    event->call = unit;
    event->data.line.line = line;
    event->data.line.info = &inst->record->info;
    event->data.line.previous = lines;
    event->data.line.next = l_nullptr;

    /* Update lines list */
    if (lines != l_nullptr)
      lines->data.line.next = event;
    inst->begin_event->data.event.lines = event;
  }
  return TRACE_EVENT_OK;
}

LUA_API void timeline_adjust(TraceEventTimeline *list) {
  const lu_time base = list->baseTime;
#if LMPROF_HAS_LOGGER
  lu_time last = 0;
#endif

  TraceEventPage *page = l_nullptr;
  for (page = list->head; page != l_nullptr; page = page->next) {
    size_t i;
    for (i = 0; i < page->count; ++i) {
      TraceEvent *event = &page->event_array[i];
      if (op_adjust(event->op)) {
        lu_time time = event->call.s.time;
#if LMPROF_HAS_LOGGER
        if (base > time) {
          const char *name = LMPROF_RECORD_NAME_UNKNOWN;
          if (op_event(event->op))
            name = LMPROF_RECORD_NAME(event->data.event.info->source, LMPROF_RECORD_NAME_UNKNOWN);
          LMPROF_LOG("Incorrect base time: %s %" PRIluTIME " %" PRIluTIME "\n", name, LU_TIME_MICRO(time), LU_TIME_MICRO(last));
        }
#endif

        time -= base;
        time -= event->call.overhead;

        /*
        ** Ensure times are strictly increasing after taking into account
        ** overhead and base-time adjustments.
        */
#if LMPROF_HAS_LOGGER
        if (time < last) {
          const char *name = LMPROF_RECORD_NAME_UNKNOWN;
          if (op_event(event->op))
            name = LMPROF_RECORD_NAME(event->data.event.info->source, LMPROF_RECORD_NAME_UNKNOWN);
          LMPROF_LOG("Time not strictly increasing: %d %s %" PRIluTIME " %" PRIluTIME "\n", event->op, name, LU_TIME_MICRO(time), LU_TIME_MICRO(last));
        }
        last = time;
#endif
        event->call.s.time = time;
      }
    }
  }
}

/* Simplification */
#define INCLUDE_PROCESS(E, O) ((O).id.pid == 0 || (E)->call.proc.pid == (O).id.pid)
#define INCLUDE_THREAD(E, O) (INCLUDE_PROCESS(E, O) && ((O).id.tid == 0 || (E)->call.proc.tid == (O).id.tid))
#define INCLUDE_DURATION(D, O) ((O).threshold == 0 || ((D) >= (O).threshold))

LUA_API int timeline_compress(TraceEventTimeline *list, TraceEventCompressOpts opts) {
  TraceEventPage *page = list->head;
  for (page = list->head; page != l_nullptr; page = page->next) {
    size_t i;
    for (i = 0; i < page->count; ++i) {
      TraceEvent *event = &page->event_array[i];
      if (!INCLUDE_THREAD(event, opts))
        continue;
      else if (event->op == ENTER_SCOPE && event->data.event.sibling != l_nullptr) {
        TraceEvent *sibling = event->data.event.sibling;
        const lu_time delta_time = sibling->call.s.time - event->call.s.time;
        const int ignore = BITFIELD_TEST(event->data.event.info->event, LMPROF_RECORD_IGNORED)
                           || !INCLUDE_DURATION(delta_time, opts);

        /*
        ** @TODO: If non-meta TraceEvents ever allocate their own data, this
        ** logic must change (i.e., IGNORE_SCOPE becomes a flag).
        */
        if (ignore) {
          TraceEvent *tail;
          event->op = sibling->op = IGNORE_SCOPE;
          for (tail = event->data.event.lines; tail != l_nullptr; tail = tail->data.line.previous) {
            tail->op = IGNORE_SCOPE;
          }
        }
      }
    }
  }

  return TRACE_EVENT_OK;
}

/* }================================================================== */
