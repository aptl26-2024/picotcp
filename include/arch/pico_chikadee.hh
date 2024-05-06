#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>

#ifndef PICO_SUPPORT_CHIKADEE_HH
#define PICO_SUPPORT_CHIKADEE_HH

#include "../../../../k-alloc.hh"

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


#endif

#ifdef __cplusplus
}
#endif