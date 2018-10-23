#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h> 
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include "tBroker.h"

#define BUF_SIZE    512
#define Q_SIZE      1
#define LOOP        100000

pthread_t All_topic_sub0, All_topic_sub1, All_topic_sub2, All_topic_sub3, All_topic_sub4, All_topic_sub5;
void *All_topic_sub0_func(void *); 


#define TOPIC_0 0
#define TOPIC_1 1
#define TOPIC_2 2
#define TOPIC_3 3
#define TOPIC_4 4
#define TOPIC_5 5
#define TOPIC_6 6
#define TOPIC_7 7


uint64_t exit_app = 0;
int exit_0_fd = -1, exit_1_fd = -1, exit_2_fd = -1, exit_3_fd = -1;


void main(void)
{
    int times=0;
    pthread_t this_thread = pthread_self();
    pthread_attr_t my_attr;
    struct sched_param schedParam;
    uint8_t dummy_pub_data[BUF_SIZE];
    
    pthread_attr_init(&my_attr);
    schedParam.sched_priority = 40;
    pthread_attr_setschedparam(&my_attr, &schedParam);
    pthread_attr_setschedpolicy(&my_attr, SCHED_FIFO);
    
    pthread_setschedparam(this_thread, SCHED_FIFO, &schedParam);
    mlockall(MCL_CURRENT|MCL_FUTURE);
    unsigned char dummy[8196];
	memset(dummy, 0, 8196);
    
    tBroker_connect();
    
    /* This main loop simulates publisher thread, lets create subscriber threads */
    pthread_create( &All_topic_sub0, &my_attr, &All_topic_sub0_func, NULL); exit_0_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    
 
    sleep(5);
    
    /* this main thread simulates publishers */
    /* Publish on every topic */
    while (times < LOOP) {
        sprintf(dummy_pub_data, "%d \r\n", times);
    //    tBroker_topic_publish(TOPIC_7, dummy_pub_data);
        times++;
        usleep(1 * 1000);
    }
    
    sleep(5);
    
    exit_app = 1;

    write(exit_0_fd, &exit_app, 8);

    pthread_join(All_topic_sub0, NULL);
    
    close(exit_0_fd); 
    
    tBroker_disconnect();

}

int add_to_epoll(int *p_epoll, int *p_fd)
{
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLPRI;
	event.data.fd = *p_fd;
    return epoll_ctl (*p_epoll, EPOLL_CTL_ADD, event.data.fd, &event);
}


