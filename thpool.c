#include "thpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <signal.h>

static volatile size_t threads_keepalive;
static volatile size_t threads_on_hold;

/*
	Bsem functions.
*/

static __inline__ int bsem_configure(struct THPOOL_BSEM* bsem, int value)
{
	if ((value < 0) || (value > 1)) {
		return -1;
	}

	pthread_mutex_init(&(bsem->mutex), NULL);
	pthread_cond_init(&(bsem->cond), NULL);
	bsem->v = value;

	return 0;
}

static	__inline__ void bsem_reset(struct THPOOL_BSEM* bsem)
{
	bsem_configure(bsem, 0);
}

static	__inline__ void bsem_post(struct THPOOL_BSEM* bsem)
{
	pthread_mutex_lock(&(bsem->mutex));
	bsem->v = 1;
	pthread_cond_signal(&(bsem->cond));
	pthread_mutex_unlock(&(bsem->mutex));
}

static	__inline__ void bsem_post_all(struct THPOOL_BSEM* bsem)
{
	pthread_mutex_lock(&(bsem->mutex));
	bsem->v = 1;
	pthread_cond_broadcast(&(bsem->cond));
	pthread_mutex_unlock(&(bsem->mutex));
}

static	__inline__ void bsem_wait(struct THPOOL_BSEM* bsem)
{
	pthread_mutex_lock(&(bsem->mutex));
	while (bsem->v != 1) {
		pthread_cond_wait(&(bsem->cond), &(bsem->mutex));
	}
	bsem->v = 0;
	pthread_mutex_unlock(&(bsem->mutex));
}

/*
	Thread functions.
*/

static __inline__ void thread_hold(int sig_id)
{
	(void) sig_id;
	threads_on_hold = 1;
	while (threads_on_hold) {
		sleep(1);
	}
}

static __inline__ struct THPOOL_JOB* jobs_queue_pull(struct THPOOL_JOBS_QUEUE* jbq);

static __inline__ void* thread_do(struct THPOOL_THREAD* th)
{
	char thread_name[32] = "";
	struct THPOOL* thp;
	struct sigaction act;

	/* Set thread name for profiling and debuging */
	snprintf(thread_name, 32, "thread-pool-%d", th->id);
	prctl(PR_SET_NAME, thread_name);

	/* Assure all threads have been created before starting serving */
	thp = th->thp;

	/* Register signal handler */
	sigemptyset(&(act.sa_mask));
	act.sa_flags = 0;
	act.sa_handler = thread_hold;
	if (sigaction(SIGUSR1, &act, NULL) == -1) {
		/* TODO */
	}

	/* Mark thread as alive (initialized) */
	pthread_mutex_lock(&(thp->thcount_lock));
	thp->n_threads_alive += 1;
	pthread_mutex_unlock(&(thp->thcount_lock));

	while (threads_keepalive) {
		bsem_wait(thp->jobs_queue.has_jobs);

		if (threads_keepalive) {
			pthread_mutex_lock(&(thp->thcount_lock));
			thp->n_threads_working++;
			pthread_mutex_unlock(&(thp->thcount_lock));

			/* Read job from queue and execute it */
			void (*func_buff)(void*);
			void* arg_buff;
			struct THPOOL_JOB* jb = jobs_queue_pull(&(thp->jobs_queue));
			if (jb != NULL) {
				func_buff = jb->function;
				arg_buff = jb->arg;
				func_buff(arg_buff);
				free(jb);
			}

			pthread_mutex_lock(&(thp->thcount_lock));
			thp->n_threads_working--;
			if (! thp->n_threads_working) {
				pthread_cond_signal(&(thp->threads_all_idle));
			}
			pthread_mutex_unlock(&(thp->thcount_lock));
		}
	}

	pthread_mutex_lock(&(thp->thcount_lock));
	thp->n_threads_alive--;
	pthread_mutex_unlock(&(thp->thcount_lock));

	return NULL;
}

static __inline__ int thread_new(struct THPOOL* thp, struct THPOOL_THREAD** th, int id)
{
	*th = (struct THPOOL_THREAD*) malloc(sizeof(struct THPOOL_THREAD));
	if (*th == NULL) {
		return -1;
	}

	(*th)->thp = thp;
	(*th)->id = id;

	pthread_create(&(*th)->pthread, NULL, (void*(*)(void *)) thread_do, (*th));
	pthread_detach((*th)->pthread);

	return 0;
}

static __inline__ void thread_free(struct THPOOL_THREAD* th)
{
	free(th);
}

/*
	Thread pool jobs queue functions.
*/

static	__inline__ int jobs_queue_configure(struct THPOOL_JOBS_QUEUE* jbq)
{
	jbq->len = 0;
	jbq->front = NULL;
	jbq->rear  = NULL;

	jbq->has_jobs = (struct THPOOL_BSEM*) malloc(sizeof(struct THPOOL_BSEM));
	if (jbq->has_jobs == NULL) {
		return -1;
	}

	pthread_mutex_init(&(jbq->rwmutex), NULL);
	bsem_configure(jbq->has_jobs, 0);

	return 0;
}

