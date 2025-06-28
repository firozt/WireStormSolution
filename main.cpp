#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <set>
#include <mutex>

// global constants
#define SOURCE_PORT 33333
#define DESTINATION_PORT 44444
#define MAX_BUFFER_SIZE 2048 // bytes
#define HEADER_SIZE 8 // bytes


/**
 * @class ConnectionManager
 * @brief Thread safe way of managing list of connected clients
 * controls max number of clients
 */
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

/**
 * @class CTMPMessageValidator (abstract)
 * @brief Validates and parses custom binary message packets.
 */
class CTMPMessageValidator {
    public:
    /**
    * @brief Validates a CTMP message against the CTMP protocol standards
    *
    * Checks for all header properties, and validates them, including: magic byte,
    * padding bytes, length bytes. This method also checks that the payload data's
    * length corresponds to the headers payload length property
    *
    * @param data  the data contained within a vector of uint8_t
    * @return boolean, true if data is a valid CTMP message else false
    */
    static bool validate(const std::vector<uint8_t>& data) {
        constexpr uint8_t magic_byte = 0xCC;
        constexpr uint8_t padding_byte = 0x00;


        // not enough bytes for a CTMP message
        if (data.size() < HEADER_SIZE) return false;

        // checking magic and padding byte
        if (data[0] != magic_byte) return false;
        if (data[1] != padding_byte) return false;

        // checking payload length
        int payload_length = get_payload_length(data);
        if (payload_length < 0) return false;


        // checks next 4 padding bytes
        for (size_t i = 4; i < HEADER_SIZE; ++i) {
            if (data[i] != padding_byte) return false;
        }


        // if the data in payload exceeds payload size drop message
        size_t actual_payload_size = data.size() - HEADER_SIZE;
        if (actual_payload_size > static_cast<size_t>(payload_length)) return false;

        return true;
    }

    static int get_payload_length(const std::vector<uint8_t>& data) {
        if (data.size() < HEADER_SIZE) return -1;
        // extract payload length from ctmp message
        // copy the next 2 bytes, convert it from network order.
        uint16_t payload_length_network_order;
        std::memcpy(&payload_length_network_order, &data[2], sizeof(uint16_t));
        return ntohs(payload_length_network_order);
    }
};


/**
 * @class BaseServer
 * @brief Basic functionalities of a TCP IPv4 server, listens to a port, manages connections
 * actions of connected clients is handled by the virtual method handleClient, that must be
 * overwritten in child classes
 */
class BaseServer {
    private:
    const uint16_t port;
    ConnectionManager connections;


    // creates a IPv4 TCP socket listening to a given port passed in through address
    int create_socket(sockaddr_in address, int addr_len) const {
        //  create socket
        // ipv4 protocol (af_inet), via tcp (sock_stream)
        int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        // socket_fd should be > 0 to be valid else error
        if (socket_fd < 0) {
            perror("Socket creation failed\n");
            return -1;
        }

        //  bind socket with address and port
        int bind_status = bind(socket_fd, (struct sockaddr*) &address, (socklen_t)addr_len);

        if (bind_status < 0) {
            printf("Unable to bind socket for port %d\n",port);
            return -1;
        }
        //  listen to tne binded socket at client port
        // takes socket field descriptor and a max len of the queue
        int listen_status = listen(socket_fd, 10);

        if (listen_status < 0) {
            perror("Unable to create a listener for socket");
            return -1;
        }
        return socket_fd;
    }

    // all child classes must override this
    virtual void handleClient(int conn_fd) = 0;

    public:

    BaseServer(uint16_t port, int max_connections)
    : port(port), connections(max_connections) {};

    ConnectionManager* get_connections() {
        return &connections;
    }

    void run(){
        // creates a listening socket

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
            std::thread t([this,conn_fd]() {
                // use corresponding handler
                    this->handleClient(conn_fd);
                // cleanup socket from connection list
                connections.remove(conn_fd);
                close(conn_fd);
            });
            t.detach();
        }
        close(socket_fd);
    }
};

/**
 * @class DestinationServer
 * @brief Takes in any number of connections, waits for TCMP to
 * be sent from another server
 */
class DestinationServer: public BaseServer {
    public:
    DestinationServer()
    : BaseServer(DESTINATION_PORT, -1) {} // -1 means unlimited connections

    private:
    // handles the destination clients actions
    void handleClient(int conn_fd) {
        char buff[MAX_BUFFER_SIZE];
        while (true) {
            bzero(buff, MAX_BUFFER_SIZE);
            ssize_t bytes_read = recv(conn_fd, buff, MAX_BUFFER_SIZE, 0); // check if client is still connected
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
};

/**
 * @class SourceServer
 * @brief Takes in at most one client at a time (enforced via constructor),
 * broadcasts validated CTMP messages to a list of connected
 * destination clients (passed in through the constructor)
 */
class SourceServer: public BaseServer {
    public:
    SourceServer(ConnectionManager* dest_clients)
    : BaseServer(SOURCE_PORT, 1), destination_clients(dest_clients) {}

    private:
    ConnectionManager* destination_clients;
    void handleClient(int conn_fd) override {
        std::vector<uint8_t> message;
        message.reserve(MAX_BUFFER_SIZE);
        uint8_t buffer[MAX_BUFFER_SIZE/2];

        while (true) {
            ssize_t bytes_received = recv(conn_fd, buffer, MAX_BUFFER_SIZE/2, 0);
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
            // attempt to process the current message in buffer

            // returns -1 if size of message cannot contain the header
            while (message.size() >= HEADER_SIZE) {

                uint8_t payload_length = CTMPMessageValidator::get_payload_length(message);
                size_t full_msg_len = HEADER_SIZE + payload_length;

                if (message.size() < full_msg_len) {
                    // Not enough data yet for full message
                    break;
                }

                // Validate the full message
                if (!CTMPMessageValidator::validate(message)) {
                    // Invalid message, drop these bytes
                    message.erase(message.begin(), message.begin() + full_msg_len);
                    break;
                }

                // send the valid ctmp message to all destination clients
                for (int conn : destination_clients->get_all()) {
                    if (send(conn, message.data(), full_msg_len, 0) < 0) {
                        perror("Unable to send message to destination client");
                    }
                }

                // remove the processed message from buffer
                message.erase(message.begin(), message.begin() + full_msg_len);
            }
        }
    }
};


int main() {
    // create destination server (allows unlimited connections)
    DestinationServer destination_server;

    // create source server (only 1 connection allowed, forwards to destinations)
    // pass in destination server connections to source server, used for sending messages
    // to all clients of destination server
    SourceServer source_server(destination_server.get_connections());

    // start both servers on their own threads
    std::thread dst_thread([&]() { destination_server.run(); });
    std::thread src_thread([&]() { source_server.run(); });

    dst_thread.join();
    src_thread.join();

    return 0;
}
