#ifdef __cplusplus
extern "C" {
#endif
#include "tBroker.h"
#ifdef __cplusplus
}
#endif

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>      /* For O_* constants */
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>	/* For mode constants */
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/un.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internals - 
 * The creator app spawns a 'init thread'(creator thread) and creates a 
 * POSIX SHM which stores topic "metadata". It also creates a socket used to 
 * listen for connection requests in the creator thread. 
 * All apps in tBroker send a connection request(using tBroker_connect). 
 * This connection is used for bidirectional comms between creator thread 
 * and other apps.
 * All apps on connection create a local copy of topic "metadata" SHM.
 * 
 * Publishers and subscribers data transfer is done through topic SHM's(data buffer). 
 * There is only one copy of it, so it should be deep enough for the slowest 
 * subscriber to use it.
 *
 * When there is topic_subscribe call an eventfd pair is created. The fd is 
 * passed to creator thread which relays it to all tBroker connected apps.
 * Thus a subscriber linked list is created for every topic in all tBroker apps. 
 * On any topic_publish in any app, all subscribers are notified.
 * The fd's are sent between apps using Unix domain socket & ancillary data
 * http://poincare.matf.bg.ac.rs/~ivana/courses/ps/sistemi_knjige/pomocno/apue/APUE/0201433079/ch17lev1sec4.html
 * We use a simple protocol for transfer and use a systemwide unique id to 
 * identify every subscribe call.
 */

/* 
 * Creator app uses topic_create and fills array of __topics_info__ 
 * created in SHM
 * @name: POSIX SHM of the topic will have this name
 * @id: unique id to reference this topic
 * @size: size of datatype associated with this topic
 * @queue_size: size of topic data queue, should be large enough that 
 * 		all subscribers have time to consume all data	
 */
struct __topics_info__ {
	char name[MAX_TOPIC_NAME_CHARS];
	int32_t id;                         	
	int size;                       	
	int queue_size;
};

/* 
 * Metadata of topics in the tBroker (resides in SHM)
 * @num_topics: Increments on every call to topic_create
 * @sub_id_count: Give each subscribe call an unique ID(systemwide)
 * @sub_id_count_lock: Synchronise systemwide access to  sub_id_count
 */
struct topics_info {
	struct __topics_info__ info[MAX_TOPICS];
	int num_topics;
	uint32_t sub_id_count;
	pthread_mutex_t sub_id_count_lock;
};

/*
 * Returned on subscribe call
 * @fd: eventfd used for pub/sub signalling
 * @s_uid: unique id assigned to this sub
 * @note: this data struct is opaque to caller, only references used 
 */
struct tBroker_subscriber_context {
	int32_t fd;
	uint32_t s_uid;
};

/* 
 * internal data of library (every app which links it)
 * @shm: points to SHM (circular buffer of topic data) of size @shm_sz
 * @first_subscriber: head of linked list of subscribers for this topic
 */
struct topic_desc {
	/* First 4 elements match __topics_info__, they get copied from SHM */
	char name[MAX_TOPIC_NAME_CHARS];
	int32_t id;			
	int size;		
	int queue_size;
						
	struct tBroker_topic_SHM *shm;
	size_t shm_sz;
	struct tBroker_subscriber *first_subscriber;
};
static struct topic_desc topics[MAX_TOPICS]; 
static int num_topics = 0;
/* UDS to creator app, referenced as clients[<index>] in creator */
static int tBroker_socket = -1;
/* sync receive & send data to creator app */
static pthread_mutex_t tBroker_socket_lock; 
static pthread_t tBroker_connect_th = 0;
static int tBroker_quit = 0;
struct topics_info *info_main_shm = NULL; /* ref to main topic "metadata" SHM */

/* Other data for creator app (which calls tBroker_init and tBroker_create) */
static int sock_listen = -1;
static int sock_ack = -1;
struct sock_conn {
	int conn_fd;
	int ack;
};
static struct sock_conn clients[MAX_BROKER_APPS];
static volatile int num_clients = 0;
static int epoll_fd = -1;
static pthread_t tBroker_init_th = 0;
struct tBroker_subscriber_metadata {
	int32_t id; 
	int32_t subscriber_fd; 
	int32_t orig_s_uid ;
};
static struct tBroker_subscriber_metadata *all_subs;
static volatile int all_subs_index = 0;

/* 
 * Creator app runs a thread which acts as a tBroker server.
 * It has following functions - 
 * 1. Listen for incoming client connections(apps which call tBroker_connect)
 *	and maintain a list of them. Pass them all existing subscribe requests.
 * 2. Receive subscribe requests from 'a' client for a topic and relay it to all
 * 	connected clients. Thus every client knows about all subscribe requests.
 * 	This is necessary as the client may wish to publish on that topic.
 * 3. Receive subscribe ACKS from all clients and ACK the client which requested
 * 4. Receive unsubscribe requests from 'a' client for a topic and relay it to
 * 	all connected clients.
 * 
 * Every app which call tBroker_connect is a tbroker client.
 * UDS comms between server <--> client is a simple protocol of 17 data bytes.
 */

#define CLIENT_CONNECTED_RET_CODE 	100
#define CLIENT_DISCONNECTED_RET_CODE 	200
#define CLIENT_WAIT_FOR_ACK_SUB_FD	301
#define CLIENT_ACK_SUB_FD		302
#define ALL_CLIENTS_ACKED_SUB_FD	303

/* helper function, add pointed fd to pointed epoll */
static int add_to_epoll(int *p_epoll, int *p_fd)
{
	int ret;
	struct epoll_event event;
	event.events = EPOLLIN | EPOLLPRI;
	event.data.fd = *p_fd;
	ret = epoll_ctl (*p_epoll, EPOLL_CTL_ADD, event.data.fd, &event);
	if (ret < 0)
		fprintf(stderr, "epoll err - %s", strerror(errno));
	return ret;
}

/* helper function, delete pointed fd from pointed epoll */
static int del_from_epoll(int *p_epoll, int *p_fd)
{
	int ret;
	struct epoll_event event;
	event.events = EPOLLIN | EPOLLPRI;
	event.data.fd = *p_fd;
	ret = epoll_ctl (*p_epoll, EPOLL_CTL_DEL, event.data.fd, &event);
	if (ret < 0)
		fprintf(stderr, "epoll err - %s", strerror(errno));
	return ret;
}

/*****************************************************************************/
/****************************** TBROKER SERVER *******************************/
/*****************************************************************************/

/* helper function, use protocol to send fd to an app referenced by client_i */
static void send_topic_fd_to_a_client(int client_i, int32_t id, 
				int32_t subscriber_fd, int32_t orig_s_uid)
{
	/* server ---> client */
	struct iovec    iov[1];
	struct msghdr   msg;
	char            buf[32] = {0};
	int flags = 0;
	int CONTROLLEN  = CMSG_LEN(sizeof(int));
	static struct cmsghdr   *cmptr = NULL;
	cmptr = (struct cmsghdr *)malloc(CONTROLLEN);
	if (cmptr == NULL) return;
		
	/* pass buf as message on the socket */
	memset(&msg, 0, sizeof(msg));
	iov[0].iov_base = buf;  /* only 1 location(buf), only 1 iov */
	iov[0].iov_len  = 17; 
	msg.msg_iov     = iov;
	msg.msg_iovlen  = 1;
	msg.msg_name    = NULL;
	msg.msg_namelen = 0;
		
	/* add ancillary data which is the fd and set SCM_RIGHTS */
	cmptr->cmsg_level  = SOL_SOCKET;
	cmptr->cmsg_type   = SCM_RIGHTS;
	cmptr->cmsg_len    = CONTROLLEN;
	msg.msg_control    = cmptr;
	msg.msg_controllen = CONTROLLEN;
	*(int32_t *)CMSG_DATA(cmptr) = subscriber_fd;
		
	/* 
	 * populate, buf = 17 bytes = 
	 * str('topic id') + 4 byte topic id + 4 byte original fd + str('\n') 
	 */
	buf[0]='t';buf[1]='o'; buf[2]='p';buf[3]='i'; 
	buf[4]='c';buf[5]=' '; buf[6]='i';buf[7]='d';
	*(int32_t *)(&(buf[8])) = id;
	*(int32_t *)(&(buf[12])) = orig_s_uid;
	buf[16] = '\n';
		
	/* Send buf which has ancillary data(subscriber_fd) to the client */

	/* Make the fd blocking temporarily to comply for sendmsg semantics */
	flags = fcntl(clients[client_i].conn_fd, F_GETFL, 0);
	flags &= ~O_NONBLOCK;
	fcntl(clients[client_i].conn_fd, F_SETFL, flags);
	if (sendmsg(clients[client_i].conn_fd, &msg, 0) != 17) {
		fprintf(stderr, "fd send issue, client %d - id %d \r\n", 
							client_i, id);
	}
	flags |= O_NONBLOCK;
	fcntl(clients[client_i].conn_fd, F_SETFL, flags);
	/* Now we can epoll in tBroker_init th thread again */

	free(cmptr);
}

