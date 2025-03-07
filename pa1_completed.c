/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/*
Please specify the group members here

# Student #1: Jack Smoroske
# Student #2: Yoshinobu Tabita
# Student #3: Ibrahim Amjad

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

/*
 * This structure is used to store per-thread data in the client
 */
typedef struct
{
    int epoll_fd;        /* File descriptor for the epoll instance, used for monitoring events on the socket. */
    int socket_fd;       /* File descriptor for the client socket connected to the server. */
    long long total_rtt; /* Accumulated Round-Trip Time (RTT) for all messages sent and received (in microseconds). */
    long total_messages; /* Total number of messages sent and received. */
    float request_rate;  /* Computed request rate (requests per second) based on RTT and total messages. */
} client_thread_data_t;

/*
 * This function runs in a separate client thread to handle communication with the server
 */
void *client_thread_func(void *arg)
{
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP"; /* Send 16-Bytes message every time */
    char recv_buf[MESSAGE_SIZE];
    struct timeval start, end;
    
    // register client socket
    event.events = EPOLLIN | EPOLLOUT;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) != 0)
    {
        // handle errors
        perror("Epoll control failed");
        return NULL;
    }

    // send num_requests requests
    for (int i = 0; i < num_requests; i++)
    {
        if (gettimeofday(&start, NULL) == -1)
        {
            // handle errors
            perror("Get time of day failed");
            return NULL;
        } 
        
        // send message
        if (send(data->socket_fd, send_buf, MESSAGE_SIZE, 0) == -1)
        {
            // handle errors
            perror("Send failed");
            return NULL;
        }
    
        // wait for epoll response
        int n;
        if ((n = epoll_wait(data->epoll_fd, &events, MAX_EVENTS, -1)) == -1)
        {
            // handle errors
            perror("Epoll wait failed");
            return NULL;
        }
        for (int j = 0; j < n; j++)
        {
            // our socket is ready to recieve
            if (events[j].data.fd == data->socket_fd)
            {
                // recieve response
                if (recv(data->socket_fd, recv_buf, MESSAGE_SIZE, 0) == -1)
                {
                    // handle errors
                    perror("Receive failed");
                    return NULL;
                }
            }
        }

        // measure RTT & update data
        if (gettimeofday(&end, NULL) == -1)
        {
            perror("Get time of day failed");
            return NULL;
        } 
        data->total_rtt += ((end.tv_sec - start.tv_sec) * 1000000) + (end.tv_usec - start.tv_usec);
        data->total_messages++;
    }
    
    // calculate request rate and return
    data->request_rate = (data->total_rtt / 1000000.f) / data->total_messages;
    return NULL;
}

/*
 * This function orchestrates multiple client threads to send requests to a server,
 * collect performance data of each threads, and compute aggregated metrics of all threads.
 */
