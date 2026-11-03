#include <bits/stdc++.h>
using namespace std;

#include "../include/RedisDatabase.h"

RedisDatabase& RedisDatabase::getInstance() {
    static RedisDatabase instance;

    return instance;
}

static long long wallClockMs() {
    return chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count();
}

/*
Memory -> File(Dump)
File -> Memory(Load)

Format v2 (binary safe, keeps TTLs):
    MYRDB 2\n
    <type> <expire-at-unix-ms | -1> <len>:<key> <count> <len>:<item> <len>:<item> ...\n

K = Key Value   (count = 1, the value)
L = List        (count = number of items)
H = Hash        (count = 2 * fields, field/value alternating)
S = Set         (count = number of members)

Every string is length prefixed, so values may contain spaces, ':' or newlines.
The old space separated format (v1) is still accepted by load().
*/

static const string DUMP_HEADER = "MYRDB 2\n";

bool RedisDatabase::dump(const string& filename) {
    string data;
    long long dirtyAtSnapshot = 0;
    {
        // Only hold the db lock while serializing, not while writing to disk.
        lock_guard<mutex> lock(db_mutex);
        dirtyAtSnapshot = dirty;

        const auto steadyNow = chrono::steady_clock::now();
        const long long wallNow = wallClockMs();

        ostringstream out;
        out << DUMP_HEADER;
        auto writeStr = [&out](const string &s) { out << s.size() << ':' << s; };

        for(const auto &entry : type_store) {
            const string &key = entry.first;
            long long expireAt = -1;
            auto expIt = expiry_map.find(key);
            if(expIt != expiry_map.end()) {
                if(expIt->second <= steadyNow) continue;
                expireAt = wallNow + chrono::duration_cast<chrono::milliseconds>(expIt->second - steadyNow).count();
            }

            if(entry.second == RedisType::STRING) {
                auto it = kv_store.find(key);
                if(it == kv_store.end()) continue;
                out << "K " << expireAt << ' '; writeStr(key); out << " 1 "; writeStr(it->second);
            }
            else if(entry.second == RedisType::LIST) {
                auto it = list_store.find(key);
                if(it == list_store.end()) continue;
                out << "L " << expireAt << ' '; writeStr(key); out << ' ' << it->second.size();
                for(const auto &item : it->second) { out << ' '; writeStr(item); }
            }
            else if(entry.second == RedisType::HASH) {
                auto it = hash_store.find(key);
                if(it == hash_store.end()) continue;
                out << "H " << expireAt << ' '; writeStr(key); out << ' ' << it->second.size() * 2;
                for(const auto &field_val : it->second) {
                    out << ' '; writeStr(field_val.first);
                    out << ' '; writeStr(field_val.second);
                }
            }
            else if(entry.second == RedisType::SET) {
                auto it = set_store.find(key);
                if(it == set_store.end()) continue;
                out << "S " << expireAt << ' '; writeStr(key); out << ' ' << it->second.size();
                for(const auto &member : it->second) { out << ' '; writeStr(member); }
            }
            out << '\n';
        }
        data = out.str();
    }

    // Write to a temp file and rename it, so a crash mid-write never corrupts the old dump.
    lock_guard<mutex> fileLock(dump_mutex);
    const string tmpFile = filename + ".tmp";
    {
        ofstream ofs(tmpFile ,ios::binary | ios::trunc);
        if(!ofs) return false;
        ofs.write(data.data() ,data.size());
        ofs.flush();
        if(!ofs) return false;
    }
    error_code ec;
    filesystem::rename(tmpFile ,filename ,ec);
    if(ec) return false;

    dirty -= dirtyAtSnapshot;
    last_save = time(nullptr);
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
    string data((istreambuf_iterator<char>(ifs)) ,istreambuf_iterator<char>());

    // Keep the current data around so a corrupt file doesn't wipe the database.
    auto kvBackup = move(kv_store);
    auto listBackup = move(list_store);
    auto hashBackup = move(hash_store);
    auto setBackup = move(set_store);
    auto expiryBackup = move(expiry_map);
    auto typeBackup = move(type_store);
    clearUnlocked();

    const bool ok = data.compare(0 ,DUMP_HEADER.size() ,DUMP_HEADER) == 0 ? loadV2(data) : loadLegacy(data);
    if(!ok) {
        kv_store = move(kvBackup);
        list_store = move(listBackup);
        hash_store = move(hashBackup);
        set_store = move(setBackup);
        expiry_map = move(expiryBackup);
        type_store = move(typeBackup);
        return false;
    }
    dirty = 0;
    return true;
}

