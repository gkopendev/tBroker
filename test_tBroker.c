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
#define Q_SIZE      64
#define LOOP        (1024 * 100)

#define TOPIC_0 0
#define TOPIC_1 1
#define TOPIC_2 2
#define TOPIC_3 3
#define TOPIC_4 4
#define TOPIC_5 5
#define TOPIC_6 6
#define TOPIC_7 7

uint64_t exit_app = 0;
struct timespec ts = {0, 5000000L};

pthread_t   All_topic_pub1, All_topic_pub2, All_topic_pub3;
void *All_topic_pub_func1(void *par)
{
    int times = 0;
    uint8_t dummy_pub_data[BUF_SIZE];

    sleep(10);
    printf("S - 0 \r\n");
    while (times < LOOP) {
        sprintf(dummy_pub_data, "%d \r\n", (times+100));
        sprintf(dummy_pub_data+200, "%d \r\n", (times+100));
        sprintf(dummy_pub_data+500, "%d \r\n", (times+100));
        tBroker_topic_publish(TOPIC_0, dummy_pub_data);
        times++;
        nanosleep(&ts, NULL);
    }
    sleep(5);
}

void *All_topic_pub_func2(void *par)
{
    int times = 0;
    uint8_t dummy_pub_data[BUF_SIZE];
    
    sleep(10);
     printf("S - 1 \r\n");
    while (times < LOOP) {
        sprintf(dummy_pub_data, "%d \r\n", (times+200));
        sprintf(dummy_pub_data+200, "%d \r\n", (times+200));
        sprintf(dummy_pub_data+500, "%d \r\n", (times+200));
        tBroker_topic_publish(TOPIC_1, dummy_pub_data);
        times++;
        nanosleep(&ts, NULL);
    }
     sleep(5);
}

void *All_topic_pub_func3(void *par)
{
    int times = 0;
    uint8_t dummy_pub_data[BUF_SIZE];
    
    sleep(10);
    printf("S - 2 \r\n");
    while (times < LOOP) {
        sprintf(dummy_pub_data, "%d \r\n", (times+300));
        sprintf(dummy_pub_data+200, "%d \r\n", (times+300));
        sprintf(dummy_pub_data+500, "%d \r\n", (times+300));
        tBroker_topic_publish(TOPIC_2, dummy_pub_data);
        times++;
        nanosleep(&ts, NULL);
    }
     sleep(5);
}


void main(void)
{
    int times=0;
    pthread_t this_thread = pthread_self();
    pthread_attr_t my_attr;
    struct sched_param schedParam;
    uint8_t dummy_pub_data[BUF_SIZE];
    
    pthread_attr_init(&my_attr);
    schedParam.sched_priority = sched_get_priority_max(SCHED_FIFO) - 15;
    pthread_attr_setschedparam(&my_attr, &schedParam);
    pthread_attr_setschedpolicy(&my_attr, SCHED_FIFO);
    
    pthread_setschedparam(this_thread, SCHED_FIFO, &schedParam);
    mlockall(MCL_CURRENT|MCL_FUTURE);
    unsigned char dummy[8196];
    memset(dummy, 0, 8196);

    /* Init the broker */
    tBroker_init();
    
    if (tBroker_topic_create(TOPIC_0, "First topic", BUF_SIZE, Q_SIZE) >= 0);
    if (tBroker_topic_create(TOPIC_1, "Second topic", BUF_SIZE, Q_SIZE) >= 0);
    if (tBroker_topic_create(TOPIC_2, "Third topic", BUF_SIZE, Q_SIZE) >= 0);
    if (tBroker_topic_create(TOPIC_3, "Fourth topic", BUF_SIZE, Q_SIZE) >= 0);
    if (tBroker_topic_create(TOPIC_4, "fifth topic", BUF_SIZE, Q_SIZE) >= 0);
    if (tBroker_topic_create(TOPIC_5, "sixth topic", BUF_SIZE, Q_SIZE) >= 0);
    if (tBroker_topic_create(TOPIC_6, "seventh topic", BUF_SIZE, Q_SIZE) >= 0);
    if (tBroker_topic_create(TOPIC_7, "eigth topic", BUF_SIZE, Q_SIZE) >= 0);

    tBroker_connect();
    
    //sleep(5);
    
    //pthread_create(&All_topic_pub1, &my_attr, &All_topic_pub_func1, NULL);
    // pthread_create(&All_topic_pub2, &my_attr, &All_topic_pub_func2, NULL);
     pthread_create(&All_topic_pub3, &my_attr, &All_topic_pub_func3, NULL);
 
    sleep(10);
    
    /* this main thread simulates publishers */
    /* Publish on every topic */
     printf("S - 3 \r\n");
    while (times < LOOP) {

        sprintf(dummy_pub_data, "%d \r\n", (times+400));
        sprintf(dummy_pub_data+200, "%d \r\n", (times+400));
        sprintf(dummy_pub_data+500, "%d \r\n", (times+400));
        tBroker_topic_publish(TOPIC_3, dummy_pub_data);
        times++;
        nanosleep(&ts, NULL);
    }
    
    sleep(5);
    
    exit_app = 1;
    
    tBroker_disconnect();
    tBroker_deinit();
}
