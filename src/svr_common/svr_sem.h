#pragma once
#include "svr_common.h"

// User level semaphore.
// Based on "Creating a semaphore from WaitOnAddress" https://devblogs.microsoft.com/oldnewthing/20170612-00/?p=96375
// Was faster in testing and also useful being able to see and reset the semaphore count.

struct SvrSemaphore
{
    s32 count;
    s32 max_count;
};

void svr_sem_init(SvrSemaphore* sem, s32 init_count, s32 max_count);
void svr_sem_release(SvrSemaphore* sem);
void svr_sem_wait(SvrSemaphore* sem);
