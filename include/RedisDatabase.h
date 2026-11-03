#include <bits/stdc++.h>
using namespace std;

#ifndef REDIS_DATABASE_H
#define REDIS_DATABASE_H

enum class RedisType {
    STRING ,
    LIST ,
    HASH ,
    SET
};

// Result of operations that can fail in more than one way
enum class OpStatus {
    OK ,
    NOT_FOUND ,
    WRONG_TYPE ,
    NOT_INTEGER ,
    NOT_FLOAT ,
    OVERFLOW
};

// SET ... NX / XX
enum class SetCondition {
    NONE ,
    NX ,    // only set if the key does not exist
    XX      // only set if the key already exists
};

class RedisDatabase {
    public:
        // Get the singleton instance
        static RedisDatabase& getInstance();

        //Common commands
        bool flushAll();
        size_t dbSize();

        //Key-Value Operations
        bool set(const string &key ,const string &value ,long long ttlMs = -1 ,SetCondition cond = SetCondition::NONE);
        bool get(const string &key ,string &value);
        void mset(const vector<pair<string ,string>> &keyValues);
        OpStatus getSet(const string &key ,const string &value ,string &oldValue);
        OpStatus getDel(const string &key ,string &value);
        OpStatus incrBy(const string &key ,long long delta ,long long &result);
        OpStatus incrByFloat(const string &key ,long double delta ,string &result);
        OpStatus append(const string &key ,const string &value ,size_t &newLength);
        OpStatus stringLength(const string &key ,size_t &length);
        OpStatus getRange(const string &key ,long long start ,long long end ,string &result);

        //Generic key commands
        vector<string> keys(const string &pattern = "*");
        string type(const string &key);
        bool exists(const string &key);
        bool deleteUnlocked(const string &key);
        bool del(const string &key);
        bool expire(const string &key ,int seconds);
        bool pexpire(const string &key ,long long milliseconds);
        long long pttl(const string &key);   // -2 missing, -1 no expiry, else ms left
        bool persist(const string &key);
        bool rename(const string &oldKey ,const string &newKey);
        bool randomKey(string &key);

        //helper functions
        bool checkExpiry(const string& key);
        string getString(RedisType type);
        static bool globMatch(const string &pattern ,const string &str);
        static bool parseInteger(const string &str ,long long &value);

        //Active expiry: delete up to maxKeys expired keys, returns the number removed
        int purgeExpired(int maxKeys);

        //List Operations
        string lpush(const string &key ,const vector<string> &values);
        string rpush(const string &key ,const vector<string> &values);
        OpStatus lpop(const string &key ,int count ,vector<string> &values);
        OpStatus rpop(const string &key ,int count ,vector<string> &values);
        string llen(const string &key);
        bool lrange(const string &key ,const int start ,const int end ,vector<string> &values);
        bool lindex(const string &key, int index, string &value);
        bool lset(const string &key, int index, const string &value);
        int lrem(const string &key, int count, const string &value);
        bool ltrim(const string &key, int start, int end);
        OpStatus linsert(const string &key, bool before, const string &pivot, const string &value, long long &newLength);

        //Hash Operations
        bool hset(const string &key, const vector<pair<string, string>> &fieldValues, int &added);
        OpStatus hsetnx(const string &key, const string &field, const string &value, bool &set);
        bool hget(const string &key, const string &field, string &value);
        int hdel(const string &key, const vector<string> &fields);
        bool hexists(const string &key, const string &field, bool &exists);
        int hlen(const string &key);
        bool hgetall(const string &key, vector<pair<string, string>> &fieldValues);
        bool hkeys(const string &key, vector<string> &fields);
        bool hvals(const string &key, vector<string> &values);
        OpStatus hincrBy(const string &key, const string &field, long long delta, long long &result);

        //Set Operations
        OpStatus sadd(const string &key, const vector<string> &members, int &added);
        OpStatus srem(const string &key, const vector<string> &members, int &removed);
        OpStatus sismember(const string &key, const string &member, bool &isMember);
        OpStatus smembers(const string &key, vector<string> &members);
        OpStatus scard(const string &key, size_t &count);
        OpStatus spop(const string &key, int count, vector<string> &members);
        OpStatus srandmember(const string &key, string &member);
        // op: 'I' = intersection, 'U' = union, 'D' = difference
        OpStatus setOperation(char op, const vector<string> &keys, vector<string> &result);

        //Persistance: Dump / load the database from a file
        bool dump(const string& filename);
        bool load(const string& filename);
        void setDumpFile(const string &filename);
        string getDumpFile();

        //Stats (used by INFO)
        void noteWrite();
        long long changesSinceSave() const;
        long long lastSaveTime() const;     // unix seconds, 0 if never saved
        long long expiredKeys();
        map<string ,size_t> typeCounts();
        size_t expiresCount();

    private:
        RedisDatabase() = default;
        ~RedisDatabase() = default;
        RedisDatabase(const RedisDatabase&) = delete;
        RedisDatabase& operator = (const RedisDatabase&) = delete;

        // Expiry-aware type lookup: nullptr if the key is missing (or just expired)
        RedisType* findTypeUnlocked(const string &key);
        void clearUnlocked();
        bool loadLegacy(const string &data);
        bool loadV2(const string &data);
        OpStatus popUnlocked(const string &key ,int count ,bool fromFront ,vector<string> &values);

        mutex db_mutex;
        mutex dump_mutex;
        unordered_map<string ,string> kv_store;
        unordered_map<string ,deque<string>> list_store;
        unordered_map<string ,unordered_map<string ,string>> hash_store;
        unordered_map<string ,unordered_set<string>> set_store;

        unordered_map<string ,chrono::steady_clock::time_point> expiry_map;
        unordered_map<string ,RedisType> type_store;

        string dump_file = "dump.my_rdb";
        long long expired_keys = 0;
        atomic<long long> dirty{0};
        atomic<long long> last_save{0};
        mt19937 rng{random_device{}()};
};

#endif