void *All_topic_sub0_func(void *par) 
{
    int fd_00 = -1, fd_01 = -1, fd_02 = -1, fd_03 = -1;
    struct tBroker_subscriber_context *fd_00_ctx, *fd_01_ctx, *fd_02_ctx, *fd_03_ctx;
    int fd_04 = -1, fd_05 = -1, fd_06 = -1, fd_07 = -1;
    
    int fd_00_count = 0, fd_01_count = 0, fd_02_count = 0, fd_03_count = 0;
         
    int epoll_fd = -1, res, poll_forever = -1, i, n;
    struct epoll_event events[10];
    uint64_t exit0 = 0;
    FILE *fd_00_0, *fd_00_1, *fd_00_2, *fd_00_3;
    uint8_t dummy_sub_data[BUF_SIZE];

    int header = -1, foooter = -1, middle = -1; 
    
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    
    if (epoll_fd > 0) {
        /* First subscriber to TOPIC_0 */
        fd_00_ctx = tBroker_topic_subscribe(TOPIC_0); 
        if (fd_00_ctx == NULL) {
            printf("fd_00 err \r\n");
        } else {
            fd_00 = tBroker_get_subscriber_fd(fd_00_ctx);
            add_to_epoll(&epoll_fd, &fd_00);
        }

        /* First subscriber to TOPIC_1 */
        fd_01_ctx = tBroker_topic_subscribe(TOPIC_1); 
        if (fd_01_ctx == NULL) {
            printf("fd_01 err \r\n");
        } else {
            fd_01 = tBroker_get_subscriber_fd(fd_01_ctx);
            add_to_epoll(&epoll_fd, &fd_01);
        }

        /* First subscriber to TOPIC_2 */
        fd_02_ctx = tBroker_topic_subscribe(TOPIC_2); 
        if (fd_02_ctx == NULL) {
            printf("fd_02 err \r\n");
        } else {
            fd_02 = tBroker_get_subscriber_fd(fd_02_ctx);
            add_to_epoll(&epoll_fd, &fd_02);
        }  
        
        /* First subscriber to TOPIC_3 */
        fd_03_ctx = tBroker_topic_subscribe(TOPIC_3); 
        if (fd_03_ctx == NULL) {
            printf("fd_03 err \r\n");
        } else {
            fd_03 = tBroker_get_subscriber_fd(fd_03_ctx);
            add_to_epoll(&epoll_fd, &fd_03);
        }  
        
        add_to_epoll(&epoll_fd, &exit_0_fd);
        
        // fd_00_0 = fopen("fd_00_0.txt", "w+");
        // fd_00_1 = fopen("fd_00_1.txt", "w+");
        // fd_00_2 = fopen("fd_00_2.txt", "w+");
        // fd_00_3 = fopen("fd_00_3.txt", "w+");
        printf("Ready \r\n");
        //usleep(200);
        while(!exit_app) {
    		res = epoll_wait(epoll_fd, events, 10, poll_forever);
    		if (res == -1) break;
    		else {
    		    for (i=0;i<res;i++) {
    		        if (events[i].data.fd == fd_00) {
    		            n = tBroker_topic_peek(TOPIC_0, fd_00_ctx);
                        //printf("n= %d \r\n", n);
    		            while(n > 0) {
							
    		                tBroker_topic_read(TOPIC_0, fd_00_ctx, dummy_sub_data);
                            // fputs(dummy_sub_data, fd_00_0);
                            header = strtol(dummy_sub_data,NULL,10);
                            middle = strtol(dummy_sub_data+200,NULL,10);
                            foooter = strtol(dummy_sub_data+500,NULL,10);
                            if ((header == (fd_00_count + 100)) &&
                                (middle == (fd_00_count + 100)) &&
                                (foooter == (fd_00_count + 100))) ;
                            else 
                                printf("n");
                            n--;
                            fd_00_count++;
    		            }
    		        }
    		        else if (events[i].data.fd == fd_01) {
    		            n = tBroker_topic_peek(TOPIC_1, fd_01_ctx);
    		            while(n > 0) {
    		                tBroker_topic_read(TOPIC_1, fd_01_ctx, dummy_sub_data);
                            // fputs(dummy_sub_data, fd_00_1);
                            header = strtol(dummy_sub_data,NULL,10);
                            foooter = strtol(dummy_sub_data+500,NULL,10);
                            if ((header == (fd_01_count+200)) &&  (foooter == (fd_01_count+200)));
                            else 
                                printf("n");
                            n--;
                            fd_01_count++;
    		            }
    		        }
    		        else if (events[i].data.fd == fd_02) {
    		            n = tBroker_topic_peek(TOPIC_2, fd_02_ctx);
    		            while(n > 0) {
    		                tBroker_topic_read(TOPIC_2, fd_02_ctx, dummy_sub_data);
                            // fputs(dummy_sub_data, fd_00_2);
                            header = strtol(dummy_sub_data,NULL,10);
                            middle = strtol(dummy_sub_data+200,NULL,10);
                            foooter = strtol(dummy_sub_data+500,NULL,10);
                            if ((header == (fd_02_count+300)) &&  
                                (foooter == (fd_02_count+300)) &&
                                (middle == (fd_02_count+300)));
                            else 
                                printf("n");
                            n--;
                            fd_02_count++;
    		            }
    		        }
    		        else if (events[i].data.fd == fd_03) {
    		            n = tBroker_topic_peek(TOPIC_3,  fd_03_ctx);
    		            while(n > 0) {
    		                tBroker_topic_read(TOPIC_3,  fd_03_ctx, dummy_sub_data);
                            // fputs(dummy_sub_data, fd_00_3);
                            header = strtol(dummy_sub_data,NULL,10);
                            middle = strtol(dummy_sub_data+200,NULL,10);
                            foooter = strtol(dummy_sub_data+500,NULL,10);
                            if ((header == (fd_03_count+400)) &&  
                                (middle == (fd_03_count + 400)) &&
                                (foooter == (fd_03_count+400)));
                            else 
                                printf("n");
                            n--;
                            fd_03_count++;
    		            }
    		        }
    		        else if (events[i].data.fd == exit_0_fd) {
			            read(exit_0_fd, &exit0, 8);
    		            break;
			        }
    		    }
    		}
        }
        
        printf("fd_00_count = %d, fd_01_count = %d, fd_02_count = %d, fd_03_count = %d \r\n", fd_00_count, fd_01_count, fd_02_count, fd_03_count);
  
        if (fd_00 > 0) close(fd_00); 
        if (fd_01 > 0) close(fd_01); 
        if (fd_02 > 0) close(fd_02); 
        if (fd_03 > 0) close(fd_03);
        if (epoll_fd > 0) close(epoll_fd);
        // fclose(fd_00_0); fclose(fd_00_1); fclose(fd_00_2); fclose(fd_00_3);   
        
    }
}
