import ctypes as ct
import time
import threading
import select

#Some tooling can be used to autoconvert #defines into this
TOPIC_0 = 0
TOPIC_1 = 1
TOPIC_2 = 2
TOPIC_3 = 3
TOPIC_4 = 4
TOPIC_5 = 5
TOPIC_6 = 6
TOPIC_7 = 7

# open our library dll, first open required sys libraries 
ct.CDLL('librt.so', mode=ct.RTLD_GLOBAL)
ct.CDLL('libpthread.so.0', mode=ct.RTLD_GLOBAL) 
tBrok = ct.CDLL('./libtBrok.so')

# create interface to call necessary dll functions from python

# 'int tBroker_connect(void)' in "tBroker.h"
tBroker_connect = tBrok.tBroker_connect
tBroker_connect.restype = ct.c_int

# 'int tBroker_disconnect(void)' in "tBroker.h"
tBroker_disconnect = tBrok.tBroker_disconnect
tBroker_disconnect.restype = ct.c_int

# 'int topic_subscribe(int topic)' in "tBroker.h"
topic_subscribe = tBrok.topic_subscribe
topic_subscribe.argtypes = [ct.c_int]
topic_subscribe.restype = ct.c_int

# 'int topic_publish(int topic, void *buffer)' in "tBroker.h"
topic_publish = tBrok.topic_publish
topic_publish.argtypes = [ct.c_int, ct.c_void_p]
topic_publish.restype = ct.c_int

# 'int topic_peek(int topic, int handle)' in "tBroker.h"
topic_peek = tBrok.topic_peek
topic_peek.argtypes = [ct.c_int, ct.c_int]
topic_peek.restype = ct.c_int

# 'int topic_read(int topic, int handle, void *buffer)' in "tBroker.h"
topic_read = tBrok.topic_read
topic_read.argtypes = [ct.c_int, ct.c_int, ct.c_void_p]
topic_read.restype = ct.c_int


fd0_count = 0
fd1_count = 0
fd2_count = 0
fd3_count = 0

#https://stackoverflow.com/questions/4145775/how-do-i-convert-a-python-list-into-a-c-array-by-using-ctypes
#topic_data = bytearray(256)
#topic_data_ptr = (ct.c_byte * 256)(*topic_data)

topic_data = (ct.c_byte * 64)()
topic_data_ptr = ct.byref(topic_data)

topic_pub_data = (ct.c_byte * 64)()
topic_pub_data_ptr = ct.byref(topic_pub_data)

def publish4_func():
        loop = 10000
        time.sleep(2)
        while loop > 0:
                time.sleep(0.01)
                topic_publish(TOPIC_4, topic_pub_data_ptr)
                loop = loop - 1
        
def publish5_func():
        loop = 10000
        time.sleep(2)
        while loop > 0:
                time.sleep(0.01)
                topic_publish(TOPIC_5, topic_pub_data_ptr)
                loop = loop - 1
        
def publish6_func():
        loop = 10000
        time.sleep(2)
        while loop > 0:
                time.sleep(0.01)
                topic_publish(TOPIC_6, topic_pub_data_ptr)
                loop = loop - 1
        
def publish7_func():
        loop = 10000
        time.sleep(2)
        while loop > 0:
                time.sleep(0.01)
                topic_publish(TOPIC_7, topic_pub_data_ptr)
                loop = loop - 1
        
t4 = threading.Thread(target=publish4_func)
t4.start()

t5 = threading.Thread(target=publish5_func)
t5.start()

t6 = threading.Thread(target=publish6_func)
t6.start()

t7= threading.Thread(target=publish7_func)
t7.start()


conn = tBroker_connect()
if conn == 0:
        inputs = []             #select call infds
        outputs = []            #select call outfds
        
        print("Connected")
        fd0 = topic_subscribe(TOPIC_0)
        fd1 = topic_subscribe(TOPIC_1)
        fd2 = topic_subscribe(TOPIC_2)
        fd3 = topic_subscribe(TOPIC_3)
        
        inputs.append(fd0)
        inputs.append(fd1)
        inputs.append(fd2)
        inputs.append(fd3)
        
        try:
                while True:
                        #events = e.poll() , using poll or epoll is resulting in extremely high CPU usage
                        readable, writable, exceptional = select.select(inputs, outputs, inputs)
                        for fd in readable:
                                if fd == fd0:
                                        n = topic_peek(TOPIC_0, fd0)
                                        while n > 0:
                                                topic_read(TOPIC_0, fd0, topic_data_ptr)
                                                fd0_count = fd0_count + 1
                                                n = n - 1
                                elif fd == fd1:
                                        n = topic_peek(TOPIC_1, fd1)
                                        while n > 0:
                                                topic_read(TOPIC_1, fd1, topic_data_ptr)
                                                n = n - 1
                                                fd1_count = fd1_count + 1
                                elif fd == fd2:
                                        n = topic_peek(TOPIC_2, fd2)
                                        while n > 0:
                                                topic_read(TOPIC_2, fd2, topic_data_ptr)
                                                n = n - 1
                                                fd2_count = fd2_count + 1
                                elif fd == fd3:
                                        n = topic_peek(TOPIC_3, fd3)
                                        while n > 0:
                                                topic_read(TOPIC_3, fd3, topic_data_ptr)
                                                n = n - 1
                                                fd3_count = fd3_count + 1
        finally:
                print('fd0_count = ' + str(fd0_count))
                print('fd1_count = ' + str(fd1_count))
                print('fd2_count = ' + str(fd2_count))
                print('fd3_count = ' + str(fd3_count))
                discon = tBroker_disconnect()
                if discon == 0:
                        print("Disconnected")
                t4.join()
                t5.join()
                t6.join()
                t7.join()
                