static	__inline__ struct THPOOL_JOB* jobs_queue_pull(struct THPOOL_JOBS_QUEUE* jbq)
{
	pthread_mutex_lock(&jbq->rwmutex);
	struct THPOOL_JOB* jb = jbq->front;

	switch (jbq->len) {
	case 0:	 /* if no jobs in queue */
		break;
	case 1:	 /* if one job in queue */
		jbq->front = NULL;
		jbq->rear  = NULL;
		jbq->len = 0;
		break;

	default: /* if >1 jobs in queue */
		jbq->front = jb->prev;
		jbq->len--;
		/* more than one job in queue -> post it */
		bsem_post(jbq->has_jobs);
	}

	pthread_mutex_unlock(&jbq->rwmutex);
	return jb;
}

static	__inline__ void jobs_queue_clear(struct THPOOL_JOBS_QUEUE* jbq)
{
	while (jbq->len) {
		free(jobs_queue_pull(jbq));
	}

	jbq->front = NULL;
	jbq->rear  = NULL;
	bsem_reset(jbq->has_jobs);
	jbq->len = 0;

}

static	__inline__ void jobs_queue_push(struct THPOOL_JOBS_QUEUE* jbq, struct THPOOL_JOB* newjob)
{
	pthread_mutex_lock(&jbq->rwmutex);
	newjob->prev = NULL;

	switch (jbq->len) {
	case 0:	 /* if no jobs in queue */
		jbq->front = newjob;
		jbq->rear  = newjob;
		break;

	default: /* if jobs in queue */
		jbq->rear->prev = newjob;
		jbq->rear = newjob;
	}
	jbq->len++;

	bsem_post(jbq->has_jobs);
	pthread_mutex_unlock(&jbq->rwmutex);
}

static	__inline__ void jobs_queue_reset(struct THPOOL_JOBS_QUEUE* jbq)
{
	jobs_queue_clear(jbq);
	free(jbq->has_jobs);
}

/*
	Thread pool functions.
*/

struct THPOOL* thpool_new()
{
	return (struct THPOOL*) malloc(sizeof(struct THPOOL));
}

int thpool_configure(struct THPOOL* thp, size_t nths)
{
	int r;

	size_t n;

	threads_on_hold	= 0;
	threads_keepalive = 1;

	/* Make new thread pool */
	thp->n_threads_alive = 0;
	thp->n_threads_working = 0;

	/* Initialise the job queue */
	if (jobs_queue_configure(&thp->jobs_queue) == -1) {
		r = -1;
		goto err0;
	}

	/* Make threads in pool */
	thp->threads = (struct THPOOL_THREAD**) malloc(nths * sizeof(struct THPOOL_THREAD*));
	if (thp->threads == NULL) {
		r = -2;
		goto err1;
	}

	pthread_mutex_init(&(thp->thcount_lock), NULL);
	pthread_cond_init(&(thp->threads_all_idle), NULL);

	/* Thread init */
	for (n = 0; n < nths; n++) {
		thread_new(thp, &thp->threads[n], n);
	}

	/* Wait for threads to initialize */
	while (thp->n_threads_alive != nths) {}

	return 0;

 err1:
	jobs_queue_reset(&thp->jobs_queue);
 err0:
	return r;
}

int thpool_reset(struct THPOOL* thp)
{
	volatile size_t threads_total = thp->n_threads_alive;

	/* Give one second to kill idle threads */
	double TIMEOUT = 1.0;
	time_t start, end;
	double tpassed = 0.0;

	size_t n;

	/* End each thread's infinite loop */
	threads_keepalive = 0;

	time(&start);
	while ((tpassed < TIMEOUT) && thp->n_threads_alive) {
		bsem_post_all(thp->jobs_queue.has_jobs);
		time(&end);
		tpassed = difftime(end, start);
	}

	/* Poll remaining threads */
	while (thp->n_threads_alive) {
		bsem_post_all(thp->jobs_queue.has_jobs);
		sleep(1);
	}

	/* Job queue cleanup */
	jobs_queue_reset(&thp->jobs_queue);
	/* Deallocs */
	for (n = 0; n < threads_total; n++) {
		thread_free(thp->threads[n]);
	}
	free(thp->threads);

	return 0;
}

void thpool_free(struct THPOOL* thp)
{
	free(thp);
}


int thpool_add_work(struct THPOOL* thp, void (*function)(void*), void* arg)
{
	int r;

	struct THPOOL_JOB* jb;

	jb = (struct THPOOL_JOB*) malloc(sizeof(struct THPOOL_JOB));
	if (jb == NULL) {
		r = -1;
		goto err0;
	}

	/* add function and argument */
	jb->function = function;
	jb->arg = arg;

	/* add job to queue */
	jobs_queue_push(&thp->jobs_queue, jb);

	return 0;

 err0:
	return r;
}


void thpool_wait(struct THPOOL* thp)
{
	pthread_mutex_lock(&thp->thcount_lock);
	while (thp->jobs_queue.len || thp->n_threads_working) {
		pthread_cond_wait(&thp->threads_all_idle, &thp->thcount_lock);
	}
	pthread_mutex_unlock(&thp->thcount_lock);
}


void thpool_pause(struct THPOOL* thp)
{
	size_t n;
	for (n = 0; n < thp->n_threads_alive; n++) {
		pthread_kill(thp->threads[n]->pthread, SIGUSR1);
	}
}

void thpool_resume(struct THPOOL* thp)
{
	// resuming a single threadpool hasn't been
	// implemented yet, meanwhile this supresses
	// the warnings
	((void) thp);
	threads_on_hold = 0;
}


size_t thpool_get_busy_threads_count(struct THPOOL* thp)
{
	return thp->n_threads_working;
}
