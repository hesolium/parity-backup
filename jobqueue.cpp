#include <stdio.h>
#include "jobqueue.h"

JobQueue::JobQueue(SimpleQueue *squeue, enum QUEUE_STATE state, bool waitMode) {
	queue = squeue;
	wait = waitMode;
	finishMode = state;
	pthread_mutex_init(&mutex, NULL);
	pthread_cond_init(&queueChanged, NULL);
}

void JobQueue::setState(QUEUE_STATE mode) {
	if(finishMode == mode)
		return;
	finishMode = mode;
	pthread_mutex_lock(&mutex);
	if(finishMode == QUEUE_ABORT)
		queue->clear();
	pthread_mutex_unlock(&mutex);
		// unblock waiting threads
	pthread_cond_broadcast(&queueChanged);
}

//JobQueue::~JobQueue() {
//
//}

void JobQueue::clear() {
	queue->clear();
}

bool JobQueue::isFull() {
	return queue->isFull();
}

int JobQueue::getCount() {
	return queue->getCount();
}

int JobQueue::mutexLock() {
	int st = (wait) ? pthread_mutex_lock(&mutex) : pthread_mutex_trylock(&mutex);
	if(st && st != EBUSY)
		setState(QUEUE_ABORT);
	return st;
}

void *JobQueue::pop() {
	if(mutexLock())
		return NULL;
	void *job = NULL;
	bool full = queue->isFull();
	job = queue->pop();
	if(wait) {
		while(job == NULL && queue->getCount() == 0 && finishMode == QUEUE_RUN) {
			pthread_cond_wait(&queueChanged, &mutex);
			if(finishMode != QUEUE_ABORT)
				job = queue->pop();
		}
	}
	if(job == NULL && finishMode != QUEUE_ABORT)
		job = queue->pop();	// sometimes queue switch to EOD but queue is not empty!
	pthread_mutex_unlock(&mutex);
	if(job && full)
		pthread_cond_broadcast(&queueChanged);
	return job;
}

int JobQueue::push(void *job) {
	if(finishMode == QUEUE_ABORT)
		return -1;
	int rv = mutexLock();
	if(rv)
		return -1;
	if(job == NULL) {
		if(finishMode == QUEUE_RUN)
			finishMode = QUEUE_END_OF_DATA;
		pthread_mutex_unlock(&mutex);
		pthread_cond_broadcast(&queueChanged);
		return 0;
	}
	bool empty = (queue->getCount() == 0);
	if(wait) {
		rv = -1;
		while(job != NULL && queue->isFull() && finishMode != QUEUE_ABORT) {
			pthread_cond_wait(&queueChanged, &mutex);
			if(finishMode != QUEUE_ABORT) {
				rv = queue->push(job);
				if(rv >= 0)
					job = NULL;
			}
		}
		if(job && finishMode != QUEUE_ABORT)
			rv = queue->push(job);
	} else
		rv = queue->push(job);
	pthread_mutex_unlock(&mutex);
	if(rv >= 0 && empty)
		pthread_cond_broadcast(&queueChanged);
	return rv;
}




