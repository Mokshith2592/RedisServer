#include <bits/stdc++.h>
using namespace std;

#ifndef REDIS_SERVER_H
#define REDIS_SERVER_H

class RedisServer {
    public:
        RedisServer(int port);

        void run();

        void shutdown();
        
    private:
        int port;
        int server_socket;
        atomic<bool> running;
};

#endif 