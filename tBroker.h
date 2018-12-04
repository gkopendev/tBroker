#ifndef __TBROK__
#define __TBROK__

/* 
 * A trival/tiny broker to pass data between "publishers"(apps/threads) and 
 * "subscribers"(apps/threads) using "topics"(shared memory). This allows 
 * data-reactive behaviour instead of polling behaviour. Subscribers get a 
 * polling fd which can be used with select/poll/epoll. The fd polls out 
 * when new data is available from publisher.
 * NICE TODO: Blocking fd for subscribe.
 * NICE TODO: Peek into something other than latest sample data.
 *  	
 * 1.One creator(setup) app creates the tBroker and all metadata for it.
 * 
 * 2.Any app which wants to use the tBroker(including the setup app) 
 *   connects to it using tBroker_connect.
 * 	
 * 3.Apps connected to tBroker can now publish or subscribe on "topics"
 * 
 * 4.Subscriber apps can peek and read for data using the subscriber context 
 *   returned on a previous successful subscribe call.
 * 
 * Topic is a memory structure with unique ID and name in the system. 
 *
 * e.g. of a topic with unique name "GNSS" can be 
 * struct GNSSData_t {
 * 	// solution type 
 *	 SolutionTypes_t SolutionType;
 *	// position
 *	double lat_rad;
 *	double lng_rad;
 *	double alt;
 * };
 * associated to id "1"
 *	
 * For e.g. create a topic as 
 * topic_create(1, "GNSS", sizeof(struct GNSSData_t), 1);
 * As explained further, we would call topic_create for all such topics in
 * our init process.
 *	
 * Publishers publish a topic, while subscribers subscribe to a topic. 
 * Every subscriber gets a handle which is a file descriptor which can be	
 * passed to poll, epoll or select. The subscriber will get data on its 
 * handle whenever something is published on that topic.
 * This allows an execution thread to wait on multiple data in one loop. 
 * A simple example would be an Kalman filter APP waiting on "GPS" 
 * and/or "IMU" topics and publishes a "solution" topic. The logging APP
 * could wait simultaneously on "GPS", "IMU" and "solution" topic.
 *
 * In absence of a broker, it becomes responsibily of data producer
 * (publisher) app/thread to tell its consumers(subscribers) data is ready 
 * (by IPC MQ, pipe etc). So data producer needs to know information 
 * about it's consumers. Also consumers waiting for data from multiple 
 * producers might end up duplicating code or implementing different ways 
 * to consume multiple produced data. The broker decouples producers and
 * consumers. Consumers only need to tell the broker what data they need 
 * during init. Producers publish to broker and it's upto the broker 
 * to inform consumers.
 */

/* config */
#define MAX_TOPICS		8
#define MAX_BROKER_APPS		32
#define MAX_TOPIC_NAME_CHARS	16

/* Advanced config */
/* 
 * POSIX SHM name reserved for broker SHM, change if this name is already 
 * taken up in your filesystem(unlikely) 
 */
#define TBROKER_POSIX_SHM	"__tBroker_topics_info__"
/*	
 * UNIX domain socket name reserved for broker SHM, change if this name is 
 * already taken up in your filesystem(unlikely) 
 */
#define TBROKER_UDS_SOCKET	"__tBroker_topics_socket__"

/*
 * When an app gets spawned and connected to broker system, there might be
 * already a few subscribe calls from other apps which spawned earlier.
 * So the new app which connects to the tBroker gets a list of such 
 * subscribe requests. This number indicates the highest number of 
 * subscribe requests that can be recorded and informed to the new app
 * when it connects. In practice, majority apps will get connect to tBroker
 * before any subscribe calls are made.
 * NICE TODO: GET RID OF THIS, Its trivial, just use a growing Linked list
 */
#define MAX_TOTAL_SUBS		1024

#include "tBroker_priv.h"

struct tBroker_subscriber_context; /* Opaque data type for API calls */

/* @desc Call in creator app once */
int tBroker_init(void);
 
/* Topic operations on the broker */

