#include <bits/stdc++.h>
using namespace std;

#include "../include/RedisDatabase.h"

RedisDatabase& RedisDatabase::getInstance() {
    static RedisDatabase instance;

    return instance;
}

/*
Memory -> File(Dump)
File -> Memory(Load)

K = Key Value
L = List
H = Hash
*/

bool RedisDatabase::dump(const string& filename) {
    lock_guard<mutex> lock(db_mutex);

    ofstream ofs(filename ,ios::binary);
    if(!ofs) return false;

    for(const auto &kv : kv_store) {
        ofs << "K " << kv.first << " " << kv.second << "\n";
    }

    for(const auto &ls : list_store) {
        ofs << "L " << ls.first;
        for(const auto &item : ls.second) 
            ofs << " " << item;
        ofs << "\n";
    }

    for(const auto &hs : hash_store) {
        ofs << "H " << hs.first;
        for(const auto &field_val : hs.second) 
            ofs << " " << field_val.first << ":" << field_val.second;
        ofs << "\n";
    }

    return true;
}

/*
Key-Value (K)
kv_store["name"] = "Alice";
kv_store["city"] = "Berlin";

List (L)
list_store["fruits"] = {"apple", "banana", "orange"};
list_store["colors"] = {"red", "green", "blue"};

Hash (H)
hash_store["user:100"] = {
    {"name", "Bob"},
    {"age", "30"},
    {"email", "bob@example.com"}
};

hash_store["user:200"] = {
    {"name", "Eve"},
    {"age", "25"},
    {"email", "eve@example.com"}
};
*/

bool RedisDatabase::load(const string& filename) {
    lock_guard<mutex> lock(db_mutex);

    ifstream ifs(filename ,ios::binary);
    if(!ifs) return false;

    kv_store.clear();
    list_store.clear();
    hash_store.clear();

    string line;
    while(getline(ifs ,line)) {
        istringstream iss(line);
        char type;

        iss >> type;
        
        if(type == 'K') {
            string key ,value;
            iss >> key >> value;

            kv_store[key] = value;
        }
        else if(type == 'L') {
            string key;
            iss >> key;

            string item;
            vector<string> list;
            while(iss >> item) 
                list.push_back(item);

            list_store[key] = list;
        }
        else if(type == 'H') {
            string key;
            iss >> key;

            unordered_map<string ,string> hash;
            string pair;

            while(iss >> pair) {
                auto pos = pair.find(':');
                if(pos != string::npos) {
                    string field = pair.substr(0 ,pos);
                    string value = pair.substr(pos+1);

                    hash[field] = value;
                }
            }

            hash_store[key] = hash;
        }
    }
    return true;
}

//Key - Value Operations
bool RedisDatabase::flushAll() {
    lock_guard<mutex> lock(db_mutex);

    kv_store.clear();
    list_store.clear();
    hash_store.clear();

    return true;
}

bool RedisDatabase::set(const string &key ,const string &value) {
    lock_guard<mutex> lock(db_mutex);

    if(kv_store.count(key)) {
        cerr << "The key: " << key << " is already present in database\n";
        return false;
    }

    kv_store[key] = value;
    return true;
}

bool RedisDatabase::get(const string &key ,string &value) {
    lock_guard<mutex> lock(db_mutex);

    auto itr = kv_store.find(key);
    if(itr == kv_store.end()) {
        cerr << "The key: " << key << " is not present in database\n";
        return false;
    }

    value = itr -> second;
    return true;
}

vector<string> RedisDatabase::keys() {
    lock_guard<mutex> lock(db_mutex);

    vector<string> result;
    for(const auto &pair : kv_store) {
        result.push_back(pair.first);
    }
    for(const auto &pair : list_store) {
        result.push_back(pair.first);
    }
    for(const auto &pair : hash_store) {
        result.push_back(pair.first);
    }
}