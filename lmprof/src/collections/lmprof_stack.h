/*
** $Id: lmprof_stack.h $
** Profiling Stack
** See Copyright Notice in lmprof_lib.h
*/
#ifndef lmprof_stack_h
#define lmprof_stack_h

#include "../lmprof_conf.h"

#include "lmprof_record.h"
#include "lmprof_traceevent.h"

/*
@@ LMPROF_MAXSTACK limits the size of the profiler stack. An arbitrary limit
** intended to (1) be a pre-configured one-time allocation instead of requiring
** additional logic for growing/shrinking the stack; and (2) limit stack space,
** as the profiler can be configured to create one stack instance per coroutine.
*/
#if !defined(LMPROF_MAXSTACK)
#define LMPROF_MAXSTACK 1024 /* (LUAI_MAXSTACK >> 8) */
#endif

typedef struct lmprof_StackInst {
  char tail_call; /* Whether the activation record is a tail call */
  int last_line; /* Last 'currentline' value when LUA_HOOKLINE is enabled */
  size_t last_line_instructions; /* Instruction count on last_line update */
  union {
    struct {
      lmprof_Record *record; /* Record for inlined stats updating */
      lu_time overhead; /* Total accumulated error/profiling overhead */
      lmprof_EventUnit node; /* Function measurement */
      lmprof_EventUnit path; /* Totality of function & child measurements */
    } graph;
    TraceEventStackInstance trace; /* Previous name: callFrame */
  };
} lmprof_StackInst;

typedef struct lmprof_Stack {
  char callback_api; /* Using TraceEvent profiling convention */
  lua_Integer thread_identifier; /* Unique identifier associated with this call stack */
  /*
  ** LUA_HOOKCOUNT: Number of instructions executed on the given thread. Note,
  ** if mask_count is greater than 'one' this value will only be exact within
  ** modulo mask_count instructions.
  */
  size_t instr_count; /* Number of profiler instructions */
  lu_time instr_last; /* Time of last call. */

  size_t head; /* First available stack index */
  size_t size; /* Size of the stack array */
  lmprof_StackInst stack[1]; /* Profile stack */
} lmprof_Stack;

/* Creates and pushes on the stack a new lmprof_Stack full-userdata. */
LUA_API lmprof_Stack *lmprof_stack_new(lua_State *L, lua_Integer id, char callback_api);

/* Allocate a lmprof_Stack instance; intended to be used as a Lua light userdata. */
LUA_API lmprof_Stack *lmprof_stack_light_new(lmprof_Alloc *alloc, lua_Integer id, char callback_api);

/* Free an allocated lmprof_Stack (light userdata) instance. */
LUA_API void lmprof_stack_light_free(lmprof_Alloc *alloc, lmprof_Stack *stack);

/* Removes and sanitizes (stack_clear_instance) all stack instances from the Stack. */
LUA_API void lmprof_stack_clear(lmprof_Stack *s);

/* Zero out all data associated with a stack instance */
LUA_API void stack_clear_instance(lmprof_Stack *s, lmprof_StackInst *inst);

/*
** {==================================================================
** Stack Operations
** ===================================================================
*/

/* Iterator interface */
typedef int (*lmprof_stack_Callback)(lua_State *, lmprof_StackInst *, const void *);

/* Return the number of elements contained in the stack. */
static LUA_INLINE size_t lmprof_stack_size(const lmprof_Stack *s) {
  return s->head;
}

/* Return the stack instance at the top of the stack without removing it. */
static LUA_INLINE lmprof_StackInst *lmprof_stack_peek(lmprof_Stack *s) {
  return (s->head > 0) ? &s->stack[s->head - 1] : l_nullptr;
}

/* Return the stack instance below the top of the stack without removing it. */
static LUA_INLINE lmprof_StackInst *lmprof_stack_parent(lmprof_Stack *s) {
  return (s->head > 1) ? &s->stack[s->head - 2] : l_nullptr;
}

/* Reserve the next stack instance */
static LUA_INLINE lmprof_StackInst *lmprof_stack_next(lmprof_Stack *s, char tail) {
  lmprof_StackInst *inst = l_nullptr;
  if (s->head < s->size) {
    inst = &s->stack[s->head++];
    inst->tail_call = tail;
  }
  return inst;
}

/* Remove and return the stack instance at the top of the stack. */
static LUA_INLINE lmprof_StackInst *lmprof_stack_pop(lmprof_Stack *s) {
  return (s->head > 0) ? &s->stack[--s->head] : l_nullptr;
}

/*
** Traverse the stack from top (most recently pushed) to bottom (least recently
** pushed), invoking "cb" for each stack instance in the stack.
*/
static LUA_INLINE void lmprof_stack_foreach(lua_State *L, lmprof_Stack *s, lmprof_stack_Callback cb, const void *args) {
  size_t i;
  for (i = s->head; i > 0; --i) {
    if (cb(L, &s->stack[i - 1], args) != LUA_OK)
      break;
  }
}

/* }================================================================== */

/*
** {==================================================================
** Graph Measurements
** ===================================================================
*/

/*
** Inserts a function record at the top of the Stack.
**
** PARAMS:
**  record - function record being profiled.
**  unit - current Lua state (amount allocated, deallocated, current time)
**  process - thread & process identifiers, corresponding to function ownership.
**  tail - whether this profiled function corresponds to a Lua tailcall.
**
** RETURNS:
**  1 on success, 0 on failure (profile state exceeds size of stack)
*/
LUA_API lmprof_StackInst *lmprof_stack_measured_push(lmprof_Stack *s, lmprof_Record *record, lmprof_EventUnit *unit, char tail);

/*
** Remove the stack instance at the top of this stack at a given unit (allocator
** use and current time). If available this function will increment/update the
** "measure.path" field of the new 'top' element of the stack.
**
** A: The *total* execution time (and amount allocated) is equal to the
**    difference of lmprof_stack_measured_push and lmprof_stack_measured_pop
**    events.
**
** B: The *total* time (and amount allocated) spent within the functions
**    children is stored in "measure.path".
**
** The difference between (A) and (B) is equal to the 'self' execution time (and
** amount allocated) of the function, i.e., the total amount of time the
** instance was on top of the stack.
**
** @NOTE This function does not sanitize input, and should not be called with an
**  empty stack.
*/
LUA_API lmprof_StackInst *lmprof_stack_measured_pop(lmprof_Stack *s, lmprof_EventUnit *unit);

/* Push a TraceEvent instance onto the stack. */
static LUA_INLINE lmprof_StackInst *lmprof_stack_event_push(lmprof_Stack *s, lmprof_Record *record, lmprof_EventMeasurement *unit, char tail) {
  lmprof_StackInst *inst;
  if ((inst = lmprof_stack_next(s, tail)) != l_nullptr) {
    inst->trace.call = *unit;
    inst->trace.record = record;
  }
  return inst;
}

/* }================================================================== */

#endif
