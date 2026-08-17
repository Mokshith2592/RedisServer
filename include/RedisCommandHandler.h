#include <bits/stdc++.h>
using namespace std;

#ifndef REDIS_COMMAND_HANDLER_H
#define REDIS_COMMAND_HANDLER_H

class RedisCommandHandler {
    public:
        RedisCommandHandler();
        //Process a command from a client and return RESP-formatted response.

        string processCommand(const string &commandLine);
};

#endif