#ifdef __cplusplus
extern "C" {
#endif

#include <lib.hh>

#define dbg(...) do {} while(0)
#define PICO_IDLE() do {} while(0)

#ifndef PICO_SUPPORT_CHIKADEE_HH
#define PICO_SUPPORT_CHIKADEE_HH

#include "k-alloc.hh"

static inline void *pico_zalloc(int x)
{
    void* ptr = kalloc(x);
    if (ptr) {
        memset(ptr, 0x00, x);
    }
    return ptr;
}


static inline void pico_free(void *x)
{
    kfree(x);
}

static inline unsigned long PICO_TIME_MS(void)
{
    return ticks;
}

static inline unsigned long PICO_TIME(void)
{
    return (PICO_TIME_MS() / 1000);
}


#endif

#ifdef __cplusplus
}
#endif