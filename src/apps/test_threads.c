/*
 * test_threads - AIOS pthreads smoke test
 *
 * Creates 2 worker threads, each increments a mutex-protected counter 1000x.
 * Expected result: shared == 2000.
 */
#include <stdio.h>
#include <pthread.h>

static int shared = 0;
static pthread_mutex_t mtx;

static void *worker(void *arg) {
    int id = (int)(long)arg;
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&mtx);
        shared++;
        pthread_mutex_unlock(&mtx);
    }
    printf("[thread %d] done, shared=%d\n", id, shared);
    return NULL;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    printf("=== AIOS pthreads test ===\n");
    pthread_mutex_init(&mtx, NULL);

    pthread_t t1, t2;
    printf("Creating thread 1...\n");
    int r1 = pthread_create(&t1, NULL, worker, (void *)1);
    if (r1) { printf("FAIL: pthread_create t1: %d\n", r1); return 1; }

    printf("Creating thread 2...\n");
    int r2 = pthread_create(&t2, NULL, worker, (void *)2);
    if (r2) { printf("FAIL: pthread_create t2: %d\n", r2); return 1; }

    printf("Joining thread 1...\n");
    pthread_join(t1, NULL);
    printf("Joining thread 2...\n");
    pthread_join(t2, NULL);

    printf("Final shared=%d (expected 2000)\n", shared);
    if (shared == 2000)
        printf("PASS\n");
    else
        printf("FAIL: race condition or error\n");

    pthread_mutex_destroy(&mtx);
    return 0;
}
