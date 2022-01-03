/*
	Simple thread pool.
*/

#ifndef THPOOL_H
#define THPOOL_H

#ifdef __cplusplus
extern "C" {
#endif	/* __cplusplus */

#include <pthread.h>

struct THPOOL_THREAD
{
	int id;
	pthread_t pthread;
	struct THPOOL* thp;
};

struct THPOOL_BSEM
{
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int v;
};

struct THPOOL_JOB
{
	struct THPOOL_JOB*	prev;
	void (*function)(void* arg);
	void* arg;
};

struct THPOOL_JOBS_QUEUE
{
	pthread_mutex_t rwmutex;

	struct THPOOL_JOB* front;
	struct THPOOL_JOB* rear;

	struct THPOOL_BSEM* has_jobs;

	size_t len;
};

struct THPOOL
{
	struct THPOOL_THREAD** threads;

	volatile size_t n_threads_alive;
	volatile size_t n_threads_working;

	pthread_mutex_t	thcount_lock;
	pthread_cond_t threads_all_idle;

	struct THPOOL_JOBS_QUEUE jobs_queue;
};

struct THPOOL* thpool_new();
int thpool_configure(struct THPOOL* thp, size_t nths);
int thpool_reset(struct THPOOL* thp);
void thpool_free(struct THPOOL* thp);

int thpool_add_work(struct THPOOL* thp, void (*function)(void*), void* arg);

void thpool_wait(struct THPOOL* thp);

void thpool_pause(struct THPOOL* thp);
void thpool_resume(struct THPOOL* thp);

size_t thpool_get_busy_threads_count(struct THPOOL* thp);

#ifdef __cplusplus
}
#endif	/* __cplusplus */

#endif	/* THPOOL_H */
