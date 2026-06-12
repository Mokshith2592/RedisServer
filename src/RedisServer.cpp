#include <bits/stdc++.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>

#include "../include/RedisServer.h"
using namespace std;

static RedisServer* globalServer = nullptr;

RedisServer::RedisServer(int port) : port(port) ,server_socket(-1) ,running(true) {
    globalServer = this;
}

void RedisServer::shutdown() {
    running = false;

    if(server_socket != -1) close(server_socket);
    cout << "Server Shutdown Complete\n";
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
        cerr << "Error Binding Server Socket\n";
        return;
    }

    if(listen(server_socket ,10) < 0) {
        cerr << "Error listening On Server Socket\n";
        return;
    }

    cout << "Redis Server Listining On Port " << port << "\n";
}