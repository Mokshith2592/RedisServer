#include <bits/stdc++.h>
using namespace std;

#ifndef REDIS_DATABASE_H
#define REDIS_DATABASE_H

class RedisDatabase {
    public:
        // Get the singleton instance
        static RedisDatabase& getInstance();

        //Common commands
        bool flushAll();

        //Key-Value Operations
        bool set(const string &key ,const string &value);
        bool get(const string &key ,string &value);
        vector<string> keys();
        string type(const string &key);
        bool del(const string &key);
        bool expire(const string &key ,const string &seconds);
        bool rename(const string &oldKey ,const string &newKey);
        
        //Persistance: Dump / load the database from a file
        bool dump(const string& filename);
        bool load(const string& filename);

    private:
        RedisDatabase() = default;
        ~RedisDatabase() = default;
        RedisDatabase(const RedisDatabase&) = delete;
        RedisDatabase& operator = (const RedisDatabase&) = delete;

        mutex db_mutex;
        unordered_map<string ,string> kv_store;
        unordered_map<string ,vector<string>> list_store;
        unordered_map<string ,unordered_map<string ,string>> hash_store;

        unordered_map<string ,chrono::steady_clock::time_point> expiry_map;
};

#endif