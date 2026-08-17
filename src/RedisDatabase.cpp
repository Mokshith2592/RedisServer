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
    expiry_map.clear();
    type_store.clear();


    string line;
    while(getline(ifs ,line)) {
        istringstream iss(line);
        char type;

        iss >> type;
        
        if(type == 'K') {
            string key ,value;
            iss >> key >> value;

            kv_store[key] = value;
            type_store[key] = RedisType::STRING;
        }
        else if(type == 'L') {
            string key;
            iss >> key;

            string item;
            deque<string> list;
            while(iss >> item) 
                list.push_back(item);

            list_store[key] = list;
            type_store[key] = RedisType::LIST;
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
            type_store[key] = RedisType::HASH;
        }
    }
    return true;
}

//Key - Value Operations

//Helper fns
string RedisDatabase::getString(RedisType type) {
    if(type == RedisType::STRING) return "string";
    else if(type == RedisType::LIST) return "list";
    else if(type == RedisType::HASH) return "hash";
    else return "none";
}

bool RedisDatabase::checkExpiry(const string &key) {
    if(expiry_map.find(key) != expiry_map.end() &&
        expiry_map[key] <= chrono::steady_clock::now()) {
        if(deleteUnlocked(key)) return true;
        else return false;
    }
    return false;
}

bool RedisDatabase::flushAll() {
    lock_guard<mutex> lock(db_mutex);

    kv_store.clear();
    list_store.clear();
    hash_store.clear();
    expiry_map.clear();
    type_store.clear();

    return true;
}

bool RedisDatabase::set(const string &key ,const string &value) {
    lock_guard<mutex> lock(db_mutex);

    if((type_store.find(key) != type_store.end() && type_store[key] == RedisType::STRING) || 
        (type_store.find(key) == type_store.end())) {
        kv_store[key] = value;
        type_store[key] = RedisType::STRING;

        return true;
    }  
    else {
        cerr << "The key is already set as " << getString(type_store[key]) << "\n";
        return false;
    }
}

bool RedisDatabase::get(const string &key ,string &value) {
    lock_guard<mutex> lock(db_mutex);

    if(checkExpiry(key)) {
        cerr << key << " is expired\n";
        return false;
    }

    if(type_store.find(key) != type_store.end()) {
        string type = getString(type_store[key]);
        if(type == "list") {
            cerr << "The key: " << key << " is present as a List\n";
            return false;
        }
        else if(type == "hash") {
            cerr << "The key: " << key << " is present as a hash\n";
            return false;
        }
    }

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
    // for(const auto &pair : kv_store) {
    //     result.push_back(pair.first);
    // }
    // for(const auto &pair : list_store) {
    //     result.push_back(pair.first);
    // }
    // for(const auto &pair : hash_store) {
    //     result.push_back(pair.first);
    // }

    for(auto &pair : type_store) {
        result.push_back(pair.first);
    }

    return result;
}

string RedisDatabase::type(const string &key) {
    lock_guard<mutex> lock(db_mutex);

    if(checkExpiry(key)) {
        cerr << key << " is expired\n";
        return "none";
    }

    string ans = "";
    if(type_store.find(key) != type_store.end()) ans = getString(type_store[key]);
    else ans = "none";

    return ans;
}

bool RedisDatabase::deleteUnlocked(const string &key) {
    bool erased = false;
    erased |= (kv_store.erase(key) > 0);
    erased |= (list_store.erase(key) > 0);
    erased |= (hash_store.erase(key) > 0);
    erased |= (expiry_map.erase(key) > 0);
    erased |= (type_store.erase(key) > 0);

    return erased;
}

bool RedisDatabase::del(const string &key) {
    lock_guard<mutex> lock(db_mutex);
    return deleteUnlocked(key);    
}

bool RedisDatabase::expire(const string &key ,int seconds) {
    lock_guard<mutex> lock(db_mutex);

    bool exist = (type_store.find(key) != type_store.end());

    if(!exist) return false;

    expiry_map[key] = chrono::steady_clock::now() + chrono::seconds(seconds);
    return true;
}

bool RedisDatabase::rename(const string &oldKey ,const string &newKey) {
    lock_guard<mutex> lock(db_mutex);

    if(checkExpiry(oldKey)) {
        cerr << oldKey << " is expired\n";
        return false;
    }

    const bool exists = kv_store.count(oldKey) || list_store.count(oldKey) ||
                        hash_store.count(oldKey);
    if(!exists) return false;

    if(oldKey == newKey) return true;

    kv_store.erase(newKey);
    list_store.erase(newKey);
    hash_store.erase(newKey);
    expiry_map.erase(newKey);
    type_store.erase(newKey);

    auto itrKv = kv_store.find(oldKey);
    if(itrKv != kv_store.end()) {
        kv_store.emplace(newKey, move(itrKv->second));
        kv_store.erase(itrKv);

        type_store[newKey] = RedisType::STRING;
    }

    auto itrList = list_store.find(oldKey);
    if(itrList != list_store.end()) {
        list_store.emplace(newKey, move(itrList->second));
        list_store.erase(itrList);

        type_store[newKey] = RedisType::LIST;
    }

    auto itrHash = hash_store.find(oldKey);
    if(itrHash != hash_store.end()) {
        hash_store.emplace(newKey, move(itrHash->second));
        hash_store.erase(itrHash);

        type_store[newKey] = RedisType::HASH;
    }

    auto itrExpiry = expiry_map.find(oldKey);
    if(itrExpiry != expiry_map.end()) {
        expiry_map.emplace(newKey, itrExpiry->second);
        expiry_map.erase(itrExpiry);
    }

    return true;
}

