#include <sys/socket.h> // unix sys call for threads
#include <sys/types.h> 
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <set>
#include <mutex>
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
#define MAX_BUFFER_SIZE 1024



// Thread safe list of connections
class ConnectionManager {
private:
    std::mutex mutex;
    std::set<int> clients;
    int max_connections;
public:
    ConnectionManager(int max_connections) : max_connections(max_connections) {}

    int get_max_connections() const {
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


// simple struct for representing the CTMP message
struct CTMPMessage {
    uint16_t payload_length;
    uint8_t* payload;
};

class CTMPMessageValidator {
    public:
    static bool validate(const uint8_t* data, size_t len) {
        constexpr uint8_t magic_byte = 0xCC;
        constexpr uint8_t padding_byte = 0x00;
        constexpr uint8_t header_size = 8;

        if (len < header_size) return false; // entire message cant be less than the header size

        // check magic byte and padding byte
        if (data[0] != magic_byte) return false;
        if (data[1] != padding_byte) return false;

        // check payload length
        uint16_t payload_length = get_payload_length(data);
        if (payload_length < 0) return false;


        // Check 4 padding bytes (bytes 4 to 7)
        for (int i = 4; i < 8; ++i) {
            if (data[i] != padding_byte) return false;
        }


        // if the data exceeds the length specified in header, return false
        uint16_t actual_payload_size = len - header_size;
        if (actual_payload_size > payload_length) return false;

        return true;
    }


    static uint16_t get_payload_length(const uint8_t* data) {
        // payload length cannot be in this data
        if (sizeof(data) < 4) {
            return -1;
        }
        // extract payload length from ctmp message
        // copy the next 2 bytes, convert it from network order.
        uint16_t payload_length_network_order;
        std::memcpy(&payload_length_network_order, &data[2], sizeof(uint16_t));
        uint16_t payload_length = ntohs(payload_length_network_order);
        return payload_length;
    }
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
    int create_socket(sockaddr_in address, int addr_len) const {
        // 1) create socket
        // ipv4 protocol (af_inet), via tcp (sock_stream)
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
        // 3) listen to tne binded socket at client port
        // takes socket field descriptor and a max len of the queue
        int listen_status = listen(socket_fd, 10);

        if (listen_status < 0) {
            perror("Unable to create a listener for socket");
            return -1;
        }
        return socket_fd;
    }


    static void source_listener(int conn_fd, int MAX, ConnectionManager* destination_clients) {
        std::vector<uint8_t> message;
        message.reserve(MAX);
        uint8_t buffer[MAX/2];

        while (true) {
            ssize_t bytes_received = recv(conn_fd, buffer, MAX, 0);
            if (bytes_received <= 0) {
                if (bytes_received == 0) {
                    printf("Source client disconnected gracefully\n");
                } else {
                    perror("Source client recv error");
                }
                break;
            }

            // append newly received bytes to buffer
            message.insert(message.end(), buffer, buffer + bytes_received);

            // Process all full messages in buffer
            while (message.size() >= 8) {
                // extract payload length from bytes 2 and 3 and convert to host order
                int payload_length = CTMPMessageValidator::get_payload_length(message.data());
                if (payload_length < 0) {
                    perror("Source client payload length is invalid");
                    break;
                }

                size_t full_msg_len = 8 + payload_length;

                if (message.size() < full_msg_len) {
                    // Not enough data yet for full message
                    break;
                }

                // Validate the full message
                if (!CTMPMessageValidator::validate(message.data(), full_msg_len)) {
                    // Invalid message, drop these bytes (or handle differently)
                    message.erase(message.begin(), message.begin() + full_msg_len);
                    continue;
                }

                // send the valid ctmp message to all destination clients
                for (int conn : destination_clients->get_all()) {
                    if (send(conn, message.data(), full_msg_len, 0) < 0) {
                        perror("Unable to send message to destination client");
                    }
                }

                // Remove the processed message from buffer
                message.erase(message.begin(), message.begin() + full_msg_len);
            }
        }
    }

    // handles the destination clients actions
    static void destination_listener(int conn_fd, int MAX) {
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
        struct sockaddr_in address{};
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
        
        while(true) {
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
                if (type == SOURCE) this->source_listener(conn_fd, MAX_BUFFER_SIZE, destination_clients);
                if (type == DESTINATION) this->destination_listener(conn_fd, MAX_BUFFER_SIZE);
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
    // source has max connections of 1, destination has unbound (-1)
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