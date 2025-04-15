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

#define THREAD_TIMEOUT 1000

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
    struct sockaddr_in server_addr; /* Server address struct */
    long long total_rtt; /* Accumulated Round-Trip Time (RTT) for all messages sent and received (in microseconds). */
    long total_messages; /* Total number of messages sent and received. */
    float request_rate;  /* Computed request rate (requests per second) based on RTT and total messages. */
    long long packets_lost;    /* Total number of packets lost by this thread. */
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

    socklen_t server_addr_len = sizeof(data->server_addr);
    
    // register client socket
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) != 0)
    {
        // handle errors
        perror("Epoll control failed");
        close(data->socket_fd);
        close(data->epoll_fd);
        return NULL;
    }

    // packet loss metrics
    long long packets_sent = 0;
    long long packets_received = 0;

    // send num_requests requests
    for (int i = 0; i < num_requests; i++)
    {
        if (gettimeofday(&start, NULL) == -1)
        {
            // error - skip this packet
            perror("Gettimeofday failure");
            continue;
        } 
        
        // send message
        if (sendto(data->socket_fd, send_buf, MESSAGE_SIZE, 0, (struct sockaddr*)&data->server_addr, server_addr_len) == -1)
        {
            // error - skip this packet
            perror("Send failure");
            continue;
        }
        else
        {
            packets_sent++;
        }
    
        // wait for epoll response
        int n = epoll_wait(data->epoll_fd, events, MAX_EVENTS, THREAD_TIMEOUT);
        if (n == -1)
        {
            // error - can't receive
            perror("Epoll wait failure");
            continue;
        }
        else if (n == 0)
        {
            // timeout 
            continue;
        }

        // find our socket among epoll_events
        for (int j = 0; j < n; j++)
        {
            // our socket is ready to receive
            if (events[j].data.fd == data->socket_fd)
            {
                // receive response
                if (recvfrom(data->socket_fd, recv_buf, MESSAGE_SIZE, 0, (struct sockaddr*) &data->server_addr, &server_addr_len) == -1)
                {
                    // error when receiving
                    perror("Recv failure");
                }
                else
                {
                    packets_received++;
                }
            }
        }

        // measure RTT & update data
        if (gettimeofday(&end, NULL) == -1)
        {
            // error - can't update metrics
            perror("Gettimeofday failure");
        } 
        else
        {
            data->total_rtt += ((end.tv_sec - start.tv_sec) * 1000000) + (end.tv_usec - start.tv_usec);
        }
        data->total_messages++;
    }
    
    // calculate metrics and return
    close(data->socket_fd);
    close(data->epoll_fd);
    data->request_rate = data->total_messages / (data->total_rtt / 1000000.f);
    data->packets_lost = packets_sent - packets_received;
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

    // set address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    // setup data for each thread
    for (int i = 0; i < num_client_threads; i++) 
    {
        // socket creation
        int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd < 0) 
        { 
            perror("Socket creation has failed");
            continue;
        }

        // epoll instance
        int epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) 
        {
            perror("Epoll creation failed");
            close(socket_fd);
            continue;
        }

        // store thread data here
        thread_data[i].socket_fd = socket_fd;
        thread_data[i].epoll_fd = epoll_fd;
        thread_data[i].server_addr = server_addr;
        // initialize statistics variables
        thread_data[i].total_rtt = 0;
        thread_data[i].total_messages = 0;
        thread_data[i].packets_lost = 0;
    }

    // create threads
    for (int i = 0; i < num_client_threads; i++)
    {
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    // wait for threads to complete
    for (int i = 0; i < num_client_threads; i++)
    {
        pthread_join(threads[i],NULL);
    }

    // aggregate metrics
    long long total_rtt = 0;
    int total_messages = 0;
    double total_request_rate = 0.0;
    long long total_packets_lost = 0;

    for (int i = 0; i < num_client_threads; i++) 
    {
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
        total_packets_lost += thread_data[i].packets_lost;
    }

    printf("Average RTT: %lld us\n", total_rtt / total_messages);
    printf("Total Request Rate: %f messages/s\n", total_request_rate);
    printf("Total Packets Lost: %lld packets\n",total_packets_lost);
}
void run_server()
{
    // set up variables
    int server_fd, epoll_fd, new_socket;
    struct epoll_event event, events[MAX_EVENTS]; // define epoll and events
    struct sockaddr_in channel; // define domain socket 
    socklen_t channel_len = sizeof(channel); // for new socket creation

    // create socket for UDP
    server_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); 
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

    // assigns address for the socket, no need for listen on UDP
    if (bind(server_fd, (struct sockaddr*)&channel, sizeof(channel)) == -1)
    {
        perror("Bind failed");
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
         int event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

         if (event_count == -1)
         {
            perror("Epoll wait failed");
            break;
         }

         for (int i = 0; i < event_count; i++)
         {
            if (events[i].data.fd == server_fd) 
            {
                struct sockaddr_in client; // one socket created for the client for UDP
                socklen_t client_len = sizeof(client); // for the client socket
                char buffer[MESSAGE_SIZE]; // buffer for the message to be held in

                // receive message into the buffer
                ssize_t rec = recvfrom(server_fd, buffer, MESSAGE_SIZE, 0, (struct sockaddr*)&client, &client_len);

                if (rec == -1)
                {
                    perror("Receive message failed");
                    continue;
                }

                // echo that message from the buffer
                ssize_t send = sendto(server_fd, buffer, rec, 0, (struct sockaddr*)&client, client_len);

                if (send == -1)
                {
                    perror("Send messsage failed");
                    continue;
                }
            }
        }
    }

    // close fds and return
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
