#ifndef SRC_JOBQUEUE_H_
#define SRC_JOBQUEUE_H_
#include <pthread.h>
#include <errno.h>
//#include <stdatomic.h>

class SimpleQueue {
public:
	virtual void clear() = 0;
	virtual void *pop() = 0;
	virtual int push(void *job) = 0;
	virtual int getCount() = 0;
	virtual bool isFull() = 0;
};

class JobQueue {

public:
	enum QUEUE_STATE {QUEUE_RUN = 0, QUEUE_END_OF_DATA, QUEUE_ABORT};
	void *pop();
	int push(void *job);
	void setState(QUEUE_STATE mode);
	void clear();
	bool isFull();
	int getCount();
	JobQueue(SimpleQueue *squeue, enum QUEUE_STATE state, bool waitMode = true);

private:
	int mutexLock();
	bool 				wait;
	pthread_mutex_t		mutex;		// guard
	pthread_cond_t		queueChanged;
	short	 			finishMode{QUEUE_RUN};
	SimpleQueue			*queue;
};

#endif /* SRC_JOBQUEUE_H_ */
