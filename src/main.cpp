#include <bits/stdc++.h>

#include "../include/RedisServer.h"
#include "../include/RedisDatabase.h"
#include "../include/RedisCommandHandler.h"

using namespace std;

static void printUsage(const char *program) {
    cout << "Usage: " << program << " [port] [--port N] [--dbfilename FILE] [--save-interval SECONDS]\n"
         << "  --port            port to listen on (default 6379)\n"
         << "  --dbfilename      dump file used for persistence (default dump.my_rdb)\n"
         << "  --save-interval   seconds between automatic dumps, 0 disables (default 300)\n";
}

int main(int argc ,char* argv[]) {
    int port = 6379;
    string dumpFile = "dump.my_rdb";
    int saveInterval = 300;

    try {
        for(int i=1 ;i<argc ;i++) {
            string arg = argv[i];
            if(arg == "--help" || arg == "-h") { printUsage(argv[0]); return 0; }
            else if(arg == "--port" && i + 1 < argc) port = stoi(argv[++i]);
            else if(arg == "--dbfilename" && i + 1 < argc) dumpFile = argv[++i];
            else if(arg == "--save-interval" && i + 1 < argc) saveInterval = stoi(argv[++i]);
            else if(i == 1 && isdigit(static_cast<unsigned char>(arg[0]))) port = stoi(arg);   // old style: ./my_redis_server 6380
            else { printUsage(argv[0]); return 1; }
        }
    }
    catch(const exception &) {
        printUsage(argv[0]);
        return 1;
    }

    serverStats();  // start the uptime clock
    RedisDatabase &db = RedisDatabase::getInstance();
    db.setDumpFile(dumpFile);

    if(db.load(dumpFile))
        cout << "Database loaded from " << dumpFile << " (" << db.dbSize() << " keys)\n";
    else
        cout << "No dump found or load failed; Starting with an empty database\n";

    RedisServer server(port);

    //Background persistance : dump the database every `saveInterval` seconds (only if something changed).
    if(saveInterval > 0) {
        thread persistanceThread([saveInterval](){
            while(true) {
                this_thread::sleep_for(chrono::seconds(saveInterval));

                RedisDatabase &db = RedisDatabase::getInstance();
                if(db.changesSinceSave() == 0) continue;

                //dump the database
                if(!db.dump(db.getDumpFile()))
                    cerr << "Error Dumping Database\n";
                else
                    cout << "Database Dumped to " << db.getDumpFile() << "\n";
            }
        });
        persistanceThread.detach();
    }

    //Active expiry: besides lazily expiring keys on access, sweep expired keys 10 times a second
    //so keys that are never read again don't sit in memory forever.
    thread expiryThread([](){
        while(true) {
            this_thread::sleep_for(chrono::milliseconds(100));
            RedisDatabase::getInstance().purgeExpired(200);
        }
    });
    expiryThread.detach();

    server.run();

    // run() already saved the database. Leave without running static destructors:
    // detached client/expiry threads may still be touching the database singleton.
    cout.flush();
    cerr.flush();
    _Exit(0);
}
