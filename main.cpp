#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <set>
#include <mutex>
#include <strings.h>


// global constants
#define SOURCE_PORT 33333
#define DESTINATION_PORT 44444
#define MAX_BUFFER_SIZE 2048 // bytes
#define HEADER_SIZE 8 // bytes
#define CHECKSUM_OFFSET 4
#define OPTIONS_OFFSET 1
#define LENGTH_OFFSET 2

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
 * @class CTMPMessageValidator
 * @brief Validates and parses data against the CTMP protocol standards
 * describe in the challenge briefing
 */
class CTMPMessageValidator {
    public:
    // Checks for all header properties, and validates them, including: magic byte,
    // padding bytes, length bytes, options and checksum if secure.
    // returns true if data passes all checks, else false
    static bool validate(const std::vector<uint8_t>& data) {
        constexpr uint8_t magic_byte = 0xCC;
        constexpr uint8_t padding_byte = 0x00;


        // not enough bytes for a CTMP message
        if (data.size() < HEADER_SIZE) return false;

        // checking magic byte
        if (data[0] != magic_byte) return false;

        int options = get_options(data);
        if (nth_bit_set(options,1) && !validate_checksum(data, get_checksum(data))) {
            // invalid checksum on secure message
            printf("Invalid checksum on secure CTMP message, dropping message.\n");
            return false;
        }
        size_t payload_length = get_payload_length(data);

        // checks the last 2 bytes of header is padding
        for (size_t i = 6; i < HEADER_SIZE; ++i) {
            if (data[i] != padding_byte) return false;
        }


        // if the data in payload exceeds payload size drop message
        size_t actual_payload_size = data.size() - HEADER_SIZE;
        if (actual_payload_size > (payload_length)) return false;

        return true;
    }

    // validates checksum of a CTMP message (including header)
    // returns true if valid, false if invalid
    static bool validate_checksum(const std::vector<uint8_t>& message, const uint16_t checksum) {
        if (message.size() < HEADER_SIZE) {
            return false;  // header too short
        }

        // make a copy of the message and replace checksum bytes with 0xCC

        uint32_t sum = 0;

        // sum all 16-bit words in message (including header)
        size_t i = 0;
        for (; i + 1 < message.size(); i += 2) {
            uint16_t word;
            if (i == CHECKSUM_OFFSET) {
                // skip checksum field
                word = 0xCCCC;
            } else {
                // concats byte1 with tem[i+1] via bit manipulation
                // converts byte1 to 16 bits, padding left bits with 0's
                // bit shift byte1 to the left by 8 so all the 0's are now on the right
                // logical OR byte1 and byte2
                uint8_t byte1 = message[i];
                uint8_t byte2 = message[i + 1];
                word = (static_cast<uint16_t>(message[i]) << 8) | message[i + 1];
            }
            sum += word;
        }

        // edge case that the message is odd number of bytes
        // add padding
        if (i < message.size()) {
            // pads the 8 rightmost bits with 0 by converting and bit shifts
            uint16_t last = static_cast<uint16_t>(message[i]) << 8;
            sum += last;
        }

        // fold carry bits from top 16 bits into lower 16 bits
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }


        // calculate ones complement of sum <=> negation of bits of sum
        uint16_t calculated_checksum = ~sum;

        return calculated_checksum == checksum;
    }

    // returns -1 if data is not large enough to contain the header
    // otherwise an uint16_t representing the checksum value in host order
    static int get_checksum(const std::vector<uint8_t>& data) {
        if (data.size() < HEADER_SIZE) return 0;

        uint16_t checksum;
        std::memcpy(&checksum, &data[CHECKSUM_OFFSET], sizeof(uint16_t));
        return ntohs(checksum);
    }


    // returns -1 if data is not large enough to contain the header
    // otherwise an uint8_t that represents the payload length field in host order
    static int get_payload_length(const std::vector<uint8_t>& data) {
        if (data.size() < HEADER_SIZE) return -1;

        uint16_t payload_length_network_order;
        std::memcpy(&payload_length_network_order, &data[LENGTH_OFFSET], sizeof(uint16_t));
        return ntohs(payload_length_network_order);
    }

    // returns -1 if data is not large enough to contain the header
    // otherwise an uint8_t that represents the options field
    static int get_options(const std::vector<uint8_t>& data) {
        if (data.size() < HEADER_SIZE) return -1;
        uint8_t options;
        std::memcpy(&options, &data[OPTIONS_OFFSET], sizeof(uint8_t));
        return options;
    }

    private:
    // checks if nth bit from the left is set in a byte
    static bool nth_bit_set(uint8_t byte, int n) {
        if (n < 0 || n > 7) return false; // out of bounds
        return (byte & (1 << (7 - n))) != 0;
    }
};