static void send_topic_fd_to_a_client_noack(int client_i, int32_t id, 
				int32_t subscriber_fd, int32_t orig_s_uid)
{
	/* server ---> client */
	struct iovec    iov[1];
	struct msghdr   msg;
	char            buf[32] = {0};
	int flags = 0;
	int CONTROLLEN  = CMSG_LEN(sizeof(int));
	static struct cmsghdr   *cmptr = NULL;
	cmptr = (struct cmsghdr *)malloc(CONTROLLEN);
	if (cmptr == NULL) return;
		
	/* pass buf as message on the socket */
	memset(&msg, 0, sizeof(msg));
	iov[0].iov_base = buf;  /* only 1 location(buf), only 1 iov */
	iov[0].iov_len  = 17; 
	msg.msg_iov     = iov;
	msg.msg_iovlen  = 1;
	msg.msg_name    = NULL;
	msg.msg_namelen = 0;
		
	/* add ancillary data which is the fd and set SCM_RIGHTS */
	cmptr->cmsg_level  = SOL_SOCKET;
	cmptr->cmsg_type   = SCM_RIGHTS;
	cmptr->cmsg_len    = CONTROLLEN;
	msg.msg_control    = cmptr;
	msg.msg_controllen = CONTROLLEN;
	*(int32_t *)CMSG_DATA(cmptr) = subscriber_fd;
		
	/* 
	 * populate, buf = 17 bytes = 
	 * str('topic id') + 4 byte topic id + 4 byte original fd + str('\n') 
	 */
	buf[0]='n';buf[1]='o'; buf[2]='a';buf[3]='c'; 
	buf[4]='k';buf[5]=' '; buf[6]='i';buf[7]='d';
	*(int32_t *)(&(buf[8])) = id;
	*(int32_t *)(&(buf[12])) = orig_s_uid;
	buf[16] = '\n';
		
	/* Send buf which has ancillary data(subscriber_fd) to the client */

	/* Make the fd blocking temporarily to comply for sendmsg semantics */
	flags = fcntl(clients[client_i].conn_fd, F_GETFL, 0);
	flags &= ~O_NONBLOCK;
	fcntl(clients[client_i].conn_fd, F_SETFL, flags);
	if (sendmsg(clients[client_i].conn_fd, &msg, 0) != 17) {
		fprintf(stderr, "fd send issue, client %d - id %d \r\n", 
							client_i, id);
	}
	flags |= O_NONBLOCK;
	fcntl(clients[client_i].conn_fd, F_SETFL, flags);
	/* Now we can epoll in tBroker_init th thread again */

	free(cmptr);
}

/* helper function, use protocol to unsub fd in an app referenced by client_i */
static void send_topic_unsub_fd_to_a_client(int client_i, int32_t id, 
						int32_t orig_s_uid)
{
	/* server ---> client */
	struct iovec    iov[1];
	struct msghdr   msg;
	char            buf[32] = {0};
	int flags = 0;

	/* pass buf as message on the socket */
	memset(&msg, 0, sizeof(msg));
	iov[0].iov_base = buf;  /* only 1 location(buf), only 1 iov */
	iov[0].iov_len  = 17; 
	msg.msg_iov     = iov;
	msg.msg_iovlen  = 1;
	msg.msg_name    = NULL;
	msg.msg_namelen = 0;

	/* 
	 * populate, buf = 17 bytes = 
	 * str('topicusb') + 4 byte topic id + 4 byte original fd + str('\n') 
	 */
	buf[0]='t';buf[1]='o'; buf[2]='p';buf[3]='i'; 
	buf[4]='c';buf[5]='u'; buf[6]='s';buf[7]='b';
	*(int32_t *)(&(buf[8])) = id;
	*(int32_t *)(&(buf[12])) = orig_s_uid;
	buf[16] = '\n';

	/* Make the fd blocking temporarily to comply for sendmsg semantics */
	flags = fcntl(clients[client_i].conn_fd, F_GETFL, 0);
	flags &= ~O_NONBLOCK;
	fcntl(clients[client_i].conn_fd, F_SETFL, flags);
	if (sendmsg(clients[client_i].conn_fd, &msg, 0) != 17) {
		fprintf(stderr, "fd unsub issue, client %d - id %d \r\n", 
							client_i, id);
	}
	flags |= O_NONBLOCK;
	fcntl(clients[client_i].conn_fd, F_SETFL, flags);
	/* Now we can epoll in tBroker_init th thread again */
}

/* Send buf which sends ancillary data(subscriber_fd) to all clients */
static void send_topic_fd_to_clients(int32_t id, int32_t subscriber_fd, int32_t orig_s_uid)
{
	int client_i;

	for (client_i = 0; client_i < num_clients; client_i++)
		clients[client_i].ack = CLIENT_WAIT_FOR_ACK_SUB_FD;

	for (client_i = 0; client_i < num_clients; client_i++)
		send_topic_fd_to_a_client(client_i, id, subscriber_fd, orig_s_uid);	
}

/* Send unsub request to all clients */
static void send_topic_unsub_fd_to_clients(int32_t id, int32_t orig_s_uid)
{
	int client_i;

	for (client_i = 0; client_i < num_clients; client_i++)
		send_topic_unsub_fd_to_a_client(client_i, id, orig_s_uid);	
}

/* inform broker client that it is now ready to start pub-sub */
static void send_connected_message_to_a_client(int client_i)
{
	/* server ---> client */
	struct iovec    iov[1];
	struct msghdr   msg;
	char            buf[32] = {0};
	int flags = 0;
			
	/* pass buf as message on the socket */
	memset(&msg, 0, sizeof(msg));
	iov[0].iov_base = buf;  /* only 1 location(buf), only 1 iov */
	iov[0].iov_len  = 17; 
	msg.msg_iov     = iov;
	msg.msg_iovlen  = 1;
	msg.msg_name    = NULL;
	msg.msg_namelen = 0;
		
	/* 
	 * populate, buf = 17 bytes = 
	 * str('connectd') + 4 byte CLIENT_CONNECTED_RET_CODE + 4 byte dont care + str('\n') 
	 */
	buf[0]='c';buf[1]='o'; buf[2]='n';buf[3]='n'; 
	buf[4]='e';buf[5]='c'; buf[6]='t';buf[7]='d';
	*(int32_t *)(&(buf[8])) = CLIENT_CONNECTED_RET_CODE;
	*(int32_t *)(&(buf[12])) = client_i;
	buf[16] = '\n';
	
	/* Make the fd blocking temporarily to comply for sendmsg semantics */
	flags = fcntl(clients[client_i].conn_fd, F_GETFL, 0);
	flags &= ~O_NONBLOCK;
	fcntl(clients[client_i].conn_fd, F_SETFL, flags);
	if (sendmsg(clients[client_i].conn_fd, &msg, 0) != 17) {
		fprintf(stderr, "connect send issue, client %d %s \r\n", client_i, strerror(errno));
	}
	else 
		fprintf(stdout, "Sent connectd to client %d \r\n", client_i);
	flags |= O_NONBLOCK;
	fcntl(clients[client_i].conn_fd, F_SETFL, flags);
}

