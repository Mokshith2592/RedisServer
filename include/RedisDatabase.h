#include <bits/stdc++.h>
using namespace std;

#ifndef REDIS_DATABASE_H
#define REDIS_DATABASE_H

class RedisDatabase {
    public:
        // Get the singleton instance
        static RedisDatabase& getInstance();

        //Persistance: Dump / load the database from a file
        bool dump(const string& filename);
        bool load(const string& filename);

    private:
        RedisDatabase() = default;
        ~RedisDatabase() = default;
        RedisDatabase(const RedisDatabase&) = delete;
        RedisDatabase& operator = (const RedisDatabase&) = delete;
};

#endif