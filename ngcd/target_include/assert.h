#ifndef NGCD_TARGET_ASSERT_H
#define NGCD_TARGET_ASSERT_H

#ifndef assert
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
extern void __assert_fail(const char *expression, const char *file,
                          unsigned int line, const char *function);
#define assert(expression)                                                   \
    ((expression) ? (void)0                                                  \
                  : __assert_fail(#expression, __FILE__, __LINE__, __func__))
#endif
#endif

#endif