/* tell a pub client, its sub request was successful */
static void send_all_clients_acked_message_to_a_client(int client_i)
{
	/* server ---> client */
	struct iovec    iov[1];
	struct msghdr   msg;
	char            buf[32] = {0};
	int 		flags = 0;
		
	memset(&msg, 0, sizeof(msg));
	iov[0].iov_base = buf;
	iov[0].iov_len  = 17; 
	msg.msg_iov     = iov;
	msg.msg_iovlen  = 1;
	msg.msg_name    = NULL;
	msg.msg_namelen = 0;

	buf[0]='a';buf[1]='l'; buf[2]='l';buf[3]='s'; 
	buf[4]='u';buf[5]='b'; buf[6]='e';buf[7]='d';
	*(int32_t *)(&(buf[8])) = ALL_CLIENTS_ACKED_SUB_FD;
	buf[16] = '\n';

	/* Make the fd blocking temporarily to comply for sendmsg semantics */
	flags = fcntl(clients[client_i].conn_fd, F_GETFL, 0);
	flags &= ~O_NONBLOCK;
	fcntl(clients[client_i].conn_fd, F_SETFL, flags);
	if (sendmsg(clients[client_i].conn_fd, &msg, 0) != 17) {
		fprintf(stderr, "connect send issue, client %d \r\n", client_i);
	}
	flags |= O_NONBLOCK;
	fcntl(clients[client_i].conn_fd, F_SETFL, flags);
}

/* 
 * client connection handler executed when data is received from client(apps)
 * one of the apps must have sent data (topic + fd), process it and send to all 
 * apps connected to tBroker 
 */
static void sock_conn_handler(uint32_t revents, int client_i)
{
	int             nr = 0, r = 0, i;
	int32_t         newfd = -1, orig_s_uid = -1, topic_id;
	uint8_t         buf[32] = {0};
	struct iovec    iov[1];
	struct msghdr   msg;
	static struct cmsghdr   *cmptr = NULL;
	int CONTROLLEN  = CMSG_LEN(sizeof(int));
	int fd = clients[client_i].conn_fd;
	
	if (revents & EPOLLIN);
	else return;
	
	cmptr = (struct cmsghdr *)malloc(CONTROLLEN);
	if (cmptr == NULL) return;
	
	/* 
	 * Make sure you collect the complete message and fd, send it to all 
	 * clients before you return from this function
	 */
	for ( ; ; ) {
		memset(&msg, 0, sizeof(msg));
		//iov[0].iov_base = buf+nr;
		//iov[0].iov_len  = sizeof(buf) - nr;
		iov[0].iov_base = buf;
		iov[0].iov_len  = 17;
		msg.msg_iov     = iov;
		msg.msg_iovlen  = 1;
		msg.msg_name    = NULL;
		msg.msg_namelen = 0;
		msg.msg_control    = cmptr;
		msg.msg_controllen = CONTROLLEN;
		while (r < 17) {
			r += recvmsg(fd, &msg, MSG_PEEK);
			usleep(100);
		}
		if ((r = recvmsg(fd, &msg, 0)) < 0) {
			fprintf(stderr, "recvmsg error - server - %s \r\n", strerror(errno));
		}
		else if (r == 0) {
			fprintf(stdout, "connection closed by client \r\n");
			/* remove client fd and adjust our client array */
			del_from_epoll(&epoll_fd, &fd);
			close(fd);
			for (i=client_i; i<num_clients; i++) {
				clients[i].conn_fd = clients[i+1].conn_fd;
				clients[i].ack = clients[i+1].ack;
			}
			clients[i].conn_fd = -1;
			clients[i].ack  = -1;
			num_clients--;
			break;
		}
		if (r > 0) nr += r;
		/* 
		 * Protocol = 
		 * 'topic id' + 4 bytes of topic id + 4 byte original fd + '\n' 
		 */
		if ((nr == 17) && (buf[nr - 1] == '\n')) {
			/* complete message received */
			if (strncmp(buf, "topic id",8) == 0) {
				/* Grab fd and relay to all clients */
				topic_id = *(int32_t *)(&(buf[8]));
				newfd = *(int32_t *)CMSG_DATA(cmptr);
				orig_s_uid = *(int32_t *)(&(buf[12]));
				if ((all_subs) && 
				(all_subs_index < MAX_TOTAL_SUBS)) {
					all_subs[all_subs_index].id = topic_id;
					all_subs[all_subs_index].subscriber_fd = newfd;
					all_subs[all_subs_index].orig_s_uid = orig_s_uid;
					all_subs_index++;
				}
				send_topic_fd_to_clients(topic_id, newfd, orig_s_uid);
				break;
			} else if (strncmp(buf, "topicack",8) == 0) {
				/* Client acked the last sub req */
				if (*(int32_t *)(&(buf[8])) == CLIENT_ACK_SUB_FD)
					if ((clients[client_i].ack) == 	CLIENT_WAIT_FOR_ACK_SUB_FD)
						clients[client_i].ack = CLIENT_ACK_SUB_FD;
				/* Check if all clients acked, if yes inform the client which reqested sub */
				for (i=client_i; i<num_clients; i++) 
					if (clients[i].ack == CLIENT_WAIT_FOR_ACK_SUB_FD)
						break;
				if (i == num_clients)
					send_all_clients_acked_message_to_a_client(client_i);
				
				break;
			} else if (strncmp(buf, "topicusb",8) == 0) {
				/* Tell all clients to unsub this fd */
				topic_id = *(int32_t *)(&(buf[8]));
				orig_s_uid = *(int32_t *)(&(buf[12]));
				for (i=0; i<all_subs_index; i++)
					if (orig_s_uid == all_subs[all_subs_index].orig_s_uid)
						break;
				while(i<(all_subs_index - 1))
					memcpy(&all_subs[i], &all_subs[i+1], sizeof(struct tBroker_subscriber_metadata));
				all_subs_index--;			
				send_topic_unsub_fd_to_clients(topic_id, orig_s_uid);
				break;
			}
		}
		else {
			if (nr < 17) {
				usleep(100);
				continue;
			} else {
				fprintf(stderr, "Invalid bytes from client sock \r\n");
				break;
			}
		}
	}
		
	free(cmptr);
}


/* handler for socket listener fd in creator app, adds client connections */
static int sock_listen_handler(uint32_t revents)
{
	int flags = 0;
	int client_i = -1, i=0;
	
	/* Find a free space and use it for client connection */
	for (client_i=0; client_i<MAX_BROKER_APPS; client_i++) {
		if (clients[client_i].conn_fd == -1)
			break;
	}
	
	if (client_i == MAX_BROKER_APPS) {
		fprintf(stderr, "No more clients, max reached \r\n");
		return -1;	/* No more clients allowed */
	}
	
	if (revents & EPOLLIN) {
		clients[client_i].conn_fd = accept(sock_listen, NULL, NULL);
		if (clients[client_i].conn_fd < 0) {
			clients[client_i].conn_fd = -1;
			fprintf(stderr, "Client UDS socket error \r\n");
			return -1;
		}
		else {
			/* 
			 * If we have already received topic_subscribe requests 
			 * before this app connected, inform.
			 */
			if (all_subs_index > 0) {
				for (i=0; i<all_subs_index; i++) {
					//usleep(200);
					fprintf(stdout, "bag-%d/%d \r\n", i+1, all_subs_index);
					send_topic_fd_to_a_client_noack(client_i, 
						all_subs[i].id, 
						all_subs[i].subscriber_fd, 
						all_subs[i].orig_s_uid);
				}
			}
			/*
			 * This makes sure client has an updated list of subs,
			 * so now it can start publishing
			 */
			send_connected_message_to_a_client(client_i);

			/* Add client to our tBroker_init thread epoll */
			/* fd is non blocking, as epoll is used */
			// flags = fcntl(clients[client_i].conn_fd, F_GETFL, 0);
			// flags |= O_NONBLOCK;
			// fcntl(clients[client_i].conn_fd, F_SETFL, flags);
			
			/* 
			 * A message containing fd is received on this 
			 * conn_fd(socket) which is then sent to all apps in 
			 * tBroker system 
			 */
			if (add_to_epoll(&epoll_fd, 
				&(clients[client_i].conn_fd)) >= 0) {
				num_clients++;
				fprintf(stdout, "Added client %d\r\n", client_i);
			} else {
				fprintf(stderr, "cannot epoll client \r\n");
			}
			return 0;
		}
	}
	else
		return -1;
}