/*
 * @desc Called while initialising the system. Call this function 
 * individually for all topics in system. Create all topics before trying 
 * to subscribe/publish.
 *	
 * @arg topic: unique ID
 * @arg name: name of topic, used for POSIX SHM name.
 * @arg size: sizeof(memory) associated with topic
 * @arg queue_size: Number of buffers for topic data in memory, 
 * 		1 if no buffering needed. Should be in powers of 2
 * 		(2,4,8,16 ...)
 * @return: 0 if successful, -1 when unsuccessful
 * @note : Do all topic_create in creator app, before any connect is 
 * attempted. 
 */
int tBroker_topic_create(int32_t topic, const char *name, int size, int queue_size);

/* @desc All apps need to connect once (only once) to use the tBroker */
int tBroker_connect(void);

/* @desc All apps disconnect from the tBroker to free the memory */
int tBroker_disconnect(void);

/* 
 * @desc Called by subscriber thread
 * @arg topic: topic to subscribe
 * @return ctx: reference to struct tBroker_subscriber_context.
 * Populated tBroker_subscriber_context on success or NULL
 */
struct tBroker_subscriber_context* tBroker_topic_subscribe(int32_t topic);

/*
 * @desc Another way to get Non blocking fd which can be polled on.
 * @arg ctx is the ctx populated from a previous successful subscribe call()
 */
int32_t tBroker_get_subscriber_fd(struct tBroker_subscriber_context *ctx);

/* 
 * @desc Called by subscriber thread
 * @arg topic: topic to unsubscribe
 * @return ctx: 0 on success.
 */
int32_t tBroker_topic_unsubscribe(int32_t topic, struct tBroker_subscriber_context *ctx);

/*
 * @desc For every subscribe call, do this for cleanup.
 * @note This is NOT Unsubscribe. Only for freeing memory on exit
 * @arg ctx is the ctx populated from a previous successful subscribe call()
 */
void tBroker_subscriber_context_free(struct tBroker_subscriber_context *ctx);
/*
 * @desc Called by publisher thread
 * @arg topic: which topic to publish
 * @arg buffer: data to be published. It is pointer to data of memory 
 * associated with the topic
 * @return: 0 if successful, -1 if unsuccessful
 */
int tBroker_topic_publish(int32_t topic, void *buffer);

/*
 * @desc Called by subscriber thread to check for any published data
 * @arg topic: which topic to peek
 * @arg ctx: returned by previous  topic_subscribe call
 * @return: number of data in buffer. If it's greater than queue_size,
 * some data has been lost
 */
int tBroker_topic_peek(int32_t topic, struct tBroker_subscriber_context *ctx);

/*
 * @desc Called by subscriber thread, to read published data. Until this 
 * call, data is not dequeued and file descriptor(fd) will keep polling out.
 * If data is not required for any reason(for e.g data rate too fast, etc.), 
 * pass a NULL to the buffer. The message will be dequeued and fd will not 
 * poll out again
 * @arg topic: which topic to read data from
 * @arg ctx: returned by previous  topic_subscribe call
 * @arg buffer: dest memory for topic data (allocated by caller)
 * @return: Number of bytes read and -1 if no new data available
 */
int tBroker_topic_read(int32_t topic, struct tBroker_subscriber_context *ctx, 
							void *buffer);

/*
 * @desc topic data section descriptor specifying dest, source & len for memcpy
 * @buffer: dest memory for topic data section(allocated by caller)
 * @offset: Copy from this offset(in original topic data structure)
 * @len: number of bytes to be copied 
 */
struct topic_data_section {
	void  *buffer;   
	uint32_t offset; 
	size_t len;
};

/* @desc same as tBroker_topic_read except data can be read into multiple
 * sections. This allows only partial reading of topic data
 */
int32_t tBroker_topic_read_sections(int32_t topic,
				struct tBroker_subscriber_context *ctx,
				const struct topic_data_section *sections, 
				int32_t num_sections);

/* @desc Destroy broker, call in creator app after everyone's disconnected */
int tBroker_deinit(void);

#endif