#ifndef NGCD_TARGET_PTHREAD_H
#define NGCD_TARGET_PTHREAD_H

#include <stddef.h>

typedef unsigned long pthread_t;

typedef union {
    unsigned char bytes[48];
    long alignment;
} pthread_mutex_t;

typedef union {
    unsigned char bytes[48];
    long long alignment;
} pthread_cond_t;

extern int pthread_create(pthread_t *thread, const void *attributes,
                          void *(*function)(void *), void *argument);
extern int pthread_join(pthread_t thread, void **result);
extern int pthread_mutex_init(pthread_mutex_t *mutex, const void *attributes);
extern int pthread_mutex_destroy(pthread_mutex_t *mutex);
extern int pthread_mutex_lock(pthread_mutex_t *mutex);
extern int pthread_mutex_unlock(pthread_mutex_t *mutex);
extern int pthread_cond_init(pthread_cond_t *condition, const void *attributes);
extern int pthread_cond_destroy(pthread_cond_t *condition);
extern int pthread_cond_wait(pthread_cond_t *condition,
                             pthread_mutex_t *mutex);
extern int pthread_cond_broadcast(pthread_cond_t *condition);

#endif
