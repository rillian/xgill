
#ifndef SIXGILL_H
#define SIXGILL_H 

/******************************************************************************
 * Macros for static assertion annotations. These are used by the sixgill tool;
 * see sixgill.org for descriptions of these macros.
 * When the tool is not running these macros are no-ops.
 *****************************************************************************/

#ifdef XGILL_PLUGIN

#define precondition(COND)         __attribute__((precondition(#COND)))
#define precondition_assume(COND)  __attribute__((precondition_assume(#COND)))
#define postcondition(COND)        __attribute__((postcondition(#COND)))
#define postcondition_assume(COND) __attribute__((postcondition_assume(#COND)))
#define invariant(COND)            __attribute__((invariant(#COND)))
#define invariant_assume(COND)     __attribute__((invariant_assume(#COND)))

/* Used to make identifiers for assert/assume annotations in a function. */
#define static_paste2(X,Y) X ## Y
#define static_paste1(X,Y) static_paste2(X,Y)

#define static_assert(COND)                          \
  do {                                               \
    __attribute__((static_assert(#COND), unused))    \
    int static_paste1(static_assert_, __COUNTER__);  \
  } while (0)

#define static_assume(COND)                          \
  do {                                               \
    __attribute__((static_assume(#COND), unused))    \
    int static_paste1(static_assume_, __COUNTER__);  \
  } while (0)

#define static_assert_runtime(COND)                         \
  do {                                                      \
    __attribute__((static_assert_runtime(#COND), unused))   \
    int static_paste1(static_assert_runtime_, __COUNTER__); \
  } while (0)

#else /* XGILL_PLUGIN */

#define precondition(COND)          /* nothing */
#define precondition_assume(COND)   /* nothing */
#define postcondition(COND)         /* nothing */
#define postcondition_assume(COND)  /* nothing */
#define invariant(COND)             /* nothing */
#define invariant_assume(COND)      /* nothing */

#define static_assert(COND)          do { /* nothing */ } while (0)
#define static_assume(COND)          do { /* nothing */ } while (0)
#define static_assert_runtime(COND)  do { /* nothing */ } while (0)

#endif /* XGILL_PLUGIN */

#endif /* SIXGILL_H */
