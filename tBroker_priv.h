#ifndef __TBROK_PRIV__
#define __TBROK_PRIV__

#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>

/*	
 * One app can create SHM's necessary for topics, other apps can connect.
 * All topic SHM's should be created before any pub/sub.
 * One "topic" POSIX SHM will be of  
 * size = ((sizeof topic data in bytes + size of topic data lock) 
 *				* queue size of topic data) +
 * 	(size of topic publish lock) +
 *	(size of topic head)
 * "size of topic data"  is size of data type we use as the topic.
 * "size of topic data lock" is size of pthread_rwlock_t.
 * There are "queue" number of items of (topic data and it's lock).
 * If we think of 1 topic data = 1 sample, the 
 * This data buffer must be as deep as the slowest topic subscriber needs.
 * size of publish lock is size of pthread_mutex_t.
 * size of topic head is size of uint32_t(topic publish happens at head).
 * Thus there is a queue of topic data and every element has a rwlock and 
 * queue is fed at index pointed by head.
 * So a publish happens like this -  
 * acquire publish lock, copy data at queue index = 
 * (head & (queue size - 1)), increment head.
 *
 * One topic data (or one sample) in memory looks like
 * struct tBroker_topic_data {
 * 	pthread_rwlock_t lock;
 *  	uint8_t buff[]; //topic data memory
 * }
 * We use dynamic allocation, the above struct is just a static rep.
 */

/* Queue of topic data in SHM - one for each topic  */
struct tBroker_topic_SHM {
	uint32_t head;
	pthread_mutex_t pub_lock;
	uint8_t data[];	/* memory blob which contains queue of topic_data */
};

/*
 * Subscriber to a topic. 
 * This is one item in a linked list(LL) of subscribers.
 * There would be one LL of subscribers for every topic in each app.
 */
struct tBroker_subscriber {
	/* used for communication between the subscriber and broker */
	int fd;
	/* Next subscriber of this topic in LL. If NULL, last subscriber */
	struct tBroker_subscriber *next;
	/* previous subscriber of this topic in LL. If NULL first subscriber */
	struct tBroker_subscriber *prev;
	/* Indicates how deep into the queue has this subsriber reached */
	volatile uint32_t tail;
	/* needed for reference to compare for peek and read calls */
	int orig_s_uid;
};

#endif /* __TBROK_PRIV__ */
