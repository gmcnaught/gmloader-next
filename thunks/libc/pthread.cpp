#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   /* cpu_set_t / CPU_SET / pthread_setaffinity_np */
#endif

#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdint.h>
#include <semaphore.h>
#include <unistd.h>

#include "platform.h"
#include "thunk_pthread.h"

ABI_ATTR pthread_t pthread_self_impl()
{
    return pthread_self();
}

ABI_ATTR int pthread_key_create_impl(pthread_key_t *key, void (*destr_function) (void *))
{
    int ret = pthread_key_create(key, destr_function);
    return ret;
}

ABI_ATTR int pthread_key_delete_impl(pthread_key_t key)
{
    return pthread_key_delete(key);
}

ABI_ATTR int pthread_setspecific_impl(pthread_key_t key, const void *__pointer)
{
    // TODO:: Investigate if this is valid - currently this works around Splash setting the zero key and causing crashes elsewhere.
    if (key == 0)
        return -EINVAL;

    int ret = pthread_setspecific(key, __pointer);
    return ret;
}

ABI_ATTR void* pthread_getspecific_impl(pthread_key_t key)
{
    return pthread_getspecific(key);
}

ABI_ATTR int pthread_mutex_init_impl(BIONIC_pthread_mutex_t *_uid, pthread_mutexattr_t **mutexattr)
{
    pthread_mutex_t **uid = (pthread_mutex_t**)_uid;

    pthread_mutexattr_t *attr = mutexattr ? *mutexattr : NULL; 
    pthread_mutex_t *m = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
    *m = PTHREAD_MUTEX_INITIALIZER;
    if (!m)
        return -1;

    int ret = pthread_mutex_init(m, attr);
    if (ret < 0)
    {
        free(m);
        return -1;
    }

    *uid = m;

    return 0;
}

ABI_ATTR int pthread_mutex_destroy_impl(BIONIC_pthread_mutex_t *_uid)
{
    pthread_mutex_t **uid = (pthread_mutex_t**)_uid;

    if (uid && *uid && (uintptr_t)*uid > 0x8000)
    {
        pthread_mutex_destroy(*uid);
        free(*uid);
        *uid = NULL;
    }
    return 0;
}

ABI_ATTR int pthread_mutex_lock_impl(BIONIC_pthread_mutex_t *_uid)
{
    pthread_mutex_t **uid = (pthread_mutex_t**)_uid;

    if (uid < (pthread_mutex_t**)0x1000)
        return -1;

    if (!*uid)
        pthread_mutex_init_impl(_uid, NULL);
    
    return pthread_mutex_lock(*uid);
}

ABI_ATTR int pthread_mutex_unlock_impl(BIONIC_pthread_mutex_t *_uid)
{
    pthread_mutex_t **uid = (pthread_mutex_t**)_uid;

    int ret = 0;
    if (uid < (pthread_mutex_t**)0x1000)
        return -1;

    if (!*uid)
        ret = pthread_mutex_init_impl(_uid, NULL);

    if (ret < 0)
        return ret;
    
    return pthread_mutex_unlock(*uid);
}

ABI_ATTR int pthread_cond_init_impl(pthread_cond_t **cnd, const int *condattr)
{
    pthread_cond_t *c = (pthread_cond_t *)calloc(1, sizeof(pthread_cond_t));
    if (!c)
        return -1;

    *c = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
    int ret = pthread_cond_init(c, NULL);
    if (ret < 0)
    {
        free(c);
        return -1;
    }

    *cnd = c;

    return 0;
}

ABI_ATTR int pthread_cond_broadcast_impl(pthread_cond_t **cnd)
{
    if (!*cnd)
    {
        if (pthread_cond_init_impl(cnd, NULL) < 0)
            return -1;
    }
    return pthread_cond_broadcast(*cnd);
}

ABI_ATTR int pthread_cond_signal_impl(pthread_cond_t **cnd)
{
    if (!*cnd)
    {
        if (pthread_cond_init_impl(cnd, NULL) < 0)
            return -1;
    };
    return pthread_cond_signal(*cnd);
}

