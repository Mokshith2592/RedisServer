#include <bits/stdc++.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "../include/RedisServer.h"
#include "../include/RedisCommandHandler.h"
#include "../include/RedisDatabase.h"

using namespace std;

// A client whose unparsed input grows past this is disconnected (protects memory)
static const size_t MAX_QUERY_BUFFER = 1024ULL * 1024 * 1024;

static RedisServer* globalServer = nullptr;

// Only async-signal-safe work is allowed here (no locks, no allocation, no iostreams).
// The handler just asks the server to stop; run() does the final dump on the main thread.
void signalHandler(int) {
    const char msg[] = "Caught signal, shutting down..\n";
    ssize_t ignored = write(STDOUT_FILENO ,msg ,sizeof(msg) - 1);
    (void)ignored;
    if(globalServer) globalServer -> shutdown();
}

void RedisServer::setupSignalHandler() {
    signal(SIGINT ,signalHandler);
    signal(SIGTERM ,signalHandler);
    // Writing to a socket the client already closed must not kill the server
    signal(SIGPIPE ,SIG_IGN);
}

RedisServer::RedisServer(int port) : port(port) ,server_socket(-1) ,running(true) {
    globalServer = this;
    setupSignalHandler();
}

// Safe to call from a signal handler: stops the accept loop in run()
void RedisServer::shutdown() {
    running = false;

    // Wakes up the blocked accept() so run() can finish and persist the database
    if(server_socket != -1) ::shutdown(server_socket ,SHUT_RDWR);
}

// send() may write only part of the buffer, keep going until everything is out
static bool sendAll(int socket ,const string &data) {
    size_t sent = 0;
    while(sent < data.size()) {
        ssize_t n = send(socket ,data.data() + sent ,data.size() - sent ,MSG_NOSIGNAL);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) return false;
        sent += n;
    }
    return true;
}

void RedisServer::handleClient(int client_socket) {
    ServerStats &stats = serverStats();
    stats.connectedClients++;
    stats.totalConnections++;

    RedisCommandHandler cmdHandler;
    string pending;             // bytes received but not yet parsed into a command
    char buffer[16 * 1024];
    bool open = true;

    while(open) {
        ssize_t bytes = recv(client_socket ,buffer ,sizeof(buffer) ,0);
        if(bytes < 0 && errno == EINTR) continue;
        if(bytes <= 0) break;
        pending.append(buffer ,bytes);

        // Pipelining: run every complete command in the buffer and reply in one write
        string out;
        size_t pos = 0;
        while(pos < pending.size()) {
            vector<string> tokens;
            size_t consumed = 0;
            ParseStatus status = RedisCommandHandler::parseNext(pending ,pos ,tokens ,consumed);
            if(status == ParseStatus::INCOMPLETE) break;
            if(status == ParseStatus::PROTOCOL_ERROR) {
                out += "-Error: Protocol error\r\n";
                open = false;
                break;
            }
            pos += consumed;
            if(tokens.empty()) continue;   // blank line

            out += cmdHandler.execute(tokens);

            string cmd = tokens[0];
            transform(cmd.begin() ,cmd.end() ,cmd.begin() ,::toupper);
            if(cmd == "QUIT") { open = false; break; }
        }
        pending.erase(0 ,pos);

        if(pending.size() > MAX_QUERY_BUFFER) {
            out += "-Error: max query buffer size exceeded\r\n";
            open = false;
        }
        if(!out.empty() && !sendAll(client_socket ,out)) break;
    }

    close(client_socket);
    stats.connectedClients--;
}

void RedisServer::run() {
    server_socket = socket(AF_INET ,SOCK_STREAM ,0);
    if(server_socket < 0) {
        cerr << "Error Creating Server Socket\n";
        return;
    }

    int opt = 1;
    setsockopt(server_socket ,SOL_SOCKET ,SO_REUSEADDR ,&opt ,sizeof(opt));

    sockaddr_in serverAddr{};

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if(bind(server_socket ,(struct sockaddr*)&serverAddr ,sizeof(serverAddr)) < 0) {
        cerr << "Error Binding Server Socket (is port " << port << " already in use?)\n";
        return;
    }

    if(listen(server_socket ,SOMAXCONN) < 0) {
        cerr << "Error listening On Server Socket\n";
        return;
    }

    serverStats().port = port;
    cout << "Redis Server Listining On Port " << port << "\n";

    while(running) {
        int client_socket = accept(server_socket ,nullptr ,nullptr);
        if(client_socket < 0 ) {
            if(errno == EINTR) continue;
            if(running) cerr << "Error Accepting Client Connection\n";
            break;
        }

        // Small replies should go out immediately instead of waiting for Nagle's algorithm
        setsockopt(client_socket ,IPPROTO_TCP ,TCP_NODELAY ,&opt ,sizeof(opt));

        // Detached so finished client threads don't pile up for the lifetime of the server
        thread([this ,client_socket](){ handleClient(client_socket); }).detach();
    }

    close(server_socket);

    //Before shutdown ,persisit the database
    RedisDatabase &db = RedisDatabase::getInstance();
    if(db.dump(db.getDumpFile()))
        cout << "Database Dumped to " << db.getDumpFile() << "\n";
    else
        cerr << "Error dumping database\n";
    cout << "Server Shutdown Complete\n";
}