void run_client()
{
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    /* TODO:
     * Create sockets and epoll instances for client threads
     * and connect these sockets of client threads to the server
     */
    //set address structure
    memset(&server_addr, 0, sizeof(server_addr);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    //setup data for each thread
    for(int i =0; i<num_client_threads; i++) {
        //socket creation
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) { //error handling
            perror("Socket creation has failed");
            exit(EXIT_FAILURE);
        }
    // epoll instance
        int epollfd = epoll_create1(0);
        if(epollfd<0) {
            perror("Epoll creation failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }
        //connect and check at the same time for failure
        if(connect(sockfd,(struct sockaddr *)&server_addr, sizeof(server_addr))<0){
            perror("Connection failed");
            close(sockfd);
            close(epollfd);
            exit(EXIT_FAILURE);
        }
        struct epoll_event ev; //Socket epoll monitoring
        ev.events = EPOLLIN;
        ev.data.fd = sockfd;
        if(epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &ev)<0){
            perror("Epoll control failed");
            close(sockfd);
            close(epollfd);
            exit(EXIT_FAILURE);
        }
        //store all the data here
        thread_data[i].sockfd = sockfd;
        thread_data[i].epollfd = epollfd;
        thread_data[i].thread_id = i;
        //initialize statistics variables
        thread_data[i].rtt_sum = 0;
        thread_data[i].messages_sent = 0;
        thread_data[i].messages_received = 0;
    }
    // Hint: use thread_data to save the created socket and epoll instance for each thread
    // You will pass the thread_data to pthread_create() as below
    for (int i = 0; i < num_client_threads; i++)
    {
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    /* TODO:
     * Wait for client threads to complete and aggregate metrics of all client threads
     */
//waiting part
    for(int i = 0; i<num_client_threads;i++){
        pthread_join(threads[i],NULL);
    }
    //aggregate metrics
    long long total_rtt = 0;
    int total_messages = 0;
    double total_request_rate = 0.0;
    for (int i = 0; i< num_client_threads; i++) {
        total_rtt += thread_data[i].rtt_sum;
        total_messages += thread_data[i].messages_received;
        total_request_rate += thread_data[i].request_rate;
    }
    printf("Average RTT: %lld us\n", total_rtt / total_messages);
    printf("Total Request Rate: %f messages/s\n", total_request_rate);
}

void run_server()
{
    // set up variables
    int server_fd, epoll_fd, new_socket;
    struct epoll_event event, events[MAX_EVENTS]; // define epoll and events
    struct sockaddr_in channel; // define domain socket 
    socklen_t channel_len = sizeof(channel); // for new socket creation

    // create listening socket
    server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd == -1)
    {
        perror("Listening socket creation failed");
        return;
    }
    
    // define attributes of socket address struct
    channel.sin_family = AF_INET;
    channel.sin_port = htons(server_port);

    // convert the server ip from text to binary
    if (inet_pton(AF_INET, server_ip, &channel.sin_addr) <= 0)
    {
        perror("Conversion of ip-address failed");
        close(server_fd);
        return;
    }

    // assigns address for the socket
    if (bind(server_fd, (struct sockaddr*)&channel, sizeof(channel)) == -1)
    {
        perror("Bind failed");
        close(server_fd);
        return;
    }

    // allows for accepting incoming requests
    if (listen(server_fd, DEFAULT_CLIENT_THREADS) == -1)
    {
        perror("Listen failed");
        close(server_fd);
        return;
    }

    // create epoll instance
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1)
    {
        perror("Epoll instance creation failed");
        close(server_fd);
        return;
    }

    // register listening socket to epoll
    event.events = EPOLLIN; // allows read
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1)
    {
        perror("Epoll control failed");
        close(server_fd);
        close(epoll_fd);
        return;
    }

    /* Server's run-to-completion event loop */
    while (1)
    {
        // wait for events on the epoll
         int event_count = epoll_wait(epoll_fd, &events, MAX_EVENTS, -1);

         if (event_count == -1)
         {
            perror("Epoll wait failed");
            break;
         }

         for (int i = 0; i < event_count; i++)
         {
            // case where a new socket needs to be made for the client
            if (events[i].data.fd == server_fd) 
            {
                new_socket = accept(server_fd, (struct sockaddr*)&channel, &channel_len); // creates a new connected socket 

                if (new_socket == -1)
                {
                    perror("Accept failed");
                    break;
                }

                // need to register the new socket to the epoll
                event.events = EPOLLIN; // can read
                event.data.fd = new_socket;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_socket, &event) == -1)
                {
                    perror("Adding new socket failed");
                    close(new_socket);
                    break;
                }
            }
            
            // two branches, if the established connection has nothing in it then close, if it does then echo the message back 
            else 
            {
                char buffer[MESSAGE_SIZE]; // buffer for the message to be held in
                int read_in = read(events[i].data.fd, buffer, MESSAGE_SIZE); // read in the message

                if (read_in <= 0) // close the socket
                {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                    close(events[i].data.fd);
                }

                else // echo the message back
                {
                    write(events[i].data.fd, buffer, read_in); 
                }
            }
         }
    }

    close(server_fd);
    close(epoll_fd);
    return;
}

int main(int argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "server") == 0)
    {
        if (argc > 2)
            server_ip = argv[2];
        if (argc > 3)
            server_port = atoi(argv[3]);

        run_server();
    }
    else if (argc > 1 && strcmp(argv[1], "client") == 0)
    {
        if (argc > 2)
            server_ip = argv[2];
        if (argc > 3)
            server_port = atoi(argv[3]);
        if (argc > 4)
            num_client_threads = atoi(argv[4]);
        if (argc > 5)
            num_requests = atoi(argv[5]);

        run_client();
    }
    else
    {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}
