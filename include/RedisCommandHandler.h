#include <bits/stdc++.h>
using namespace std;

#ifndef REDIS_COMMAND_HANDLER_H
#define REDIS_COMMAND_HANDLER_H

enum class ParseStatus {
    COMPLETE ,      // one full command was parsed
    INCOMPLETE ,    // need more bytes from the socket
    PROTOCOL_ERROR  // malformed input, the connection should be closed
};

// Server wide counters reported by INFO
struct ServerStats {
    atomic<long long> connectedClients{0};
    atomic<long long> totalConnections{0};
    atomic<long long> commandsProcessed{0};
    atomic<int> port{0};
    chrono::steady_clock::time_point startTime = chrono::steady_clock::now();

    mutex commandMutex;
    map<string ,long long> commandCalls;
};
ServerStats& serverStats();

class RedisCommandHandler {
    public:
        RedisCommandHandler();
        //Process a command from a client and return RESP-formatted response.

        string processCommand(const string &commandLine);

        // Run an already parsed command
        string execute(const vector<string> &tokens);

        // Parse one command (RESP array or inline text) from buffer[start..].
        // On COMPLETE, `consumed` is the number of bytes used.
        static ParseStatus parseNext(const string &buffer ,size_t start ,vector<string> &tokens ,size_t &consumed);
};

#endif
