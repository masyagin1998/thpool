#include "thpool.h"

#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <assert.h>

void task(void *arg)
{
	printf("Thread #%u working on %d\n", (int)pthread_self(), (int) arg);
}


int main()
{
	puts("Making threadpool with 4 threads");
	struct THPOOL thpool;
	int r = thpool_configure(&thpool, 4);
	assert(r == 0);

	puts("Adding 40 tasks to threadpool");
	int i;
	for (i = 0; i < 40; i++){
		thpool_add_work(&thpool, task, (void*)(uintptr_t)i);
	};

	thpool_wait(&thpool);
	puts("Killing threadpool");
	thpool_reset(&thpool);

	sleep(1);

	return 0;
}
