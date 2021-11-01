// vim: set fileencoding=utf-8
#include <stdlib.h>
#include <math.h>
#include "naomi/thread.h"

void *basic_thread(void *param)
{
    global_counter_increment(param);

    return (void *)thread_id() + 1000;
}

void test_threads_basic(test_context_t *context)
{
    void *counter = global_counter_init(0);
    uint32_t thread = thread_create("test", basic_thread, counter);

    ASSERT(thread != thread_id(), "Newly created thread has same ID as us?");

    thread_info_t info = thread_info(thread);

    ASSERT(strcmp(info.name, "test") == 0, "Newly created thread has invalid debug name!");
    ASSERT(info.priority == 0, "Newly created thread has wrong default priority!");
    ASSERT(info.alive == 1, "Newly created thread isn't alive!");
    ASSERT(info.running == 0, "Newly created thread is running already!");

    // Start the thread, wait until its done.
    thread_start(thread);
    uint32_t expected_id = (uint32_t)thread_join(thread);

    ASSERT(global_counter_value(counter) == 1, "Thread did not increment global counter!");
    ASSERT(expected_id == (thread + 1000), "Thread did not return correct value!");

    // Finally, give back the memory.
    thread_destroy(thread);
    global_counter_free(counter);
}

void *semaphore_thread(void *param)
{
    int profile = profile_start();
    semaphore_t *semaphore = param;

    semaphore_acquire(semaphore);

    unsigned int duration = profile_end(profile);

    for (volatile unsigned int i = 0; i < 1000000; i++) { ; }

    semaphore_release(semaphore);

    return (void *)duration;
}

void test_threads_semaphore(test_context_t *context)
{
    semaphore_t semaphore;
    semaphore_init(&semaphore, 2);

    uint32_t threads[5];
    unsigned int returns[5];
    unsigned int counts[4] = { 0, 0, 0, 0 };
    threads[0] = thread_create("test1", semaphore_thread, &semaphore);
    threads[1] = thread_create("test2", semaphore_thread, &semaphore);
    threads[2] = thread_create("test3", semaphore_thread, &semaphore);
    threads[3] = thread_create("test4", semaphore_thread, &semaphore);
    threads[4] = thread_create("test5", semaphore_thread, &semaphore);

    for(unsigned int i = 0; i < (sizeof(threads) / sizeof(threads[0])); i++)
    {
        thread_start(threads[i]);
    }

    for(unsigned int i = 0; i < (sizeof(threads) / sizeof(threads[0])); i++)
    {
        returns[i] = (uint32_t)thread_join(threads[i]);
    }

    unsigned int max_wait = 0;
    for(unsigned int i = 0; i < (sizeof(threads) / sizeof(threads[0])); i++)
    {
        max_wait = max_wait > returns[i] ? max_wait : returns[i];
    }

    for(unsigned int i = 0; i < (sizeof(threads) / sizeof(threads[0])); i++)
    {
        int bucket = (int)round(((double)returns[i] / (double)max_wait) * 2.0);
        if (bucket >= 0 && bucket <= 2)
        {
            counts[bucket]++;
        }
        else
        {
            counts[3]++;
        }
    }

    // Should have had two threads that waited no time.
    ASSERT(counts[0] == 2, "Unexpected number of threads %d that got semaphore immediately!", counts[0]);

    // Should have had two threads that waited for the first two threads to finish.
    ASSERT(counts[1] == 2, "Unexpected number of threads %d that got semaphore immediately!", counts[1]);

    // Should have had one thread that waited for the first two threads and the next two threads to finish.
    ASSERT(counts[2] == 1, "Unexpected number of threads %d that got semaphore immediately!", counts[2]);

    // Should have had no other buckets filled.
    ASSERT(counts[3] == 0, "Unexpected number of threads %d that got bizarre timing!", counts[3]);

    semaphore_free(&semaphore);
}