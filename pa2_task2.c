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
#define MAX_SEQ 1
#define MAX_THREADS 500

typedef unsigned int seq_nr;

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

/*
 * This structure is used to store per-thread data in the client
 */
typedef struct
{
    int thread_id;       /* ID for this thread instance */
    int epoll_fd;        /* File descriptor for the epoll instance, used for monitoring events on the socket. */
    int socket_fd;       /* File descriptor for the client socket connected to the server. */
    struct sockaddr_in server_addr; /* Server address struct */
    long long total_rtt; /* Accumulated Round-Trip Time (RTT) for all messages sent and received (in microseconds). */
    long total_messages; /* Total number of messages sent and received. */
    float request_rate;  /* Computed request rate (requests per second) based on RTT and total messages. */
    int packets_lost;    /* Total number of packets lost by this thread. */
} client_thread_data_t;

/*
 * This structure holds data and metadata transferred between the client and server
 */
typedef struct {
    int type;                   /* 0 = data, 1 = ack, 2 = nack */
    int thread_id;              /* id of client thread */
    seq_nr seq;                 /* sequence number */
    seq_nr ack;                 /* acknowledgement number */
    char data[MESSAGE_SIZE];    /* data */
} frame;

/*
 * This function runs in a separate client thread to handle communication with the server
 */
void *client_thread_func(void *arg)
{
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    char buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP"; /* Send 16-Bytes message every time */
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

    // main frame
    frame f;

    // packet loss metrics
    int packets_sent = 0;
    int acks_received = 0;

    // sequence numbers for SN/ARQ method
    seq_nr next_seqnr_to_send = 0;

    // send num_requests requests
    for (int i = 0; i < num_requests; i++)
    {
        if (gettimeofday(&start, NULL) == -1)
        {
            // error - skip this packet
            perror("Gettimeofday failure");
            continue;
        } 
        
        // create frame to send
        f.type = 0;
        f.thread_id = data->thread_id;
        f.seq = next_seqnr_to_send;
        memcpy(f.data, buf, MESSAGE_SIZE);

        // send frame to server
        if (sendto(data->socket_fd, &f, sizeof(f), 0, (struct sockaddr*)&data->server_addr, server_addr_len) == -1)
        {
            // error - skip this packet
            perror("Send failure");
            continue;
        }
        else
        {
            // packet successfully sent
            packets_sent++;
        }
    
        // enter timeout period
        int n = epoll_wait(data->epoll_fd, events, MAX_EVENTS, THREAD_TIMEOUT);
        if (n == -1)
        {
            // error - can't receive
            perror("Epoll wait failure");
            continue;
        }
        else if (n == 0)
        {
            // timeout = retransmit (dont update seqnr, same packet again)
            packets_sent--;
            continue;
        }

        // find our socket among epoll_events
        for (int j = 0; j < n; j++)
        {
            // our socket is ready to receive
            if (events[j].data.fd == data->socket_fd)
            {
                // receive response
                if (recvfrom(data->socket_fd, &f, sizeof(f), 0, (struct sockaddr*) &data->server_addr, &server_addr_len) == -1)
                {
                    // error when receiving
                    perror("Recv failure");
                }
                else
                {
                    // ack received, verify that it is correct
                    if (f.ack == next_seqnr_to_send)
                    {
                        // increment seqnr
                        if (next_seqnr_to_send < MAX_SEQ)
                        {
                            next_seqnr_to_send++;
                        }
                        else
                        {
                            next_seqnr_to_send = 0;
                        }
                        acks_received++;
                    }
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
    data->packets_lost = packets_sent - acks_received;
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
        thread_data[i].thread_id = i;
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
    // final packets comparison
    if (total_packets_lost == 0)
    {
        printf("No packets lost\n");
    }
    else
    {
        printf("%lld packets lost\n", total_packets_lost);
    }
}

void run_server()
{
    // set up variables
    int server_fd, epoll_fd, new_socket;
    struct epoll_event event, events[MAX_EVENTS]; // define epoll and events
    struct sockaddr_in channel; // define domain socket 
    socklen_t channel_len = sizeof(channel); // for new socket creation
    seq_nr frame_expected[MAX_THREADS] = {0}; // expected sequence number tracked for the client id
    
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
            // case where a new socket needs to be made for the client
            if (events[i].data.fd == server_fd) 
            {
                struct sockaddr_in client; // one socket created for the client for UDP
                socklen_t client_len = sizeof(client); // for the client socket
                frame client_packet, ack_packet; // define packets for client and acknowledgement

                // receive message into the client packet
                ssize_t rec = recvfrom(server_fd, &client_packet, sizeof(client_packet), 0, (struct sockaddr*)&client, &client_len);

                if (rec == -1)
                {
                    perror("Receive message failed");
                    continue;
                }

                int client_thread_id = client_packet.thread_id; // get the clients thread id

                // check to see if the client id is in bounds
                if (client_thread_id < 0 || client_thread_id >= MAX_THREADS) 
                {
                    perror("Client thread id is not in the bounds of the threads threshold");
                    continue;
                }

                memset(&ack_packet, 0, sizeof(frame)); // reset the ack packet for a new client packet
                ack_packet.thread_id = client_thread_id; // set the ack packet thread_id to its client 

                // if the received packet has the expected sequence number for the client id
                if (client_packet.seq == frame_expected[client_thread_id])
                {
                    // increment sequence number 
                    if (frame_expected[client_thread_id] < MAX_SEQ)
                    {
                        frame_expected[client_thread_id] = frame_expected[client_thread_id] + 1;
                    }
                    else
                    {
                        frame_expected[client_thread_id] = 0;
                    }
                    
                    ack_packet.type = 1; // ack, since the expected sq num lined up     
                }
                
                else
                {
                    ack_packet.type = 2;  // nack, since the expected sq num didn't line up 
                }

                ack_packet.ack = 1 - frame_expected[client_thread_id]; // say which frame is being acked

                // send back the ack packet, not echoing the whole packet back
                ssize_t send = sendto(server_fd, &ack_packet, sizeof(ack_packet), 0, (struct sockaddr*)&client, client_len);

                if (send == -1)
                {
                    perror("Acknowledgement failed to transmit");
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