bool RedisDatabase::loadV2(const string &data) {
    size_t pos = DUMP_HEADER.size();
    const size_t size = data.size();

    auto expect = [&](char c) {
        if(pos >= size || data[pos] != c) return false;
        pos++;
        return true;
    };
    auto readInt = [&](long long &n) {
        size_t start = pos;
        if(pos < size && data[pos] == '-') pos++;
        while(pos < size && isdigit(static_cast<unsigned char>(data[pos]))) pos++;
        return parseInteger(data.substr(start ,pos - start) ,n);
    };
    auto readStr = [&](string &s) {
        long long len;
        if(!readInt(len) || !expect(':') || len < 0 || pos + len > size) return false;
        s = data.substr(pos ,len);
        pos += len;
        return true;
    };

    const auto steadyNow = chrono::steady_clock::now();
    const long long wallNow = wallClockMs();

    while(pos < size) {
        char type = data[pos++];
        long long expireAt ,count;
        string key;
        if(!expect(' ') || !readInt(expireAt) || !expect(' ') || !readStr(key) || !expect(' ') || !readInt(count) || count < 0)
            return false;

        vector<string> items;
        for(long long i=0 ;i<count ;i++) {
            string item;
            if(!expect(' ') || !readStr(item)) return false;
            items.push_back(move(item));
        }
        if(!expect('\n')) return false;

        // Skip keys that expired while the server was offline
        if(expireAt != -1 && expireAt <= wallNow) continue;

        if(type == 'K' && items.size() == 1) {
            kv_store[key] = items[0];
            type_store[key] = RedisType::STRING;
        }
        else if(type == 'L') {
            list_store[key] = deque<string>(items.begin() ,items.end());
            type_store[key] = RedisType::LIST;
        }
        else if(type == 'H' && items.size() % 2 == 0) {
            auto &hash = hash_store[key];
            for(size_t i=0 ;i<items.size() ;i+=2) hash[items[i]] = items[i+1];
            type_store[key] = RedisType::HASH;
        }
        else if(type == 'S') {
            set_store[key] = unordered_set<string>(items.begin() ,items.end());
            type_store[key] = RedisType::SET;
        }
        else return false;

        if(expireAt != -1)
            expiry_map[key] = steadyNow + chrono::milliseconds(expireAt - wallNow);
    }
    return true;
}

