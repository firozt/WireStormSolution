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



// Thread safe list of connections
class ConnectionManager {
private:
    std::mutex mutex;
    std::set<int> clients;
    int max_connections;
public:

    ConnectionManager(int max_connections) {
        this->max_connections = max_connections;
    };


    int get_max_connections() {
        return max_connections;
    }

    bool add(int fd) {
        if (this->size() >= this->max_connections) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex);
        clients.insert(fd);
        return true;
    }

    void remove(int fd) {
        std::lock_guard<std::mutex> lock(mutex);
        clients.erase(fd);
    }

    std::set<int> get_all() {
        std::lock_guard<std::mutex> lock(mutex);
        return clients; 
    }



    size_t size() {
        std::lock_guard<std::mutex> lock(mutex);
        return clients.size();
    }
};

// TODO
class CTMPMessageValidator {
    public:
    static bool validate(const char* data, int len);
};

enum CLIENT_TYPE {
    SOURCE,
    DESTINATION,
};

class ServerListener {
    private:
    const uint16_t port;
    const CLIENT_TYPE type;
    ConnectionManager connections;
    /**
    * @brief Creates a socket with the classes set port and a given address.
    *
    * Creates a socket with a set port and address using the ipv4 (af_inet) protocol
    * socket is designed for TCP connections.
    *
    * @param address  socket address internet object containing all necessary information.
    * @param addr_len size of the address object.
    * @return Returns the socket field descriptor created with the socket sys call if 
    * successfull, otherwise returns -1, and perrors the error.
    */
    int create_socket(sockaddr_in address, int addr_len) {
        // 1) create socket
        // socket with ipv4 protocol (af_inet), via tcp (sock_stream)
        int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        // socket_fd should be > 0 to be valid else error
        if (socket_fd < 0) {
            perror("Socket creation failed\n");
            return -1;
        }

        // 2) bind socket with address and port
        int bind_status = bind(socket_fd, (struct sockaddr*) &address, (socklen_t)addr_len);

        if (bind_status < 0) {
            printf("Unable to bind socket for port %d\n",port);
            return -1;
        }
        // 3) listen to binded socket at client port
        // takes socket field descriptor and a max len of the queue
        int listen_status = listen(socket_fd, 10);

        if (listen_status < 0) {
            perror("Unable to create a listener for socket");
            return -1;
        }
        return socket_fd;
    }


    void source_listener(int conn_fd,int MAX, ConnectionManager* destination_clients) {
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
            for (int conn : destination_clients->get_all()) {
                if (send(conn, buff, msg_bytes, 0) < 0) {
                    perror("Unable to send message to destination client");
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

    public:

    ConnectionManager* get_connections() {
        return &connections;
    }

    ServerListener(uint16_t port, CLIENT_TYPE type, int max_connections)
        : port(port), type(type), connections(max_connections) {};

        

    // creates a listening socket
    void run(ConnectionManager* destination_clients=nullptr){
        // type source must also pass in a list of destination clients to send the messages to
        if (type == SOURCE && destination_clients == nullptr) {
            fprintf(stderr, "Error: SOURCE listener requires a destination ConnectionManager.\n");
            return;
        }


        // create address object
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);
        int addr_len = sizeof(address);

        // create socket and prepares for listening
        int socket_fd = create_socket(address, addr_len);
        if (socket_fd < 0) {
            perror("Could not create socket");
            return;
        }
        
        printf("Server listening on port %d awaiting connections.\n", port);
        // accept connection
        // original thread will wait for connections, once one is requested new thread
        // is spawned and will handle broadcasted messages from source thread
        // number of destination clients will be handled by the destination client list
        
        while(1) {
            int conn_fd = accept(socket_fd, (struct sockaddr*) &address, (socklen_t*)&addr_len); 
            if (conn_fd < 0) { 
                perror("Failed to accept a source client\n");
                continue;
            } 

            // attempts to add the connection
            // -1 indicates infinite number of connections
            if (connections.size() >= connections.get_max_connections() and connections.get_max_connections() != -1) {
                const char* err_msg = "Error: Source client already connected.\n";
                send(conn_fd, err_msg, strlen(err_msg), 0);
                close(conn_fd);
                continue;  // reject this connection
            }
            
            // accept this connection
            connections.add(conn_fd);
            std::thread t([this,conn_fd, destination_clients]() {
                // use corresponding handler
                if (type == SOURCE) this->source_listener(conn_fd, 100, destination_clients);
                if (type == DESTINATION) this->destination_listener(conn_fd, 100);
                // cleanup socket from connection list
                connections.remove(conn_fd);
                close(conn_fd);
            });
            t.detach();
        }
        close(socket_fd);
    }
};


int main() {
    // create two listening servers, one for source and destination
    ServerListener source(SOURCE_PORT,SOURCE, 1);
    ServerListener destination(DESTINATION_PORT, DESTINATION, -1);

    // run the listeners for each on their own thread to handle connections
    // pass in the connections from the destination to source, so it can send the messages to them
    std::thread src_thread([&](){ source.run(destination.get_connections()); });
    std::thread dst_thread([&](){ destination.run(); });

    src_thread.join();
    dst_thread.join();
    
    return 1;
}