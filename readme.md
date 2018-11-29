A trival/tiny broker to pass data between "publishers"(threads) and 
"subscribers"(threads) using "topics"(memory). This allows data-reactive 
behaviour instead of polling behaviour. Subscribers get a polling fd which polls
out when data is available from publisher.
 
There can be multiple publishers and multiple subscribers. Publishers and 
subscribers communicate using "topics". Topic is a memory structure with unique
ID and name throughout the system. 

/* E.g. of a topic can be


	struct GNSSData_t {
		// solution type
		SolutionTypes_t	SolutionType;

		// position
		FLOAT64	latrad;
		FLOAT64	lngrad;
		FLOAT64	alt;

		// velocity
		FLOAT32	vx;
		FLOAT32	vy;
		FLOAT32	vz;

		// attitude
		FLOAT64	att_roll;
		FLOAT64	att_pitch;
		FLOAT64	att_heading;
		
		// error estimates
		FLOAT32	vSigma;
		FLOAT32	KF3DVelSTD;
		FLOAT32	Position_Quality2D;

		// status
		uint32_t KalmanStatus;
	};
	associated to id "1"
	
	So we would create a topic as 
	topic_create(1, "GNSS", sizeof(struct GNSSData_t), 1);
	
	As explained further we would call topic_create for all such topics in 
	init process.


Publishers publish a topic, while subscribers subscribe to a topic. 
Every subscriber gets a handle which is a file descriptor which can be passed to
epoll or select. The subscriber will get data on its handle whenever something 
is published on that topic.

Publishers or subscribers are just pthreads all working in same address space.
Each publisher would have a unique publish call to a topic(It can publish to
multiple topics) in its loop. Subscribers will poll on the handles and take
action on whichever handle returns. A thread can be a publisher and/or subscriber.

This allows an execution thread to wait on multiple data in one loop. 
A simple example would be an Kalman filter thread waiting on "GPS" and/or "IMU" 
topics and publishes a "solution" topic. The logging thread could wait 
simultaneously on "GPS", "IMU" and "solution" topic.

In absence of a broker, it becomes responsibily of data producer(publisher) 
thread to tell its consumers(subscribers) data is ready (by IPC MQ or pipe etc.).
So data producer needs to know information about it's consumers. Also consumers 
waiting for data from multiple producers might end up duplicating code or 
implementing different ways to consume multiple produced data. The broker 
decouples producers and consumers. Consumers only need to tell the broker what 
data they need during init. Producers publish to broker and it's upto the broker 
to inform consumers.

To use it - add tBroker.c, tBroker.h and tBroker_priv.h to your build.
Use tBroker.h as API.
The following can be configured in the header file - 

#define MAX_TOPICS		32
#define MAX_TOPIC_NAME_CHARS	64			// string for readability
#define MAX_BROKER_APPS		8			// How many processes will communicate with each other
#define MAX_TOTAL_SUBS		1024			// How many total subscribers across apps can we have
#define TBROKER_POSIX_SHM	"__topics_info__"	// POSIX SHM name used internally, do not reuse in system
#define TBROKER_UDS_SOCKET	"__topics_socket__"	// POSIX UDS name used internally, do not reuse in system

To use the example , compile test_Broker.c with all other files by

"gcc tBroker.c test_tBroker.c *.h -g  -o tBrok -lrt -lpthread"

Compile test_tBroker2.c with all other files by

"gcc tBroker.c test_tBroker2.c *.h -g  -o tBrok2 -lrt -lpthread" 

So we have 2 apps which can communicate using tBroker. 
tBrok is the main creator app which creates all topics connects to broker and does pub/sub.
tBrok2 is the other app which connects to tBroker and does pub/sub. 
Run tBrok and then tBrok2. You could run multiple apps of tBrok2.

./tBrok

./tBrok2