// Old v1 format: "K key value" / "L key a b c" / "H key f:v f:v"
bool RedisDatabase::loadLegacy(const string &data) {
    istringstream ifs(data);

    string line;
    while(getline(ifs ,line)) {
        istringstream iss(line);
        char type;

        if(!(iss >> type)) continue;

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

void RedisDatabase::setDumpFile(const string &filename) {
    lock_guard<mutex> lock(db_mutex);
    dump_file = filename;
}

string RedisDatabase::getDumpFile() {
    lock_guard<mutex> lock(db_mutex);
    return dump_file;
}

//Stats
void RedisDatabase::noteWrite() { dirty++; }
long long RedisDatabase::changesSinceSave() const { return dirty; }
long long RedisDatabase::lastSaveTime() const { return last_save; }

long long RedisDatabase::expiredKeys() {
    lock_guard<mutex> lock(db_mutex);
    return expired_keys;
}

map<string ,size_t> RedisDatabase::typeCounts() {
    lock_guard<mutex> lock(db_mutex);
    map<string ,size_t> counts;
    for(const auto &entry : type_store) counts[getString(entry.second)]++;
    return counts;
}

size_t RedisDatabase::expiresCount() {
    lock_guard<mutex> lock(db_mutex);
    return expiry_map.size();
}

//Key - Value Operations

//Helper fns
string RedisDatabase::getString(RedisType type) {
    if(type == RedisType::STRING) return "string";
    else if(type == RedisType::LIST) return "list";
    else if(type == RedisType::HASH) return "hash";
    else if(type == RedisType::SET) return "set";
    else return "none";
}

bool RedisDatabase::checkExpiry(const string &key) {
    auto it = expiry_map.find(key);
    if(it != expiry_map.end() && it->second <= chrono::steady_clock::now()) {
        if(deleteUnlocked(key)) {
            expired_keys++;
            return true;
        }
        else return false;
    }
    return false;
}

RedisType* RedisDatabase::findTypeUnlocked(const string &key) {
    checkExpiry(key);
    auto it = type_store.find(key);
    return it == type_store.end() ? nullptr : &it->second;
}

// Strict integer parsing: the whole string must be a number ("12abc" is rejected)
bool RedisDatabase::parseInteger(const string &str ,long long &value) {
    if(str.empty() || str.size() > 20 || isspace(static_cast<unsigned char>(str[0]))) return false;
    errno = 0;
    char *end = nullptr;
    long long parsed = strtoll(str.c_str() ,&end ,10);
    if(errno == ERANGE || end != str.c_str() + str.size()) return false;
    value = parsed;
    return true;
}

static bool parseLongDouble(const string &str ,long double &value) {
    if(str.empty() || isspace(static_cast<unsigned char>(str[0]))) return false;
    errno = 0;
    char *end = nullptr;
    long double parsed = strtold(str.c_str() ,&end);
    if(errno == ERANGE || end != str.c_str() + str.size() || isnan(parsed)) return false;
    value = parsed;
    return true;
}

// Redis style glob matching used by KEYS: * ? [abc] [^a] [a-z] and \ escapes
static bool globMatchFrom(const string &p ,size_t pi ,const string &s ,size_t si) {
    while(pi < p.size()) {
        char c = p[pi];
        if(c == '*') {
            while(pi < p.size() && p[pi] == '*') pi++;
            if(pi == p.size()) return true;
            for(; si <= s.size(); si++)
                if(globMatchFrom(p ,pi ,s ,si)) return true;
            return false;
        }
        if(si >= s.size()) return false;

        if(c == '?') { pi++; si++; continue; }

        if(c == '[') {
            size_t j = pi + 1;
            bool negate = j < p.size() && p[j] == '^';
            if(negate) j++;
            bool matched = false;
            while(j < p.size() && p[j] != ']') {
                if(p[j] == '\\' && j + 1 < p.size()) {
                    j++;
                    if(p[j] == s[si]) matched = true;
                }
                else if(j + 2 < p.size() && p[j+1] == '-' && p[j+2] != ']') {
                    char lo = p[j] ,hi = p[j+2];
                    if(lo > hi) swap(lo ,hi);
                    if(s[si] >= lo && s[si] <= hi) matched = true;
                    j += 2;
                }
                else if(p[j] == s[si]) matched = true;
                j++;
            }
            if(j >= p.size()) {
                // No closing bracket: treat '[' as a literal character
                if(s[si] != '[') return false;
                pi++; si++;
                continue;
            }
            if(matched == negate) return false;
            pi = j + 1; si++;
            continue;
        }

        if(c == '\\' && pi + 1 < p.size()) c = p[++pi];
        if(c != s[si]) return false;
        pi++; si++;
    }
    return si == s.size();
}

bool RedisDatabase::globMatch(const string &pattern ,const string &str) {
    return globMatchFrom(pattern ,0 ,str ,0);
}

void RedisDatabase::clearUnlocked() {
    kv_store.clear();
    list_store.clear();
    hash_store.clear();
    set_store.clear();
    expiry_map.clear();
    type_store.clear();
}

bool RedisDatabase::flushAll() {
    lock_guard<mutex> lock(db_mutex);
    clearUnlocked();
    return true;
}

size_t RedisDatabase::dbSize() {
    lock_guard<mutex> lock(db_mutex);
    size_t count = 0;
    const auto now = chrono::steady_clock::now();
    for(const auto &entry : type_store) {
        auto expIt = expiry_map.find(entry.first);
        if(expIt == expiry_map.end() || expIt->second > now) count++;
    }
    return count;
}

// SET overwrites any existing key (of any type) and clears its TTL, like Redis.
bool RedisDatabase::set(const string &key ,const string &value ,long long ttlMs ,SetCondition cond) {
    lock_guard<mutex> lock(db_mutex);

    const bool exists = findTypeUnlocked(key) != nullptr;
    if(cond == SetCondition::NX && exists) return false;
    if(cond == SetCondition::XX && !exists) return false;

    if(exists) deleteUnlocked(key);
    kv_store[key] = value;
    type_store[key] = RedisType::STRING;
    if(ttlMs > 0) expiry_map[key] = chrono::steady_clock::now() + chrono::milliseconds(ttlMs);

    return true;
}

bool RedisDatabase::get(const string &key ,string &value) {
    lock_guard<mutex> lock(db_mutex);

    if(checkExpiry(key)) {
        cerr << key << " is expired\n";
        return false;
    }

    if(type_store.find(key) != type_store.end()) {
        string type = getString(type_store[key]);
        if(type != "string") {
            cerr << "The key: " << key << " is present as a " << type << "\n";
            return false;
        }
    }

    auto itr = kv_store.find(key);
    if(itr == kv_store.end()) return false;

    value = itr -> second;
    return true;
}

void RedisDatabase::mset(const vector<pair<string ,string>> &keyValues) {
    lock_guard<mutex> lock(db_mutex);
    for(const auto &kv : keyValues) {
        deleteUnlocked(kv.first);
        kv_store[kv.first] = kv.second;
        type_store[kv.first] = RedisType::STRING;
    }
}

OpStatus RedisDatabase::getSet(const string &key ,const string &value ,string &oldValue) {
    lock_guard<mutex> lock(db_mutex);
    RedisType *type = findTypeUnlocked(key);
    if(type && *type != RedisType::STRING) return OpStatus::WRONG_TYPE;

    OpStatus status = type ? OpStatus::OK : OpStatus::NOT_FOUND;
    if(type) oldValue = kv_store[key];
    kv_store[key] = value;
    type_store[key] = RedisType::STRING;
    expiry_map.erase(key);
    return status;
}

OpStatus RedisDatabase::getDel(const string &key ,string &value) {
    lock_guard<mutex> lock(db_mutex);
    RedisType *type = findTypeUnlocked(key);
    if(!type) return OpStatus::NOT_FOUND;
    if(*type != RedisType::STRING) return OpStatus::WRONG_TYPE;
    value = kv_store[key];
    deleteUnlocked(key);
    return OpStatus::OK;
}

OpStatus RedisDatabase::incrBy(const string &key ,long long delta ,long long &result) {
    lock_guard<mutex> lock(db_mutex);
    RedisType *type = findTypeUnlocked(key);
    if(type && *type != RedisType::STRING) return OpStatus::WRONG_TYPE;

    long long current = 0;
    if(type && !parseInteger(kv_store[key] ,current)) return OpStatus::NOT_INTEGER;
    if(__builtin_add_overflow(current ,delta ,&result)) return OpStatus::OVERFLOW;

    kv_store[key] = to_string(result);
    type_store[key] = RedisType::STRING;
    return OpStatus::OK;
}

OpStatus RedisDatabase::incrByFloat(const string &key ,long double delta ,string &result) {
    lock_guard<mutex> lock(db_mutex);
    RedisType *type = findTypeUnlocked(key);
    if(type && *type != RedisType::STRING) return OpStatus::WRONG_TYPE;

    long double current = 0;
    if(type && !parseLongDouble(kv_store[key] ,current)) return OpStatus::NOT_FLOAT;
    long double updated = current + delta;
    if(isnan(updated) || isinf(updated)) return OpStatus::NOT_FLOAT;

    char buffer[64];
    snprintf(buffer ,sizeof(buffer) ,"%.17Lg" ,updated);
    result = buffer;
    kv_store[key] = result;
    type_store[key] = RedisType::STRING;
    return OpStatus::OK;
}

OpStatus RedisDatabase::append(const string &key ,const string &value ,size_t &newLength) {
    lock_guard<mutex> lock(db_mutex);
    RedisType *type = findTypeUnlocked(key);
    if(type && *type != RedisType::STRING) return OpStatus::WRONG_TYPE;

    string &current = kv_store[key];
    current += value;
    type_store[key] = RedisType::STRING;
    newLength = current.size();
    return OpStatus::OK;
}

OpStatus RedisDatabase::stringLength(const string &key ,size_t &length) {
    lock_guard<mutex> lock(db_mutex);
    length = 0;
    RedisType *type = findTypeUnlocked(key);
    if(!type) return OpStatus::OK;
    if(*type != RedisType::STRING) return OpStatus::WRONG_TYPE;
    length = kv_store[key].size();
    return OpStatus::OK;
}

OpStatus RedisDatabase::getRange(const string &key ,long long start ,long long end ,string &result) {
    lock_guard<mutex> lock(db_mutex);
    result.clear();
    RedisType *type = findTypeUnlocked(key);
    if(!type) return OpStatus::OK;
    if(*type != RedisType::STRING) return OpStatus::WRONG_TYPE;

    const string &value = kv_store[key];
    const long long size = static_cast<long long>(value.size());
    if(start < 0) start = size + start;
    if(end < 0) end = size + end;
    start = max(0LL ,start);
    end = max(0LL ,end);
    end = min(end ,size - 1);
    if(size == 0 || start > end) return OpStatus::OK;
    result = value.substr(start ,end - start + 1);
    return OpStatus::OK;
}

vector<string> RedisDatabase::keys(const string &pattern) {
    lock_guard<mutex> lock(db_mutex);

    vector<string> result;
    vector<string> expired;
    const auto now = chrono::steady_clock::now();

    for(auto &pair : type_store) {
        auto expIt = expiry_map.find(pair.first);
        if(expIt != expiry_map.end() && expIt->second <= now) {
            expired.push_back(pair.first);
            continue;
        }
        if(globMatch(pattern ,pair.first)) result.push_back(pair.first);
    }

    for(const string &key : expired) {
        deleteUnlocked(key);
        expired_keys++;
    }

    sort(result.begin() ,result.end());
    return result;
}

string RedisDatabase::type(const string &key) {
    lock_guard<mutex> lock(db_mutex);

    RedisType *type = findTypeUnlocked(key);
    return type ? getString(*type) : "none";
}

bool RedisDatabase::exists(const string &key) {
    lock_guard<mutex> lock(db_mutex);
    return findTypeUnlocked(key) != nullptr;
}

bool RedisDatabase::deleteUnlocked(const string &key) {
    bool erased = false;
    erased |= (kv_store.erase(key) > 0);
    erased |= (list_store.erase(key) > 0);
    erased |= (hash_store.erase(key) > 0);
    erased |= (set_store.erase(key) > 0);
    erased |= (expiry_map.erase(key) > 0);
    erased |= (type_store.erase(key) > 0);

    return erased;
}

bool RedisDatabase::del(const string &key) {
    lock_guard<mutex> lock(db_mutex);
    if(checkExpiry(key)) return false;
    return deleteUnlocked(key);
}

bool RedisDatabase::expire(const string &key ,int seconds) {
    return pexpire(key ,static_cast<long long>(seconds) * 1000);
}

bool RedisDatabase::pexpire(const string &key ,long long milliseconds) {
    lock_guard<mutex> lock(db_mutex);

    if(!findTypeUnlocked(key)) return false;

    // A non-positive TTL deletes the key right away (same as Redis)
    if(milliseconds <= 0) {
        deleteUnlocked(key);
        return true;
    }

    expiry_map[key] = chrono::steady_clock::now() + chrono::milliseconds(milliseconds);
    return true;
}

long long RedisDatabase::pttl(const string &key) {
    lock_guard<mutex> lock(db_mutex);

    if(!findTypeUnlocked(key)) return -2;
    auto it = expiry_map.find(key);
    if(it == expiry_map.end()) return -1;

    auto left = chrono::duration_cast<chrono::milliseconds>(it->second - chrono::steady_clock::now()).count();
    return max<long long>(0 ,left);
}

bool RedisDatabase::persist(const string &key) {
    lock_guard<mutex> lock(db_mutex);
    if(!findTypeUnlocked(key)) return false;
    return expiry_map.erase(key) > 0;
}

bool RedisDatabase::rename(const string &oldKey ,const string &newKey) {
    lock_guard<mutex> lock(db_mutex);

    if(checkExpiry(oldKey)) {
        cerr << oldKey << " is expired\n";
        return false;
    }

    const bool exists = kv_store.count(oldKey) || list_store.count(oldKey) ||
                        hash_store.count(oldKey) || set_store.count(oldKey);
    if(!exists) return false;

    if(oldKey == newKey) return true;

    deleteUnlocked(newKey);

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

    auto itrSet = set_store.find(oldKey);
    if(itrSet != set_store.end()) {
        set_store.emplace(newKey, move(itrSet->second));
        set_store.erase(itrSet);

        type_store[newKey] = RedisType::SET;
    }

    auto itrExpiry = expiry_map.find(oldKey);
    if(itrExpiry != expiry_map.end()) {
        expiry_map.emplace(newKey, itrExpiry->second);
        expiry_map.erase(itrExpiry);
    }

    type_store.erase(oldKey);
    return true;
}

bool RedisDatabase::randomKey(string &key) {
    lock_guard<mutex> lock(db_mutex);

    // A few attempts in case we keep landing on expired keys
    for(int attempt = 0; attempt < 100 && !type_store.empty(); attempt++) {
        auto it = type_store.begin();
        advance(it ,rng() % type_store.size());
        string candidate = it->first;
        if(checkExpiry(candidate)) continue;
        key = candidate;
        return true;
    }
    return false;
}

int RedisDatabase::purgeExpired(int maxKeys) {
    lock_guard<mutex> lock(db_mutex);

    const auto now = chrono::steady_clock::now();
    vector<string> victims;
    for(const auto &entry : expiry_map) {
        if(entry.second <= now) {
            victims.push_back(entry.first);
            if(static_cast<int>(victims.size()) >= maxKeys) break;
        }
    }

    for(const string &key : victims) deleteUnlocked(key);
    expired_keys += victims.size();
    return static_cast<int>(victims.size());
}

//List Operations
string RedisDatabase::lpush(const string &key ,const vector<string> &values) {
    lock_guard<mutex> lock(db_mutex);

    checkExpiry(key);

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

    checkExpiry(key);

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

OpStatus RedisDatabase::popUnlocked(const string &key ,int count ,bool fromFront ,vector<string> &values) {
    RedisType *type = findTypeUnlocked(key);
    if(!type) return OpStatus::NOT_FOUND;
    if(*type != RedisType::LIST) return OpStatus::WRONG_TYPE;

    deque<string> &list = list_store[key];
    for(int i=0 ;i<count && !list.empty() ;i++) {
        if(fromFront) { values.push_back(move(list.front())); list.pop_front(); }
        else { values.push_back(move(list.back())); list.pop_back(); }
    }

    // Empty lists are removed. Nothing touches `list` after this point.
    if(list.empty()) deleteUnlocked(key);
    return OpStatus::OK;
}

OpStatus RedisDatabase::lpop(const string &key ,int count ,vector<string> &values) {
    lock_guard<mutex> lock(db_mutex);
    return popUnlocked(key ,count ,true ,values);
}

OpStatus RedisDatabase::rpop(const string &key ,int count ,vector<string> &values) {
    lock_guard<mutex> lock(db_mutex);
    return popUnlocked(key ,count ,false ,values);
}

string RedisDatabase::llen(const string &key) {
    lock_guard<mutex> lock(db_mutex);

    if(checkExpiry(key)) return "0";

    if((type_store.find(key) != type_store.end()) && (type_store[key] != RedisType::LIST)) {
        cerr << "The key is already set as " << getString(type_store[key]) << "\n";
        return "-1";
    }

    if(type_store.find(key) != type_store.end()) return to_string(list_store[key].size());
    return "0";
}

bool RedisDatabase::lrange(const string &key ,const int start ,const int end ,vector<string> &values) {
    lock_guard<mutex> lock(db_mutex);

    if(checkExpiry(key)) return true;

    if((type_store.find(key) != type_store.end()) && (type_store[key] != RedisType::LIST)) {
        cerr << "The key is already set as " << getString(type_store[key]) << "\n";
        return false;
    }

    if(type_store.find(key) != type_store.end()) {
        deque<string> &currValues = list_store[key];

        const int size = static_cast<int>(currValues.size());
        int first = start < 0 ? size + start : start;
        int last = end < 0 ? size + end : end;
        first = max(0, first);
        last = min(size - 1, last);
        if(first > last || first >= size) return true;

        for(int i=first ;i<=last ;i++) {
            values.push_back(currValues[i]);
        }

        return true;
    }
    return true; // Redis returns an empty array for a missing list key.

}

bool RedisDatabase::lindex(const string &key, int index, string &value) {
    lock_guard<mutex> lock(db_mutex);
    if(checkExpiry(key)) return false;
    auto typeIt = type_store.find(key);
    if(typeIt == type_store.end() || typeIt->second != RedisType::LIST) return false;

    const deque<string> &list = list_store[key];
    const int resolved = index < 0 ? static_cast<int>(list.size()) + index : index;
    if(resolved < 0 || resolved >= static_cast<int>(list.size())) return false;
    value = list[resolved];
    return true;
}

bool RedisDatabase::lset(const string &key, int index, const string &value) {
    lock_guard<mutex> lock(db_mutex);
    if(checkExpiry(key)) return false;
    auto typeIt = type_store.find(key);
    if(typeIt == type_store.end() || typeIt->second != RedisType::LIST) return false;

    deque<string> &list = list_store[key];
    const int resolved = index < 0 ? static_cast<int>(list.size()) + index : index;
    if(resolved < 0 || resolved >= static_cast<int>(list.size())) return false;
    list[resolved] = value;
    return true;
}

int RedisDatabase::lrem(const string &key, int count, const string &value) {
    lock_guard<mutex> lock(db_mutex);
    if(checkExpiry(key)) return 0;
    auto typeIt = type_store.find(key);
    if(typeIt == type_store.end() || typeIt->second != RedisType::LIST) return 0;

    deque<string> &list = list_store[key];
    int removed = 0;
    if(count >= 0) {
        for(auto it = list.begin(); it != list.end() && (count == 0 || removed < count); ) {
            if(*it == value) { it = list.erase(it); ++removed; } else ++it;
        }
    } else {
        for(auto it = list.end(); it != list.begin() && removed < -count; ) {
            --it;
            if(*it == value) { it = list.erase(it); ++removed; }
        }
    }
    if(list.empty()) deleteUnlocked(key);
    return removed;
}

bool RedisDatabase::ltrim(const string &key, int start, int end) {
    lock_guard<mutex> lock(db_mutex);
    if(checkExpiry(key)) return true;
    auto typeIt = type_store.find(key);
    if(typeIt == type_store.end()) return true;
    if(typeIt->second != RedisType::LIST) return false;

    deque<string> &list = list_store[key];
    const int size = static_cast<int>(list.size());
    int first = start < 0 ? size + start : start;
    int last = end < 0 ? size + end : end;
    first = max(0, first);
    last = min(size - 1, last);
    if(first > last || first >= size) { deleteUnlocked(key); return true; }
    list.erase(list.begin() + last + 1, list.end());
    list.erase(list.begin(), list.begin() + first);
    return true;
}

// newLength: list length after insert, -1 if the pivot was not found
OpStatus RedisDatabase::linsert(const string &key, bool before, const string &pivot, const string &value, long long &newLength) {
    lock_guard<mutex> lock(db_mutex);
    newLength = 0;
    RedisType *type = findTypeUnlocked(key);
    if(!type) return OpStatus::NOT_FOUND;
    if(*type != RedisType::LIST) return OpStatus::WRONG_TYPE;

    deque<string> &list = list_store[key];
    auto it = find(list.begin(), list.end(), pivot);
    if(it == list.end()) { newLength = -1; return OpStatus::OK; }
    list.insert(before ? it : next(it), value);
    newLength = static_cast<long long>(list.size());
    return OpStatus::OK;
}

// Hash Operations
bool RedisDatabase::hset(const string &key, const vector<pair<string, string>> &fieldValues, int &added) {
    lock_guard<mutex> lock(db_mutex);
    added = 0;
    if(checkExpiry(key)) { /* expired keys are recreated below */ }
    auto typeIt = type_store.find(key);
    if(typeIt != type_store.end() && typeIt->second != RedisType::HASH) return false;

    unordered_map<string, string> &hash = hash_store[key];
    for(const auto &fieldValue : fieldValues) {
        if(hash.find(fieldValue.first) == hash.end()) ++added;
        hash[fieldValue.first] = fieldValue.second;
    }
    type_store[key] = RedisType::HASH;
    return true;
}

OpStatus RedisDatabase::hsetnx(const string &key, const string &field, const string &value, bool &set) {
    lock_guard<mutex> lock(db_mutex);
    set = false;
    RedisType *type = findTypeUnlocked(key);
    if(type && *type != RedisType::HASH) return OpStatus::WRONG_TYPE;

    unordered_map<string, string> &hash = hash_store[key];
    type_store[key] = RedisType::HASH;
    set = hash.emplace(field, value).second;
    return OpStatus::OK;
}

bool RedisDatabase::hget(const string &key, const string &field, string &value) {
    lock_guard<mutex> lock(db_mutex);
    if(checkExpiry(key)) return false;
    auto typeIt = type_store.find(key);
    if(typeIt == type_store.end() || typeIt->second != RedisType::HASH) return false;
    auto fieldIt = hash_store[key].find(field);
    if(fieldIt == hash_store[key].end()) return false;
    value = fieldIt->second;
    return true;
}

int RedisDatabase::hdel(const string &key, const vector<string> &fields) {
    lock_guard<mutex> lock(db_mutex);
    if(checkExpiry(key)) return 0;
    auto typeIt = type_store.find(key);
    if(typeIt == type_store.end() || typeIt->second != RedisType::HASH) return 0;
    unordered_map<string, string> &hash = hash_store[key];
    int removed = 0;
    for(const string &field : fields) removed += hash.erase(field) > 0;
    if(hash.empty()) deleteUnlocked(key);
    return removed;
}

bool RedisDatabase::hexists(const string &key, const string &field, bool &exists) {
    lock_guard<mutex> lock(db_mutex);
    exists = false;
    if(checkExpiry(key)) return true;
    auto typeIt = type_store.find(key);
    if(typeIt == type_store.end()) return true;
    if(typeIt->second != RedisType::HASH) return false;
    exists = hash_store[key].find(field) != hash_store[key].end();
    return true;
}

int RedisDatabase::hlen(const string &key) {
    lock_guard<mutex> lock(db_mutex);
    if(checkExpiry(key)) return 0;
    auto typeIt = type_store.find(key);
    if(typeIt == type_store.end() || typeIt->second != RedisType::HASH) return 0;
    return static_cast<int>(hash_store[key].size());
}

bool RedisDatabase::hgetall(const string &key, vector<pair<string, string>> &fieldValues) {
    lock_guard<mutex> lock(db_mutex);
    if(checkExpiry(key)) return true;
    auto typeIt = type_store.find(key);
    if(typeIt == type_store.end()) return true;
    if(typeIt->second != RedisType::HASH) return false;
    for(const auto &fieldValue : hash_store[key]) fieldValues.push_back(fieldValue);
    sort(fieldValues.begin(), fieldValues.end());
    return true;
}

bool RedisDatabase::hkeys(const string &key, vector<string> &fields) {
    vector<pair<string, string>> fieldValues;
    if(!hgetall(key, fieldValues)) return false;
    for(const auto &fieldValue : fieldValues) fields.push_back(fieldValue.first);
    return true;
}

bool RedisDatabase::hvals(const string &key, vector<string> &values) {
    vector<pair<string, string>> fieldValues;
    if(!hgetall(key, fieldValues)) return false;
    for(const auto &fieldValue : fieldValues) values.push_back(fieldValue.second);
    return true;
}

OpStatus RedisDatabase::hincrBy(const string &key, const string &field, long long delta, long long &result) {
    lock_guard<mutex> lock(db_mutex);
    RedisType *type = findTypeUnlocked(key);
    if(type && *type != RedisType::HASH) return OpStatus::WRONG_TYPE;

    unordered_map<string, string> &hash = hash_store[key];
    type_store[key] = RedisType::HASH;

    long long current = 0;
    auto fieldIt = hash.find(field);
    if(fieldIt != hash.end() && !parseInteger(fieldIt->second, current)) return OpStatus::NOT_INTEGER;
    if(__builtin_add_overflow(current, delta, &result)) return OpStatus::OVERFLOW;
    hash[field] = to_string(result);
    return OpStatus::OK;
}

// Set Operations
OpStatus RedisDatabase::sadd(const string &key, const vector<string> &members, int &added) {
    lock_guard<mutex> lock(db_mutex);
    added = 0;
    RedisType *type = findTypeUnlocked(key);
    if(type && *type != RedisType::SET) return OpStatus::WRONG_TYPE;

    unordered_set<string> &set = set_store[key];
    for(const string &member : members) added += set.insert(member).second;
    type_store[key] = RedisType::SET;
    return OpStatus::OK;
}

OpStatus RedisDatabase::srem(const string &key, const vector<string> &members, int &removed) {
    lock_guard<mutex> lock(db_mutex);
    removed = 0;
    RedisType *type = findTypeUnlocked(key);
    if(!type) return OpStatus::OK;
    if(*type != RedisType::SET) return OpStatus::WRONG_TYPE;

    unordered_set<string> &set = set_store[key];
    for(const string &member : members) removed += set.erase(member) > 0;
    if(set.empty()) deleteUnlocked(key);
    return OpStatus::OK;
}

OpStatus RedisDatabase::sismember(const string &key, const string &member, bool &isMember) {
    lock_guard<mutex> lock(db_mutex);
    isMember = false;
    RedisType *type = findTypeUnlocked(key);
    if(!type) return OpStatus::OK;
    if(*type != RedisType::SET) return OpStatus::WRONG_TYPE;
    isMember = set_store[key].count(member) > 0;
    return OpStatus::OK;
}

OpStatus RedisDatabase::smembers(const string &key, vector<string> &members) {
    lock_guard<mutex> lock(db_mutex);
    RedisType *type = findTypeUnlocked(key);
    if(!type) return OpStatus::OK;
    if(*type != RedisType::SET) return OpStatus::WRONG_TYPE;
    const unordered_set<string> &set = set_store[key];
    members.assign(set.begin(), set.end());
    sort(members.begin(), members.end());
    return OpStatus::OK;
}

OpStatus RedisDatabase::scard(const string &key, size_t &count) {
    lock_guard<mutex> lock(db_mutex);
    count = 0;
    RedisType *type = findTypeUnlocked(key);
    if(!type) return OpStatus::OK;
    if(*type != RedisType::SET) return OpStatus::WRONG_TYPE;
    count = set_store[key].size();
    return OpStatus::OK;
}

OpStatus RedisDatabase::spop(const string &key, int count, vector<string> &members) {
    lock_guard<mutex> lock(db_mutex);
    RedisType *type = findTypeUnlocked(key);
    if(!type) return OpStatus::NOT_FOUND;
    if(*type != RedisType::SET) return OpStatus::WRONG_TYPE;

    unordered_set<string> &set = set_store[key];
    for(int i=0 ;i<count && !set.empty() ;i++) {
        auto it = set.begin();
        advance(it, rng() % set.size());
        members.push_back(*it);
        set.erase(it);
    }
    if(set.empty()) deleteUnlocked(key);
    return OpStatus::OK;
}

OpStatus RedisDatabase::srandmember(const string &key, string &member) {
    lock_guard<mutex> lock(db_mutex);
    RedisType *type = findTypeUnlocked(key);
    if(!type) return OpStatus::NOT_FOUND;
    if(*type != RedisType::SET) return OpStatus::WRONG_TYPE;

    const unordered_set<string> &set = set_store[key];
    auto it = set.begin();
    advance(it, rng() % set.size());
    member = *it;
    return OpStatus::OK;
}

OpStatus RedisDatabase::setOperation(char op, const vector<string> &keys, vector<string> &result) {
    lock_guard<mutex> lock(db_mutex);

    // Missing keys behave like empty sets
    static const unordered_set<string> emptySet;
    vector<const unordered_set<string>*> sets;
    for(const string &key : keys) {
        RedisType *type = findTypeUnlocked(key);
        if(!type) { sets.push_back(&emptySet); continue; }
        if(*type != RedisType::SET) return OpStatus::WRONG_TYPE;
        sets.push_back(&set_store[key]);
    }
    if(sets.empty()) return OpStatus::OK;

    if(op == 'U') {
        unordered_set<string> merged;
        for(const auto *set : sets) merged.insert(set->begin(), set->end());
        result.assign(merged.begin(), merged.end());
    }
    else {
        for(const string &member : *sets[0]) {
            bool keep = true;
            for(size_t i=1 ;i<sets.size() && keep ;i++) {
                const bool inOther = sets[i]->count(member) > 0;
                keep = (op == 'I') ? inOther : !inOther;
            }
            if(keep) result.push_back(member);
        }
    }
    sort(result.begin(), result.end());
    return OpStatus::OK;
}
