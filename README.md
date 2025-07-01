# Operation WIRE STORM Solution

### Main Structure:
#### Class List:
- ConnectionManager — Manages active client connections whilst also being thread safe.

- CTMPMessageValidator — Parses and validates CTMP message headers and checksums.

- BaseServer — Abstract TCP server providing core accept/listen connection/disconnection logic. Children of this class must override the handle_client(), which gets called after a connection is accepted.

- DestinationServer (extends BaseServer) — Accepts multiple clients, waits to receive messages.

- SourceServer (extends BaseServer)  — Accepts a single client, validates and broadcasts CTMP messages to a list of destination clients.

#### Rationale for solution
This solution is my object oriented solution to the problem. DestinationServer and SourceServer are specific implementations of the BaseServer, their own specific logic falls within the responsibility of their respective classes (Single Responsiblity Principle). The ConnectionManager also follows this as its only purpose is to handle the connected clients list in a thread safe way. The CTMPMessageValidator is a static class with all static members, this was done to group all the validation logic of CTMP message under one class.
<br> <br>
The benefits of my implementaion is that it is easy to debug as each component only handles related logic and furthermore it is easy to implement new features. For example if the requirements change and the source server needs to accept n number of users it just requires a change of a number.
<br> <br>
On the call of the run method on a server, a new thread is created for each connected client, the original thread that is running the run method loops infinitely awaiting more connection, or to refuse connections if max clients is reached on a given server.


---
### UML Diagram
```
                 +----------------------+
                 |      BaseServer      |   (abstract)
                 +----------------------+
                 | - port: uint16_t     |
                 | - connections:       |
                 |   ConnectionManager  |
                 +----------------------+
                 | + run(): void        |
                 | + get_connections(): |
                 |   ConnectionManager* |
                 +----------------------+
                 | - create_socket(...) |
                 | - handleClient(int): |
                 |   virtual            |
                 +----------------------+
                          ▲
        +-----------------+------------------+
        |                                    |
+--------------------------+     +--------------------------+
|    DestinationServer     |     |       SourceServer       |
+--------------------------+     +--------------------------+
| + DestinationServer()    |     | + SourceServer(dest*)    |
+--------------------------+     +--------------------------+
| - handleClient(int):void |     | - handleClient(int):void |
|                          |     | - destination_clients:   |
|                          |     |   ConnectionManager*     |
|                          |     | - send_message_all(...)  |
+--------------------------+     +--------------------------+

+--------------------------------------+
|            ConnectionManager         |
+--------------------------------------+
| - max_connections: int               |
| - clients: std::set<int>             |
| - mutex: std::mutex                  |
+--------------------------------------+
| + add(fd: int): bool                 |
| + remove(fd: int): void              |
| + get_all(): std::set<int>           |
| + size(): size_t                     |
| + get_max_connections(): int         |
+--------------------------------------+

+----------------------------------------------------+
|                CTMPMessageValidator (static)       |
+----------------------------------------------------+
| + validate(data: vector<uint8_t>): bool            |
| + validate_checksum(message, checksum): bool       |
| + get_checksum(data): int                          |
| + get_payload_length(data): int                    |
| + get_options(data): int                           |
+----------------------------------------------------+
| - nth_bit_set(byte: uint8_t, n: int): bool         |
+----------------------------------------------------+

```
---

### Github Branch Structure
#### main
contains solution for both the base challenge and optional challenge (checksum), also contains updated readme.md

#### original-solution
contains the original solution for the base challenge


---

### How to run the server

```
git clone https://github.com/firozt/WireStormSolution.git
cd WireStormSolution
g++ -std=c++17 main.cpp -o main -pthread && ./main
```
after running the g++ command the program should indicate that it's listening on port 44444 and 33333