ABI_ATTR int pthread_cond_destroy_impl(pthread_cond_t **cnd)
{
    if (cnd && *cnd)
    {
        pthread_cond_destroy(*cnd);
        free(*cnd);
        *cnd = NULL;
    }
    return 0;
}

ABI_ATTR int pthread_cond_wait_impl(pthread_cond_t **cnd, BIONIC_pthread_mutex_t *_mtx)
{
    pthread_mutex_t **mtx = (pthread_mutex_t**)_mtx;
    
    if (!*cnd)
    {
        if (pthread_cond_init_impl(cnd, NULL) < 0)
            return -1;
    }
    return pthread_cond_wait(*cnd, *mtx);
}

ABI_ATTR int pthread_cond_timedwait_impl(pthread_cond_t **cnd, BIONIC_pthread_mutex_t *_mtx, const struct timespec *t)
{
    pthread_mutex_t **mtx = (pthread_mutex_t**)_mtx;

    if (!*cnd)
    {
        if (pthread_cond_init_impl(cnd, NULL) < 0)
            return -1;
    }
    return pthread_cond_timedwait(*cnd, *mtx, t);
}

ABI_ATTR int pthread_once_impl(volatile int *once_control, void (*init_routine)(void))
{
    if (!once_control || !init_routine)
        return -1;
    if (__sync_lock_test_and_set(once_control, 1) == 0)
        (*init_routine)();
    return 0;
}

// pthread_t is an unsigned int, so it should be fine
// TODO: probably shouldn't assume default attributes
ABI_ATTR int pthread_create_impl(pthread_t *thread, const void *unused, void *(*entry)(void *), void *arg)
{
    int rc = pthread_create(thread, NULL, entry, arg);
#ifdef __linux__
    // Linux gives a new thread its creator's CPU affinity mask. On MiSTer the
    // main thread is pinned to core 0 (mister_native_audio.cpp) so the fabric
    // backend's C_DONE spin cannot starve the audio pump on core 1 -- but that
    // pin is inherited by every thread the guest creates from the main thread,
    // including OpenAL Soft's mixer thread, which then has to share one core
    // with a loop that spins. Give guest threads the whole machine back; the
    // pin exists to place OUR two threads, not to confine the runner's.
    if (rc == 0) {
        cpu_set_t all;
        CPU_ZERO(&all);
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;
        if (n > CPU_SETSIZE) n = CPU_SETSIZE;
        for (long i = 0; i < n; ++i) CPU_SET((int)i, &all);
        pthread_setaffinity_np(*thread, sizeof(all), &all);
    }
#endif
    return rc;
}

ABI_ATTR int pthread_mutexattr_init_impl(pthread_mutexattr_t **attr_ptr)
{
    pthread_mutexattr_t *attr = (pthread_mutexattr_t*)calloc(1, sizeof(pthread_mutexattr_t));
    int r = pthread_mutexattr_init(attr);
    *attr_ptr = attr;

    return r;
}

ABI_ATTR int pthread_mutexattr_settype_impl(pthread_mutexattr_t **attr_ptr, int kind)
{
    int ret = pthread_mutexattr_settype(*attr_ptr, kind);
    return ret;
}

ABI_ATTR int pthread_mutexattr_destroy_impl(pthread_mutexattr_t **attr_ptr)
{
    int ret = pthread_mutexattr_destroy(*attr_ptr);
    return ret;
    //free(attr_ptr);
}

ABI_ATTR int pthread_join_impl(pthread_t th, void **thread_return)
{
    return pthread_join(th, thread_return);
}

/* Return the previously set address for the stack.  */
ABI_ATTR int pthread_attr_getstackaddr_impl (const pthread_attr_t *attr, void **stackaddr)
{
    size_t size;
    return pthread_attr_getstack(attr, stackaddr, &size);
}

/* Set the starting address of the stack of the thread to be created.
   Depending on whether the stack grows up or down the value must either
   be higher or lower than all the address in the memory block.  The
   minimal size of the block must be PTHREAD_STACK_MIN.  */
ABI_ATTR int pthread_attr_setstackaddr_impl (pthread_attr_t *attr, void *stackaddr)
{
    size_t size;
    pthread_attr_getstacksize(attr, &size); /* lets assume stack size didnt change... */
    return pthread_attr_setstack(attr, stackaddr, size);
}