/**
 * Abstract
 * @class BaseServer
 * @brief Basic functionalities of a TCP IPv4 server, listens to a port, manages connections
 * actions of connected clients is handled by the virtual method handleClient, that must be
 * overwritten in child classes
 */
class BaseServer {
    private:
    const uint16_t port;
    ConnectionManager connections;


    // creates a IPv4 TCP socket, binds it with the address config and listens to it
    // returns the socket_fd
    int create_socket(sockaddr_in& address, int addr_len) const {
        //  create socket
        // ipv4 protocol (af_inet), via tcp (sock_stream)
        int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        // socket_fd should be > 0 to be valid else error
        if (socket_fd < 0) {
            perror("Socket creation failed\n");
            return -1;
        }

        //  bind socket with address and port
        int bind_status = bind(socket_fd, reinterpret_cast<struct sockaddr *>(&address), static_cast<socklen_t>(addr_len));

        if (bind_status < 0) {
            printf("Unable to bind socket for port %d\n",port);
            return -1;
        }
        //  listen to the binded socket at client port
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


    // returns a pointer to the current objects connection mannager object
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
                // reject this connection, send attempted client an error
                const char* err_msg = "Error: Source client already connected.\n";
                send(conn_fd, err_msg, strlen(err_msg), 0);
                close(conn_fd);
                continue;
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
 * @brief Implementation of the destination server, takes in any number of connections,
 * waits for CTMP to be sent from another server
 */
class DestinationServer: public BaseServer {
    public:
    DestinationServer()
    : BaseServer(DESTINATION_PORT, -1) {} // -1 means unlimited connections

    private:
    // handles the destination clients actions
    void handleClient(const int conn_fd) override {
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
 * @brief Implementation of the source server, takes in at most one client at a time
 * (enforced via constructor), broadcasts validated CTMP messages to a list of connected
 * destination clients (passed in through the constructor)
 */
class SourceServer: public BaseServer {
    public:
    SourceServer(ConnectionManager* dest_clients)
    : BaseServer(SOURCE_PORT, 1), destination_clients(dest_clients) {}

    private:
    ConnectionManager* destination_clients;
    void handleClient(const int conn_fd) override {
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

            // attempt to process the current messages in buffer
            while (message.size() >= HEADER_SIZE) {
                int payload_length = CTMPMessageValidator::get_payload_length(message);

                // header size + length field in header
                size_t full_msg_len = HEADER_SIZE + payload_length;

                if (message.size() < full_msg_len) {
                    // not enough data yet for full message
                    break;
                }

                // Validate the full message
                if (!CTMPMessageValidator::validate(message)) {
                    // Invalid message, drop these bytes
                    message.erase(message.begin(), message.begin() + full_msg_len);
                    break;
                }

                // passed all validation, send to all destination clients
                send_message_all(message);

                // remove the processed message from buffer
                message.erase(message.begin(), message.begin() + full_msg_len);
            }
        }
    }
    private:

    // sends a message passed in to all destination clients
    void send_message_all(const std::vector<uint8_t>& message) {
        // send the valid ctmp message to all destination clients
        for (int conn : destination_clients->get_all()) {
            if (send(conn, message.data(), message.size(), 0) < 0) {
                perror("Unable to send message to destination client");
            }
        }
    }

};

int main() {
    // create destination server (allows unlimited connections)
    DestinationServer destination_server;

    // create source server (only 1 connection allowed, forwards to destinations)
    // pass in a pointer to destination server connections to source server, used for sending messages
    // to all clients of destination server
    SourceServer source_server(destination_server.get_connections());

    // start both servers on their own threads
    std::thread dst_thread([&]() { destination_server.run(); });
    std::thread src_thread([&]() { source_server.run(); });

    dst_thread.join();
    src_thread.join();

    return 0;
}