//List Operations
string RedisDatabase::lpush(const string &key ,const vector<string> &values) {
    lock_guard<mutex> lock(db_mutex);

    if(checkExpiry(key)) {
        cerr << key << " is expired\n";
        return "0";
    }

    if(((type_store.find(key) != type_store.end()) && (type_store[key] == RedisType::LIST)) || 
            (type_store.find(key) == type_store.end())) {
        deque<string> &currValues = list_store[key];
        
        for(const string &i : values) currValues.push_front(i);
        type_store[key] = RedisType::LIST;

        return to_string(currValues.size());
    }

    if(type_store[key] != RedisType::LIST) 
        cerr << "The key is already set as " << getString(type_store[key]) << "\n";

    return "0";
}

string RedisDatabase::rpush(const string &key ,const vector<string> &values) {
    lock_guard<mutex> lock(db_mutex);

    if(checkExpiry(key)) {
        cerr << key << " is expired\n";
        return "0";
    }

    if(((type_store.find(key) != type_store.end()) && (type_store[key] == RedisType::LIST)) || 
            (type_store.find(key) == type_store.end())) {
        deque<string> &currValues = list_store[key];
        
        for(const string &i : values) currValues.push_back(i);
        type_store[key] = RedisType::LIST;

        return to_string(currValues.size());
    }

    if(type_store[key] != RedisType::LIST) 
        cerr << "The key is already set as " << getString(type_store[key]) << "\n";
        
    return "0";
}

string RedisDatabase::lpop(const string &key) {
    lock_guard<mutex> lock(db_mutex);

    if(checkExpiry(key)) {
        cerr << key << " is expired\n";
        return "0";
    }

    if((type_store.find(key) == type_store.end())) {
        cerr << "The key " << key << " does not exisit\r\n";
        return "0";
    }
    
    if(type_store[key] != RedisType::LIST) {
        cerr << "The key is already set as " << getString(type_store[key]) << "\n";
        return "0";
    }

    deque<string> &currValue = list_store[key];
    if(currValue.size() > 0) {
        currValue.pop_front();
        if(currValue.size()  == 0) deleteUnlocked(key);
    }
    else {
        cerr << "The key " << key << " doesnt have enough elements\r\n";
        return "-1";
    }

    return to_string(currValue.size());
}

string RedisDatabase::rpop(const string &key) {
    lock_guard<mutex> lock(db_mutex);

    if(checkExpiry(key)) {
        cerr << key << " is expired\n";
        return "0";
    }

    if((type_store.find(key) == type_store.end())) {
        cerr << "The key " << key << " does not exisit\r\n";
        return "0";
    }

    if(type_store[key] != RedisType::LIST) {
        cerr << "The key is already set as " << getString(type_store[key]) << "\n";
        return "0";
    }

    deque<string> &currValue = list_store[key];
    if(currValue.size() > 0) {
        currValue.pop_back();
        if(currValue.size()  == 0) deleteUnlocked(key);
    }
    else {
        cerr << "The key " << key << " doesnt have enough elements\r\n";
        return "-1";
    }

    return to_string(currValue.size());
}

string RedisDatabase::llen(const string &key) {
    lock_guard<mutex> lock(db_mutex);

    if(checkExpiry(key)) {
        cerr << key << " is expired\n";
        return "0";
    }

    if((type_store.find(key) != type_store.end()) && (type_store[key] != RedisType::LIST)) {
        cerr << "The key is already set as " << getString(type_store[key]) << "\n";
        return "0";
    }

    if(type_store.find(key) != type_store.end()) return to_string(list_store[key].size());
    return "0";
}

bool RedisDatabase::lrange(const string &key ,const int start ,const int end ,vector<string> &values) {
    lock_guard<mutex> lock(db_mutex);

    if(checkExpiry(key)) {
        cerr << key << " is expired\n";
        return false;
    }

    if((type_store.find(key) != type_store.end()) && (type_store[key] != RedisType::LIST)) {
        cerr << "The key is already set as " << getString(type_store[key]) << "\n";
        return false;
    }

    if(type_store.find(key) != type_store.end()) {
        deque<string> &currValues = list_store[key];
        
        int size = currValues.size();
        if(start < 0 || end > size || start > end) {
            cerr << "The range doesnot exists\r\n";
            return false;
        }

        for(int i=start ;i<=end ;i++) {
            values.push_back(currValues[i]);
        }

        return true;
    }
    else {
        cerr << "The key " << key << " does not exists\r\n";
        return false;
    }

    return false;
}