/* 
 * Background thread which runs in tBroker creator app
 * 1) It waits for new client connections
 * 2) Forwards subscribe topic fd's to all apps in tBroker
 */
static int init_efd = -1;
static void *tBroker_init_func(void *par)
{
	int res, poll_forever = -1, i, n,s;
	struct epoll_event events[3 + MAX_BROKER_APPS];
	struct sockaddr_un addr;
	int flags = 0;
	uint64_t ev = 1;

	epoll_fd = epoll_create1(EPOLL_CLOEXEC);
		
	/* 
	 * This socket waits for apps to request 'accept' i.e. all apps 
	 * wanting to connect to tBroker.
	 */
	if ((sock_listen = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		fprintf(stderr,"sock_listen not added \r\n");
		write(init_efd, &ev, 8);
		return NULL;
	}	
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	
	strncpy(addr.sun_path, TBROKER_UDS_SOCKET, sizeof(addr.sun_path)-1);
	unlink(TBROKER_UDS_SOCKET);

	if (bind(sock_listen, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		fprintf(stderr,"sock_listen not binded \r\n");
		write(init_efd, &ev, 8);
		return NULL;
	}
	if (listen(sock_listen, 0) == -1) { /* No backlog */
		fprintf(stderr,"sock_listen not listening \r\n");
		write(init_efd, &ev, 8);
		return NULL;
	}
	/* Add server so you can listen and accept connections */
	flags = fcntl(sock_listen, F_GETFL, 0);
	flags |= O_NONBLOCK;
	fcntl(sock_listen, F_SETFL, flags);
	add_to_epoll(&epoll_fd, &sock_listen);
	
	/* these are fd's to talk with all clients who will connect */
	for (i=0; i<MAX_BROKER_APPS; i++)
		clients[i].conn_fd = -1;
	
	/* sync with spawner(tBroker_init func) */
	write(init_efd, &ev, 8);
	
	while(tBroker_quit == 0) {
		res = epoll_wait(epoll_fd, events, (3 + MAX_BROKER_APPS), poll_forever);
		if (res == -1) break;
		for (i=0;i<res;i++) {
			if (events[i].data.fd == sock_listen) {
				/* New connection request maybe */
				sock_listen_handler(events[i].events);
			} else {
				for (s=0; s<num_clients; s++) {
					if (events[i].data.fd == clients[s].conn_fd) {
						sock_conn_handler(events[i].events, s);
					}
				}   
			}
		}
	}
	
	if (epoll_fd > 0)       close(epoll_fd);
	if (sock_listen > 0)	close(sock_listen);
	for (s=0; s<num_clients; s++) {
		if (clients[s].conn_fd > 0)
			close(clients[s].conn_fd);
	}
	close(init_efd);
}

/*  only Creator app in tBroker calls this */
int tBroker_init(void)
{
	int ret;  uint64_t ev = 0;
	struct topics_info *info_shm;
	pthread_mutexattr_t mutex_attr;

	/* Create a SHM which has topics info and all apps will use it */
	size_t sz = sizeof(struct topics_info);
	shm_unlink(TBROKER_POSIX_SHM);
	int fd = shm_open(TBROKER_POSIX_SHM, 
				O_CREAT | O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO);
	if (fd < 0) {
		fprintf(stdout, "tBroker_init failed on SHM \r\n");
		ret = -1;
	} else {
		ftruncate(fd, sz);
		info_shm = (struct topics_info *)
		 mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		pthread_mutexattr_init(&mutex_attr);
		pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
		pthread_mutex_init(&(info_shm->sub_id_count_lock), &mutex_attr);
		pthread_mutex_lock(&(info_shm->sub_id_count_lock));
		info_shm->sub_id_count = 1000;
		pthread_mutex_unlock(&(info_shm->sub_id_count_lock));
		close(fd);
		munmap((void*)info_shm, sz);
					
		num_topics = 0;
		all_subs = malloc(sizeof(struct tBroker_subscriber_metadata) * 
							MAX_TOTAL_SUBS);
		init_efd = eventfd(0,0);

		ret = pthread_create(&tBroker_init_th, NULL, 
					&tBroker_init_func, NULL);
		if (ret >= 0) read(init_efd, &ev, 8);
		
	}

tBroker_init_ret:	
	return ret;
}

/* 
 * Creator app in tBroker calls this for every topic in system, 
 * create all topics before any apps connect 
 */
int tBroker_topic_create(int32_t topic, const char *name, int size, int queue_size)
{
	int i,q,fd,ret = 0;
	size_t sz = sizeof(struct topics_info);
	struct topics_info *info_shm;
	
	size_t offset_lock;
	pthread_mutexattr_t mutex_attr;
	pthread_rwlockattr_t rwlock_attr;
	pthread_rwlock_t *p_rwlock;
	
	if (num_topics < MAX_TOPICS);
	else {
		fprintf(stdout, "s - No more topics can be added \r\n");
		return -1;
	}

	pthread_mutexattr_init(&mutex_attr);
	/* Inform about illegal lock/unlocks */
	pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_ERRORCHECK);
	/* Should not be necessary */
	pthread_mutexattr_setprotocol(&mutex_attr, PTHREAD_PRIO_INHERIT);
	/* publish mutex lock needs to be accessed by multiple processes */
	pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
	/* topic data readwrite lock accessed by multiple processes */
	pthread_rwlockattr_init(&rwlock_attr);
	/* Prefer writers and same thread cannot lock again before unlock */
	pthread_rwlockattr_setkind_np(&rwlock_attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
	/* Data is accessed across multiple processes */
	pthread_rwlockattr_setpshared(&rwlock_attr, PTHREAD_PROCESS_SHARED);
    
	i = num_topics;
	strcpy(topics[i].name, name);
	topics[i].id = topic; topics[i].size = size; 
	topics[i].queue_size = queue_size;
	
	/* Create fresh POSIX SHM with name as specified by init app */
	shm_unlink(topics[i].name);
	fd = shm_open(topics[i].name, 
			O_CREAT | O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO);
	topics[i].shm_sz =
		sizeof(pthread_mutex_t) +	/* memory for pub lock */
		sizeof(uint32_t) +		/* memory for head */
		/* memory for ((topic data size + lock) * queue size) */
		((topics[i].size + sizeof(pthread_rwlock_t)) * 
					topics[i].queue_size);
	if (fd < 0) {
		ret = -1;
		fprintf(stderr, "Failed to create %s topic SHM - %s \r\n", 
			topics[i].name, strerror(errno));
		goto topic_create_ret;
	} else {
		/* Allocate required memory for this topic SHM */
		ftruncate(fd, topics[i].shm_sz); 
	}
	
	(topics[i].shm) = (struct tBroker_topic_SHM *)
		mmap(NULL, topics[i].shm_sz, 
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (topics[i].shm ==  MAP_FAILED) {
		ret = -1;
		fprintf(stderr, "Failed to mmap %s topic SHM - %s \r\n", 
			topics[i].name, strerror(errno));
		close (fd);
		goto topic_create_ret;
	}
	(topics[i].shm)->head = 0;
	
	/* Init the publisher lock */
	if (pthread_mutex_init(&((topics[i].shm)->pub_lock), &mutex_attr) < 0) {
		ret = -1;
		fprintf(stderr, "Failed to init lock %s topic SHM - %s \r\n", 
			topics[i].name, strerror(errno));
		close (fd);
		goto topic_create_ret;
	}
	
	/* Init all rwlock's for the queue */
	offset_lock = sizeof(pthread_mutex_t) + sizeof(uint32_t);
	for (q=0; q<topics[i].queue_size; q++) {
		/* access the rwlock in memory using offsets */
		p_rwlock = (pthread_rwlock_t *)
				((void*)topics[i].shm + offset_lock);
		if (pthread_rwlock_init(p_rwlock, &rwlock_attr) < 0) {
			ret = -1;
			fprintf(stderr, "Failed to init rwlock %s topic SHM - %s \r\n", 
				topics[i].name, strerror(errno));
			close (fd);
			goto topic_create_ret;
		}
		offset_lock += (topics[i].size + sizeof(pthread_rwlock_t));
	}
	
	close (fd);
		
topic_create_ret:	
	/* If unsuccesfull cleanup everything */
	if (ret < 0)
		tBroker_deinit();
	else {
		/* update shared memory of topics info and increment index */
		fd = shm_open(TBROKER_POSIX_SHM, 
				O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO);
		if (fd < 0) ;
		else {
			ftruncate(fd, sz);
			info_shm = (struct topics_info *)
				mmap(NULL, sz, PROT_READ | PROT_WRITE, 
						MAP_SHARED, fd, 0);
			memcpy(&(info_shm->info[i]), &(topics[i]), 
				sizeof(struct __topics_info__));
			close(fd);
			num_topics++;
			info_shm->num_topics = num_topics;
			munmap((void*)info_shm, sz);
		}
	}
	
	if (topics[i].shm !=  MAP_FAILED) {
		munmap((void*)topics[i].shm, topics[i].shm_sz); 
		/* We will map it for using in connect */
	}
	
	return ret;
}

/* Creator app deinit's to free up resources for tBroker */
int tBroker_deinit(void)
{
	int i, q, fd, sz;
	size_t offset_lock;
	pthread_rwlock_t *p_rwlock;
	
	for (i=0; i<num_topics; i++) {
		if ((topics[i].shm) && (topics[i].shm != MAP_FAILED)) {
			pthread_mutex_destroy(&((topics[i].shm)->pub_lock));
			offset_lock = sizeof(pthread_mutex_t) + sizeof(uint32_t);
			for (q=0; q < topics[i].queue_size; q++) {
				p_rwlock = (pthread_rwlock_t *)
					((void *)topics[i].shm + offset_lock);
				pthread_rwlock_destroy(p_rwlock);
				offset_lock += 
				(topics[i].size + sizeof(pthread_rwlock_t));
			}
			munmap(topics[i].shm, topics[i].shm_sz);
			topics[i].shm = NULL;
			shm_unlink(topics[i].name);
		}
	}

	fd = shm_open(TBROKER_POSIX_SHM, O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO);
	sz = sizeof(struct topics_info);
	struct topics_info *info_shm = (struct topics_info *)
		 mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	pthread_mutex_destroy(&(info_shm->sub_id_count_lock));
	close(fd);
	munmap((void*)info_shm, sz);
	
	shm_unlink(TBROKER_POSIX_SHM);
	num_topics = 0;
	pthread_cancel(tBroker_init_th);
	pthread_join(tBroker_init_th, NULL);
	if (all_subs) free(all_subs);
}


/*****************************************************************************/
/****************************** TBROKER CLIENTS ******************************/
/*****************************************************************************/

/* forward declarations */
static int __topic_subscribe(int32_t topic, int32_t fd, int32_t orig_s_uid); 
static int __topic_unsubscribe(int32_t topic, int32_t orig_s_uid);
static void send_sub_fd_ack(void);

/* handler for any data received from the server. Add subscriber fd to  list */
static int client_handler(uint32_t revents, int fd)
{
	int             nr = 0, r = 0,  ret = -1;
	char            buf[32] = {0};
	struct iovec    iov[1];
	struct msghdr   msg;
	static struct cmsghdr   *cmptr = NULL;
	int CONTROLLEN  = CMSG_LEN(sizeof(int));
	int32_t topic_id = -100, rcv_fd = -1;
	int32_t orig_s_uid = -1;

	cmptr = (struct cmsghdr *)malloc(CONTROLLEN);
	if (cmptr == NULL) return ret;

	if (revents & EPOLLIN);
	else return 0;
		
	for ( ; ; ) {
		memset(&msg, 0, sizeof(msg));
		iov[0].iov_base = buf+nr;
		iov[0].iov_len  = sizeof(buf) - nr;
		msg.msg_iov     = iov;
		msg.msg_iovlen  = 1;
		msg.msg_name    = NULL;
		msg.msg_namelen = 0;
		msg.msg_control    = cmptr;
		msg.msg_controllen = CONTROLLEN;
		if ((r = recvmsg(fd, &msg, 0)) < 0) {
			
			fprintf(stderr, "c - recvmsg error %s \r\n", strerror(errno));
			ret = 0;
			break;
		} else if (r == 0) {
			fprintf(stdout, "c - connection closed by server \r\n");
			/* remove epoll no point! */
			ret = CLIENT_DISCONNECTED_RET_CODE;
			break;
		}
		if (r > 0) nr += r;
		/* Protocol = 'topic id' + 4 bytes of topic id + '\n' */
		if ((nr == 17) && (buf[nr - 1] == '\n')) {
			/* complete message received */
			if (strncmp(buf, "topic id",8) == 0) {
				/* add this subscriber fd */
				topic_id = *(int32_t *)(&(buf[8]));
				rcv_fd = *(int32_t *)CMSG_DATA(cmptr);
				orig_s_uid = *(int32_t *)(&(buf[12]));
				if (__topic_subscribe(topic_id, rcv_fd, orig_s_uid) < 0)
					fprintf(stderr, "c - topic id %d not added \r\n", topic_id);
				send_sub_fd_ack();
				ret = 0;
				break;
			} else if (strncmp(buf, "noack id",8) == 0) {
				/* add this subscriber fd */
				topic_id = *(int32_t *)(&(buf[8]));
				rcv_fd = *(int32_t *)CMSG_DATA(cmptr);
				orig_s_uid = *(int32_t *)(&(buf[12]));
				if (__topic_subscribe(topic_id, rcv_fd, orig_s_uid) < 0)
					fprintf(stderr, "c - topic id %d not added \r\n", topic_id);
				ret = 0;
				break;
			
			} else if (strncmp(buf, "connectd",8) == 0) {
				/* Client connect request success, can now pub-sub */
				if ((*(int32_t *)(&(buf[8]))) == CLIENT_CONNECTED_RET_CODE) {
					fprintf(stdout, "c - connected at %d \r\n", *(int32_t *)(&(buf[12])));
					ret = CLIENT_CONNECTED_RET_CODE;
				}
				else
					ret = 0;
				break;
			} else if (strncmp(buf, "allsubed",8) == 0) {
				/* Most recent sub request was sent to all clients and acked */
				if ((*(int32_t *)(&(buf[8]))) == ALL_CLIENTS_ACKED_SUB_FD)
					ret = ALL_CLIENTS_ACKED_SUB_FD;
				else
					ret = 0;
				break;
			} else if (strncmp(buf, "topicusb",8) == 0) {
				/* remove this subscriber fd */
				topic_id = *(int32_t *)(&(buf[8]));
				orig_s_uid = *(int32_t *)(&(buf[12]));
				if (__topic_unsubscribe(topic_id, orig_s_uid) < 0)
					fprintf(stderr, "c - topic id %d not unsub \r\n", topic_id);
				ret = 0;
				break;
			}
			
		}
		else {
			/* wait if needed and collect complete message */
			if (nr < 17) {
				usleep(100);
				continue;
			} else {
				/* Some error in client socket, never mind */
				ret = 0;
				break;
			}
		}
	}

	free(cmptr);

	return ret;
}

/* 
 * All apps in tBroker connect, so they can wait for subscriber fd's sent by 
 * main creator app, this is the background thread 
 */
static int conn_efd = -1;
static int curr_sub_efd = -1;
void *tBroker_connect_func(void *par)
{
	int epoll_fd_connect = -1, res = -1, poll_forever = -1, 
		i = 0, flags = 0, quit_conn = 0, ret_code = -1;
	struct epoll_event events[2];
	struct sockaddr_un addr;
	struct epoll_event event;
	uint64_t ev = 1;

	epoll_fd_connect = epoll_create1(EPOLL_CLOEXEC);
	if ((tBroker_socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		fprintf(stdout, "sock_listen not added \n");
		write(conn_efd, &ev, 8);
		return NULL;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;

	strncpy(addr.sun_path, TBROKER_UDS_SOCKET, sizeof(addr.sun_path)-1);

	int times = 1000;
	do {
		/* try till the connection binds */
		res = connect(tBroker_socket, 
				(struct sockaddr*)&addr, sizeof(addr));
		if (res < 0)  {
			usleep(10000);
			fprintf(stdout, "Retry broker socket conn \r\n");
		} else
			break;
	}
	while((res < 0) && (times-- > 0));

	pthread_mutex_init(&tBroker_socket_lock, NULL);
		
	flags = fcntl(tBroker_socket, F_GETFL, 0);	
	flags |= O_NONBLOCK;
	fcntl(tBroker_socket, F_SETFL, flags);
	event.events = EPOLLIN;
	event.data.fd = tBroker_socket;
	if (epoll_ctl 
		(epoll_fd_connect, EPOLL_CTL_ADD, event.data.fd, &event) < 0) {
		write(conn_efd, &ev, 8);
		return NULL;
	}

	while(tBroker_quit == 0) {
		res = epoll_wait(epoll_fd_connect, events, 2, poll_forever);
		if (res == -1) break;
		for (i=0; i<res; i++) {
			if (events[i].data.fd == tBroker_socket) {
				if (events[i].events & EPOLLIN);
				else continue;
				pthread_mutex_lock(&tBroker_socket_lock);
				ret_code = client_handler(events[i].events, tBroker_socket);
				if (ret_code ==  CLIENT_DISCONNECTED_RET_CODE) {
					epoll_ctl (epoll_fd_connect, 
						EPOLL_CTL_DEL, 
						tBroker_socket, NULL);
					/* In case connection was never complete */
					write(conn_efd, &ev, 8);
					quit_conn = 1;
				}
				else if (ret_code ==  CLIENT_CONNECTED_RET_CODE) {
					/* connect is successful, ready to pub-sub */
					write(conn_efd, &ev, 8); 
				}
				else if (ret_code == ALL_CLIENTS_ACKED_SUB_FD) {
					/* Successful sub, signal client to carry on */
					write(curr_sub_efd, &ev, 8); 
				}
				pthread_mutex_unlock(&tBroker_socket_lock);
			}
		}
		if (quit_conn == 1) break;
	}

	close(conn_efd);
	close(curr_sub_efd);
}

/* all apps in tBroker need to connect */
int tBroker_connect(void)
{
	int i,q,fd,ret=0;
	size_t sz = sizeof(struct topics_info);
	uint64_t ev = 0;
	
	/* Get information of all topics */
	fd = shm_open(TBROKER_POSIX_SHM, O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO);
	if (fd < 0) {
		ret = -1;
		goto tBroker_connect_ret;
	}
	ftruncate(fd, sz);

	info_main_shm = (struct topics_info *)mmap(NULL, sz, 
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	
	if (info_main_shm == MAP_FAILED) {
		ret = -1;
		goto tBroker_connect_ret;
	}
	num_topics = info_main_shm->num_topics;
	/* Populate in lib copy */
	for (i=0; i<num_topics; i++) {
		memcpy(&topics[i], &(info_main_shm->info[i]), 
			sizeof(struct __topics_info__));
	}
	close(fd);
	
	for (i=0; i<num_topics; i++) {
		/* connect to all the SHM's, you have all topic data access */
		fd = shm_open(topics[i].name, O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO);
		topics[i].shm_sz = sizeof(pthread_mutex_t) + sizeof(uint32_t) + 
				((topics[i].size + sizeof(pthread_rwlock_t)) * 
				topics[i].queue_size);
		if (fd < 0) {
			ret = -1;
			fprintf(stderr, "Failed to create %s topic SHM - %s \r\n", 
						topics[i].name, strerror(errno));
			break;
		} else 
			ftruncate(fd, topics[i].shm_sz);
		topics[i].shm = (struct tBroker_topic_SHM *)
				mmap(NULL, topics[i].shm_sz, 
				PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (topics[i].shm ==  MAP_FAILED ) {
			ret = -1;
			fprintf(stderr, "Failed to mmap %s topic SHM - %s \r\n", 
				topics[i].name, strerror(errno));
			close (fd);
			break;
		}
		close (fd);
		/* 
		 * The app can access all topic data using "topics" data struct
		 * Subscribers will be later added to this same struct 
		 */
		topics[i].first_subscriber = NULL;
	}

tBroker_connect_ret:	
	/* If unsuccesfull cleanup everything */
	if (ret < 0)
		tBroker_disconnect();
	else {
		/* start thread to accept subscribers */
		conn_efd = eventfd(0,0);
		curr_sub_efd = eventfd(0,0);
		ret = pthread_create(&tBroker_connect_th, NULL, 
					&tBroker_connect_func, NULL);
		/* block till th is ready to start publishing */
		read(conn_efd, &ev, 8); 
		/* we are ready now or just quit */
	}
	
	return ret;
}

/* all apps in tBroker disconnect to free up resources */
int tBroker_disconnect(void)
{
	int i;
	struct tBroker_subscriber *sub = NULL, *tmp = NULL;
	
	for (i=0; i<num_topics; i++) {
		if ((topics[i].shm) && (topics[i].shm != MAP_FAILED)) {
			/* free the mallocs for subscribers */
			sub = topics[i].first_subscriber;
			while(sub) {
				tmp = sub;
				sub = sub->next;
			}
			while(tmp) {
				sub = tmp->prev;
				if (tmp) {
					close(tmp->fd);
					free(tmp);
				}
				tmp = sub;
			}
			munmap(topics[i].shm, topics[i].shm_sz);
			topics[i].shm = NULL;
		}
	}
	
	tBroker_quit = 1;
	pthread_mutex_destroy(&tBroker_socket_lock);
	pthread_cancel(tBroker_connect_th);
	pthread_join(tBroker_connect_th, NULL);

	munmap(info_main_shm, sizeof(struct topics_info));
}

/* helper function, adds fd to topic subscriber linked list */
static int __topic_subscribe(int32_t topic, int32_t fd, int32_t orig_s_uid)
{
	int i, ret = -1;
	struct tBroker_subscriber **p_sub = NULL;
	struct tBroker_subscriber **p_sub_prev = NULL;
	
	for (i=0; i<num_topics; i++) {
		if (topic == topics[i].id)
			break;
	}
	
	if (i<num_topics) {
		//pthread_mutex_lock(&((topics[i].shm)->pub_lock));
		p_sub = &(topics[i].first_subscriber);
		while(*p_sub) {
			p_sub_prev = p_sub;
			p_sub = &((*p_sub)->next);
		}
		*p_sub = malloc(sizeof(struct tBroker_subscriber));
		if (*p_sub) {
			(*p_sub)->fd = fd;
			(*p_sub)->orig_s_uid = orig_s_uid;
			if (((*p_sub)->fd) < 0) {
				free(*p_sub);
				ret = -1;
			} else {
				(*p_sub)->next = NULL;
				if (p_sub_prev)
					(*p_sub)->prev = *p_sub_prev;
				else
					(*p_sub)->prev = NULL; /* first node */
				/* start from latest data */
				(*p_sub)->tail = ((topics[i].shm)->head);
				ret = (*p_sub)->fd; 
			}
		}
		//pthread_mutex_unlock(&((topics[i].shm)->pub_lock));
	}
	else
		ret = -1;

	return ret;
}

/* 
 * Every topic subscriber call creates an fd which is sent to creator app which 
 * sends it to all apps in tBroker, thus any app which publishes can notify all 
 * subscribers 
 */
struct tBroker_subscriber_context* tBroker_topic_subscribe(int32_t topic)
{
	/* client ---> server */

	int32_t 	subscriber_fd = -1, i, s_uid = 0;
	struct iovec    iov[1];
	struct msghdr   msg;
	char            buf[32] = {0};
	struct tBroker_subscriber_context *ctx = NULL;
	uint64_t ev = 0;
		
	int flags = 0;
	int CONTROLLEN  = CMSG_LEN(sizeof(int));
	static struct cmsghdr   *cmptr = NULL;
		
	
	pthread_mutex_lock(&tBroker_socket_lock);
		
	for (i=0; i<num_topics; i++) {
		if (topic == topics[i].id)
			break;
	}
	if (i==num_topics) goto topic_subscribe_ret;
		
	cmptr = (struct cmsghdr *)malloc(CONTROLLEN);
	if (cmptr == NULL) goto  topic_subscribe_ret;
		
	pthread_mutex_lock(&((topics[i].shm)->pub_lock));

	/* pass buf as message on the socket */
	memset(&msg, 0, sizeof(msg));
	iov[0].iov_base = buf;  /* one location(buf), only one iov needed */
	iov[0].iov_len  = 17; 
	msg.msg_iov     = iov;
	msg.msg_iovlen  = 1;
	msg.msg_name    = NULL;
	msg.msg_namelen = 0;
		
	/* add ancillary data which is the fd and set SCM_RIGHTS */
	cmptr->cmsg_level  = SOL_SOCKET;
	cmptr->cmsg_type   = SCM_RIGHTS;
	cmptr->cmsg_len    = CONTROLLEN;
	msg.msg_control    = cmptr;
	msg.msg_controllen = CONTROLLEN;
		
	/* get an event fd, pass it to tBroker_init socket  */
	subscriber_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
	*(int32_t *)CMSG_DATA(cmptr) = subscriber_fd;
		
	/* 
	 * populate, buf = 17 bytes = 
	 * str('topic id') + 4 byte topic id + 4 byte original fd + str('\n') 
	 */
	pthread_mutex_lock(&info_main_shm->sub_id_count_lock);
	s_uid = info_main_shm->sub_id_count;
	info_main_shm->sub_id_count++;
	pthread_mutex_unlock(&info_main_shm->sub_id_count_lock);

	buf[0]='t';buf[1]='o'; buf[2]='p';buf[3]='i'; 
	buf[4]='c';buf[5]=' '; buf[6]='i';buf[7]='d';
	*(int32_t *)(&(buf[8])) = topic;
	*(int32_t *)(&(buf[12])) = s_uid;
	buf[16] = '\n';
 
	flags = fcntl(tBroker_socket, F_GETFL, 0);
	flags &= ~O_NONBLOCK;
	fcntl(tBroker_socket, F_SETFL, flags);
		
	sendmsg(tBroker_socket, &msg, 0);

	//flags = fcntl(tBroker_socket, F_GETFL, 0);
	flags |= O_NONBLOCK;
	fcntl(tBroker_socket, F_SETFL, flags);
        
	free(cmptr);
		
	ctx = malloc(sizeof(struct tBroker_subscriber_context));
	ctx->fd = subscriber_fd;
	ctx->s_uid = s_uid;

	read (curr_sub_efd, &ev, 1);
	pthread_mutex_unlock(&((topics[i].shm)->pub_lock));

topic_subscribe_ret:
	pthread_mutex_unlock(&tBroker_socket_lock);

	return ctx;
}

static void send_sub_fd_ack(void)
{
	/* client ---> server */

	struct iovec    iov[1];
	struct msghdr   msg;
	char            buf[32] = {0};
	int 		flags = 0;

	memset(&msg, 0, sizeof(msg));
	iov[0].iov_base = buf;
	iov[0].iov_len  = 17; 
	msg.msg_iov     = iov;
	msg.msg_iovlen  = 1;
	msg.msg_name    = NULL;
	msg.msg_namelen = 0;

	buf[0]='t';buf[1]='o'; buf[2]='p';buf[3]='i'; 
	buf[4]='c';buf[5]='a'; buf[6]='c';buf[7]='k';
	*(int32_t *)(&(buf[8])) = CLIENT_ACK_SUB_FD;
	buf[16] = '\n';

	flags = fcntl(tBroker_socket, F_GETFL, 0);
	flags &= ~O_NONBLOCK;
	fcntl(tBroker_socket, F_SETFL, flags);
	sendmsg(tBroker_socket, &msg, 0);
	flags |= O_NONBLOCK;
	fcntl(tBroker_socket, F_SETFL, flags);
}

static int __topic_unsubscribe(int32_t topic, int32_t orig_s_uid)
{
	int32_t i;
	int ret = -1;
	struct tBroker_subscriber *sub = NULL;
	
	for (i=0; i<num_topics; i++) {
		if (topic == topics[i].id)
			break;
	}
	
	if (i<num_topics) {
		pthread_mutex_lock(&((topics[i].shm)->pub_lock));
		sub = topics[i].first_subscriber;
		while(sub) {
			if (sub->orig_s_uid == orig_s_uid)
				break;
			sub = sub->next;
		}
		if (sub) {
			if (sub->next)
				sub->next->prev = sub->prev;
			if (sub->prev)
				sub->prev->next = sub->next;
			else
				topics[i].first_subscriber = sub->next;	
		}
		pthread_mutex_unlock(&((topics[i].shm)->pub_lock));
		if (sub)   {
			close(sub->fd);
			free(sub);
		}
		ret = 0;
	}
	else
		ret = -1;

	return ret;
}

int32_t tBroker_topic_unsubscribe(int32_t topic, struct tBroker_subscriber_context *ctx)
{
	/* client ---> server */

	int32_t 	ret = -1,subscriber_fd = -1, i, s_uid = 0;
	uint32_t 	curr_head;
	struct iovec    iov[1];
	struct msghdr   msg;
	char            buf[32] = {0};

	int32_t flags = 0;
	
	pthread_mutex_lock(&tBroker_socket_lock);
		
	for (i=0; i<num_topics; i++) {
		if (topic == topics[i].id)
			break;
	}
	
	if (i==num_topics) goto topic_unsubscribe_ret;
			
	/* pass buf as message on the socket */
	memset(&msg, 0, sizeof(msg));
	iov[0].iov_base = buf;
	iov[0].iov_len  = 17; 
	msg.msg_iov     = iov;
	msg.msg_iovlen  = 1;
	msg.msg_name    = NULL;
	msg.msg_namelen = 0;
		
	/* 
	 * populate, buf = 17 bytes = 
	 * str('topicusb') + 4 byte topic id + 4 byte original fd + 
	 * str('\n') 
	 */
	buf[0]='t';buf[1]='o'; buf[2]='p';buf[3]='i'; 
	buf[4]='c';buf[5]='u'; buf[6]='s';buf[7]='b';
	*(int32_t *)(&(buf[8])) = topic;
	*(int32_t *)(&(buf[12])) = ctx->s_uid;
	buf[17] = '\n';
 
	flags = fcntl(tBroker_socket, F_GETFL, 0);
	flags &= ~O_NONBLOCK;
	fcntl(tBroker_socket, F_SETFL, flags);
		
	sendmsg(tBroker_socket, &msg, 0);
			
	flags |= O_NONBLOCK;
	fcntl(tBroker_socket, F_SETFL, flags);
		
topic_unsubscribe_ret:
	pthread_mutex_unlock(&tBroker_socket_lock);
	
	return 0;
}

int32_t tBroker_get_subscriber_fd(struct tBroker_subscriber_context *ctx)
{
	if (ctx) return ctx->fd;
	else return -1;
}

void tBroker_subscriber_context_free(struct tBroker_subscriber_context *ctx)
{
	if (ctx != NULL) free(ctx);
}

/* Notify all subscribers across apps about new data */
int tBroker_topic_publish(int32_t topic, void *buffer)
{
	int i, idx, ret = -1;
	uint64_t ev = 0;
	struct tBroker_subscriber *sub;
	struct epoll_event event;
	size_t offset_lock = sizeof(pthread_mutex_t) + sizeof(uint32_t);
	void *buff;
	pthread_rwlock_t *p_rwlock;
	
	for (i=0; i<num_topics; i++) {
		if (topic == topics[i].id) break;
	}
	
	if (i<num_topics) {
		pthread_mutex_lock(&((topics[i].shm)->pub_lock));
		if (buffer) {
			idx = ((topics[i].shm)->head & (topics[i].queue_size - 1));
			offset_lock += (idx * 
				(topics[i].size + sizeof(pthread_rwlock_t)));
			p_rwlock = (pthread_rwlock_t *)
				((void *)topics[i].shm + offset_lock);
			buff = ((void *)topics[i].shm + 
				offset_lock + 
				sizeof(pthread_rwlock_t));
			if (pthread_rwlock_wrlock(p_rwlock) < 0) {
				fprintf(stderr, "Error rwlock %s \r\n", 
							strerror(errno));
				goto unlock;
			} else {
			 	memcpy(buff, buffer, topics[i].size);
			 	pthread_rwlock_unlock(p_rwlock);
			}
		}
		else
			goto unlock;

		((topics[i].shm)->head)++;
		
		sub = topics[i].first_subscriber;
		while (sub)  {
			ev = 1;
			/* efd counter += 1, subscribers will be notified */
			write(sub->fd, &ev, 8); 
			/* 
			 * Write will pass in nearly all conditions except when 
			 * counter reaches MAX(UINT64) number of unread data. 
			 * There is no hope then!
			 */
			sub = sub->next;
		}
		
		ret = 0;
	unlock:	
		pthread_mutex_unlock(&((topics[i].shm)->pub_lock));
		
	}
	
	return ret;
}

/* check how many publishes of new data, usually 1 if no data is missed */ 
int tBroker_topic_peek(int32_t topic, struct tBroker_subscriber_context *ctx)
{
	int i, ret = -1;
	struct tBroker_subscriber *sub;
	uint32_t head;
	
	for (i=0; i<num_topics; i++) {
		if (topic == topics[i].id)
			break;
	}
	
	if (i<num_topics) {
		sub = topics[i].first_subscriber;
		pthread_mutex_lock(&((topics[i].shm)->pub_lock));
		head = (topics[i].shm)->head;
		pthread_mutex_unlock(&((topics[i].shm)->pub_lock));
		while (sub)  {
			/* check handle with original fd number */
			if (sub->orig_s_uid == ctx->s_uid) {
				if (sub->tail <= head)
					ret = (head - sub->tail);
				else
					ret = (0xFFFFFFFF - sub->tail + head + 1);
				break;
			}
			sub=sub->next;
		}
	}
	
	return ret; 
}

/* 
 * copy new published data, will copy the latest  data. Will adjust for 
 * data loss (move subscriber tail count), if data is consumed is slow 
 */
static int32_t _topic_read(int32_t i, struct tBroker_subscriber_context *ctx, 
	const struct topic_data_section *sections, int32_t num_sections)
{
	int ret = -1, idx, deque_notification = 0,j;
	struct tBroker_subscriber *sub;
	uint64_t ev;
	uint32_t head;
	pthread_rwlock_t *p_rwlock;
	size_t offset_lock;
	void *buff;
	
	sub = topics[i].first_subscriber;
	while (sub)  {
		//printf(".");
		if (sub->orig_s_uid == ctx->s_uid) {
			//printf("p \r\n");
			pthread_mutex_lock(&((topics[i].shm)->pub_lock));
			head = (topics[i].shm)->head;
			pthread_mutex_unlock(&((topics[i].shm)->pub_lock));

			int32_t queued_msgs;
			if (sub->tail <= head)
				queued_msgs = (head - sub->tail);
			else
				queued_msgs = (0xFFFFFFFF - sub->tail + head + 1);
			
			if (queued_msgs == 0) {
				break; /* Empty, no new data published */
			} else if (queued_msgs <= topics[i].queue_size) {
				idx = (sub->tail & (topics[i].queue_size - 1));
				deque_notification = 1;
			} else {
				fprintf(stdout, "loss %s %d \r\n", 
					topics[i].name, queued_msgs);
				/* Catch up to last non overwritten data */
				while (queued_msgs != topics[i].queue_size) {
					if (read(ctx->fd, &ev, 8) < 0){
						ret = -1;
						break;
					} else 
						sub->tail++;
					queued_msgs--;
				}
				idx = (sub->tail & (topics[i].queue_size - 1));
				deque_notification = 1;
			}

			/* Deque one notification */
			if (deque_notification == 1) {
				if (read(ctx->fd, &ev, 8) < 0) {
					ret = -1;
					break;
				}
				else sub->tail++;
			}
			
			/* one item of data */
			offset_lock = sizeof(pthread_mutex_t) + sizeof(uint32_t);
			if (num_sections > 0) {
				offset_lock += (idx * (topics[i].size + 
						sizeof(pthread_rwlock_t)));
				p_rwlock = (pthread_rwlock_t *)((void *)topics[i].shm + offset_lock);
				buff = ((void *)topics[i].shm + offset_lock + sizeof(pthread_rwlock_t));
				pthread_rwlock_rdlock(p_rwlock);
				for (j=0; j<num_sections; j++) {
					memcpy(sections[j].buffer, (buff + sections[j].offset), sections[j].len);
					ret += sections[j].len;
				}
				pthread_rwlock_unlock(p_rwlock);
			} else
				ret = 0;
			
			break;
		}
		sub=sub->next;
	}
	
	return ret;
}

int tBroker_topic_read_sections(int32_t topic, 
			struct tBroker_subscriber_context *ctx,
			const struct topic_data_section *sections, 
			int32_t num_sections)
{
	int i, j = 0;
	int ret = -1;
	for (i=0; i<num_topics; i++) {
		if (topic == topics[i].id)
			break;
	}
	
	if (i == num_topics)
		return ret;
	
	for (j=0; j<num_sections; j++) {
		if (((sections+j) != NULL) && sections[j].buffer)
			if ((sections[j].offset + sections[j].len) <= topics[i].size);
			else break;
	}
	if (j == num_sections) /* All sections valid */
		ret = _topic_read(i, ctx, sections, num_sections);
	else
		ret = -1;
	
	return ret;
}

int tBroker_topic_read(int32_t topic, 
			struct tBroker_subscriber_context *ctx, void *buffer)
{
	int ret = -1;
	int i;
	struct topic_data_section section;

	for (i=0; i<num_topics; i++) {
		if (topic == topics[i].id)
			break;
	}
	
	if (i == num_topics) 
		return ret;
	
	if (buffer)  {
		section.buffer = buffer;
		section.len = topics[i].size;
		section.offset = 0;
		ret = _topic_read(i, ctx, &section, 1);
	}
	else
		ret = _topic_read(i, ctx, NULL, 0);
	
	return ret;
}

#ifdef __cplusplus
}
#endif
