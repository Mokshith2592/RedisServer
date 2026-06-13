#include <bits/stdc++.h>

#include "../include/RedisServer.h"

using namespace std;

int main(int argc ,char* argv[]) {
    int port = 6379;
    if(argc >= 2) port = stoi(argv[1]);

    RedisServer server(port);
    
    //Background persistance : dump the database every 300 seconds.
    thread persistanceThread([](){
        while(true) {
            this_thread::sleep_for(chrono::seconds(300));
            
            //dump the database
        }
    });
    persistanceThread.detach();

    server.run();
    return 0;
}