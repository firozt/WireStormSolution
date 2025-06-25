#include <sys/socket.h> // unix sys call for threads
#include <sys/types.h> 
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h> 
#include <string.h> 
#include <thread>
#include <set>
#include <mutex>
#include <atomic>
/*
    REQUIREMENTS: 

    - Allow a single source client to connect on port 33333

    - Allow multiple destination clients to connect on port 44444

    - Accept CTMP messages over TCP from the source client, and broadcast to all destination clients. 
    Messages should be broadcast in the order that they are received from the source.

    - Validate the magic byte and length of the data to ensure it matches the header before broadcasting. 
    If the data exceeds the length specified in the header, the entire message should be dropped.

    0               1               2               3
    0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    | MAGIC 0xCC    | PADDING       | LENGTH                      |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    | PADDING                                                     |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    | DATA ...................................................... |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7

*/


#define SOURCE_PORT 33333
#define DESTINATION_PORT 44444


// shared resource between all threads, handled safely with mutex
std::mutex source_mutex;
std::mutex destination_mutex;

std::set<int> destination_clients;
bool source_client_connected = false;


enum CLIENT_TYPE {
    SOURCE,
    DESTINATION,
};


// handles the source clients actions
void source_listener(int conn_fd,int MAX) {
    char buff[MAX]; 

    while(1) { 

        // reset buffer
        bzero(buff, MAX); 
        
        // read the message from client and copy it in buffer 
        ssize_t msg_bytes = recv(conn_fd, buff, MAX, 0); // blocks execution
        if (msg_bytes <= 0) {
            if (msg_bytes == 0) {
                printf("Source client has disconnected gracefully\n");
            } else {
                perror("Source client recv error\n");
            }
            break;
        }
        std::lock_guard<std::mutex> lock(destination_mutex);
        for (int conn : destination_clients) {
            // send msg to all destination clients
            if (send(conn, buff, msg_bytes, 0) < 0) {
                perror("Unable to send message to desination client");
                break;
            } 
        }
    } 
} 


// handles the destination clients actions
void destination_listener(int conn_fd, int MAX) {
    char buff[MAX]; 
    while (true) {
        bzero(buff, MAX); 
        ssize_t bytes_read = recv(conn_fd, buff, MAX, 0); // check if client is still connected
        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                printf("Destination client %d has disconnected gracefully\n", conn_fd);
            } else {
                perror("Destination client recv error\n");
            }
            break;
        }
    }
}


// creates a listening socket
void create_listening_port(CLIENT_TYPE client) {
    // 1) create socket
    // socket with ipv4 protocol (af_inet), via tcp (sock_stream)
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    // socket_fd should be > 0 to be valid else error
    if (socket_fd < 0) {
        printf("Socket creation failed\n");
        return;
    }

    // 2) bind created socket
    // creating socket addresss struct with valid info
    uint16_t port = (client == SOURCE) ? SOURCE_PORT : DESTINATION_PORT;

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    int addr_len = sizeof(address);

    // 3) bind socket with address and port

    int bind_status = bind(socket_fd, (struct sockaddr*) &address, (socklen_t)addr_len);

    if (bind_status < 0) {
        printf("Unable to bind socket for port %d\n",port);
        return;
    }

    // 4) listen to binded socket at client port

    // takes socket field descriptor and a max len of the queue
    int listen_status = listen(socket_fd, 10);

    if (listen_status < 0) {
        printf("Unable to create a listener for socket");
        return;
    }

    printf("Server listening on port %d\n, awaiting connections.", port);


    // 5) accept connection

    // original thread will wait for connections, once one is requested new thread
    // is spawned and will handle broadcasted messages from source thread
    // number of destination clients will be handled by the destination client list
    if (client == SOURCE) {
        while(1) {
            int conn_fd = accept(socket_fd, (struct sockaddr*) &address, (socklen_t*)&addr_len); 
            if (conn_fd < 0) { 
                printf("Failed to accept a source client\n");
                continue;
            } 
            {
                // mutex lock check
                std::lock_guard<std::mutex> lock(source_mutex);
                if (source_client_connected) {
                    const char* err_msg = "Error: Source client already connected.\n";
                    send(conn_fd, err_msg, strlen(err_msg), 0);
                    close(conn_fd);
                    continue;  // reject this connection
                }
                source_client_connected = true;
            }

            std::thread t([conn_fd]() {
                source_listener(conn_fd, 100);
                {
                    std::lock_guard<std::mutex> lock(source_mutex);
                    source_client_connected = false;
                }
                close(conn_fd);
            });
            t.detach();
        }

    }
    // new thread for the source client, original thread will check if a source is connected
    // via the source connected flag, to ensure only one source client at a given time
    if (client == DESTINATION) {   
        while (true) {
            int conn_fd = accept(socket_fd, (struct sockaddr*) &address, (socklen_t*)&addr_len);
            if (conn_fd < 0) {
                printf("Failed to accept a destination client\n");
                continue;
            }

            {
                // add new client to list safely
                std::lock_guard<std::mutex> lock(destination_mutex);
                destination_clients.insert(conn_fd);
                printf("Number of destination clients: %zu\n", destination_clients.size());
            }

            std::thread t([conn_fd]() {
                destination_listener(conn_fd, 100);
                {
                    // remove destination client safely (on client termination)
                    std::lock_guard<std::mutex> lock(destination_mutex);
                    destination_clients.erase(conn_fd);
                    printf("Number of destination clients: %zu\n", destination_clients.size());
                }
                close(conn_fd);
            });
            t.detach();
        }
    }

    close(socket_fd);
    return;
}




int main() {
    // create listening ports on two seperare threads

    std::thread source_thread([]() {
        create_listening_port(SOURCE);
    });


    std::thread destination_thread([](){
        create_listening_port(DESTINATION);
    });

    source_thread.join();
    destination_thread.join();
    
    return 1;
}