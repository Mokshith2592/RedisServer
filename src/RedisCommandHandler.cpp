#include <bits/stdc++.h>
using namespace std;

#include "../include/RedisCommandHandler.h"
#include "../include/RedisDatabase.h"

ServerStats& serverStats() {
    static ServerStats stats;
    return stats;
}

static const char* SERVER_VERSION = "1.1.0";

// Limits that protect the server from absurd or malicious requests
static const long long MAX_ARGS = 1024 * 1024;
static const long long MAX_BULK_LEN = 512LL * 1024 * 1024;

static const vector<string> SUPPORTED_COMMANDS = {
    // Common / server
    "PING", "ECHO", "FLUSHALL", "FLUSHDB", "DBSIZE", "INFO", "TIME", "SELECT", "QUIT",
    "COMMAND", "CONFIG", "HELP", "DUMP", "SAVE", "BGSAVE", "LOAD",
    // Strings
    "SET", "GET", "SETNX", "SETEX", "PSETEX", "MSET", "MGET", "GETSET", "GETDEL",
    "INCR", "DECR", "INCRBY", "DECRBY", "INCRBYFLOAT", "APPEND", "STRLEN", "GETRANGE",
    // Keys
    "KEYS", "TYPE", "DEL", "UNLINK", "EXISTS", "EXPIRE", "PEXPIRE", "TTL", "PTTL",
    "PERSIST", "RENAME", "RANDOMKEY",
    // Lists
    "LPUSH", "RPUSH", "LPOP", "RPOP", "LLEN", "LRANGE", "LINDEX", "LSET", "LREM", "LTRIM", "LINSERT",
    // Hashes
    "HSET", "HMSET", "HSETNX", "HGET", "HMGET", "HDEL", "HEXISTS", "HLEN", "HGETALL",
    "HKEYS", "HVALS", "HINCRBY",
    // Sets
    "SADD", "SREM", "SISMEMBER", "SMEMBERS", "SCARD", "SPOP", "SRANDMEMBER",
    "SINTER", "SUNION", "SDIFF"
};

// Commands that modify the dataset (counted for INFO's changes_since_last_save)
static const unordered_set<string> WRITE_COMMANDS = {
    "FLUSHALL", "FLUSHDB", "SET", "SETNX", "SETEX", "PSETEX", "MSET", "GETSET", "GETDEL",
    "INCR", "DECR", "INCRBY", "DECRBY", "INCRBYFLOAT", "APPEND", "DEL", "UNLINK", "EXPIRE",
    "PEXPIRE", "PERSIST", "RENAME", "LPUSH", "RPUSH", "LPOP", "RPOP", "LSET", "LREM",
    "LTRIM", "LINSERT", "HSET", "HMSET", "HSETNX", "HDEL", "HINCRBY", "SADD", "SREM", "SPOP"
};

// Strict number parsing for arguments. Throws so execute() can reply with an error.
static long long toLong(const string &s) {
    long long value;
    if(!RedisDatabase::parseInteger(s ,value)) throw invalid_argument("not an integer");
    return value;
}

static int toInt(const string &s) {
    long long value = toLong(s);
    if(value < INT_MIN || value > INT_MAX) throw out_of_range("integer out of range");
    return static_cast<int>(value);
}

static long double toLongDouble(const string &s) {
    char *end = nullptr;
    errno = 0;
    long double value = s.empty() || isspace(static_cast<unsigned char>(s[0])) ? 0 : strtold(s.c_str() ,&end);
    if(end != s.c_str() + s.size() || errno == ERANGE || isnan(value) || isinf(value)) throw invalid_argument("not a float");
    return value;
}

static string toUpper(string s) {
    transform(s.begin() ,s.end() ,s.begin() ,::toupper);
    return s;
}

// Split an inline command ("SET name \"Alice Smith\"") into tokens.
// Supports double quotes (with \n \r \t \" \\ escapes) and single quotes.
static bool splitInline(const string &line ,vector<string> &tokens) {
    size_t i = 0;
    auto isSpace = [](char c) { return isspace(static_cast<unsigned char>(c)) != 0; };

    while(true) {
        while(i < line.size() && isSpace(line[i])) i++;
        if(i >= line.size()) return true;

        string token;
        char quote = line[i];
        if(quote == '"' || quote == '\'') {
            i++;
            while(true) {
                if(i >= line.size()) return false;  // unbalanced quotes
                char c = line[i];
                if(c == '\\' && i + 1 < line.size()) {
                    char n = line[i+1];
                    if(quote == '"') token += (n == 'n') ? '\n' : (n == 'r') ? '\r' : (n == 't') ? '\t' : n;
                    else token += (n == '\'') ? string(1 ,n) : string{c ,n};
                    i += 2;
                }
                else if(c == quote) { i++; break; }
                else { token += c; i++; }
            }
            // A closing quote must be followed by a space or the end of the line
            if(i < line.size() && !isSpace(line[i])) return false;
        }
        else {
            while(i < line.size() && !isSpace(line[i])) token += line[i++];
        }
        tokens.push_back(token);
    }
}

// RESP(Redis Serialization Protocol) parser:
// *2\r\n$4\r\nPING\r\n$4\r\nTEST\r\n
// *2 -> array has 2 elements
// $4 -> next string has 4 characters
// PING
// TEST
//
// Anything that doesn't start with '*' is an inline command terminated by '\n'.
ParseStatus RedisCommandHandler::parseNext(const string &input ,size_t start ,vector<string> &tokens ,size_t &consumed) {
    tokens.clear();
    if(start >= input.size()) return ParseStatus::INCOMPLETE;

    if(input[start] != '*') {
        size_t newline = input.find('\n' ,start);
        if(newline == string::npos) return ParseStatus::INCOMPLETE;

        string line = input.substr(start ,newline - start);
        if(!line.empty() && line.back() == '\r') line.pop_back();
        if(!splitInline(line ,tokens)) return ParseStatus::PROTOCOL_ERROR;

        consumed = newline + 1 - start;
        return ParseStatus::COMPLETE;
    }

    size_t pos = start + 1;
    // crlf = Carriage Return (\r) ,Line Feed (\n)
    size_t crlf = input.find("\r\n" ,pos);
    if(crlf == string::npos) return ParseStatus::INCOMPLETE;

    long long numElements;
    if(!RedisDatabase::parseInteger(input.substr(pos ,crlf - pos) ,numElements) || numElements > MAX_ARGS)
        return ParseStatus::PROTOCOL_ERROR;
    pos = crlf + 2;

    for(long long i=0 ;i<numElements ;i++) {
        if(pos >= input.size()) return ParseStatus::INCOMPLETE;
        if(input[pos] != '$') return ParseStatus::PROTOCOL_ERROR;
        pos++;

        crlf = input.find("\r\n" ,pos);
        if(crlf == string::npos) return ParseStatus::INCOMPLETE;

        long long len;
        if(!RedisDatabase::parseInteger(input.substr(pos ,crlf - pos) ,len) || len < 0 || len > MAX_BULK_LEN)
            return ParseStatus::PROTOCOL_ERROR;

        pos = crlf + 2;
        if(pos + len + 2 > input.size()) return ParseStatus::INCOMPLETE;
        if(input.compare(pos + len ,2 ,"\r\n") != 0) return ParseStatus::PROTOCOL_ERROR;

        tokens.push_back(input.substr(pos ,len));
        pos += (len + 2);
    }

    consumed = pos - start;
    return ParseStatus::COMPLETE;
}

RedisCommandHandler::RedisCommandHandler() {}

string RedisCommandHandler::processCommand(const string &commandLine) {
    if(commandLine.empty()) return "-Error: Empty Command\r\n";

    // Allow a single inline command without a trailing newline (handy for tests and tools)
    string input = commandLine;
    if(input[0] != '*' && input.back() != '\n') input += '\n';

    vector<string> tokens;
    size_t consumed = 0;
    if(parseNext(input ,0 ,tokens ,consumed) != ParseStatus::COMPLETE)
        return "-Error: Protocol error\r\n";

    return execute(tokens);
}

static string buildInfo(const string &section) {
    ServerStats &stats = serverStats();
    RedisDatabase &db = RedisDatabase::getInstance();
    const bool all = section.empty() || section == "all" || section == "default";
    const bool everything = section == "everything";
    auto want = [&](const string &name) { return all || everything || section == name; };

    ostringstream info;
    if(want("server")) {
        const long long uptime = chrono::duration_cast<chrono::seconds>(chrono::steady_clock::now() - stats.startTime).count();
        info << "# Server\r\n"
             << "server_name:my_redis_server\r\n"
             << "server_version:" << SERVER_VERSION << "\r\n"
             << "tcp_port:" << stats.port << "\r\n"
             << "uptime_in_seconds:" << uptime << "\r\n"
             << "uptime_in_days:" << uptime / 86400 << "\r\n\r\n";
    }
    if(want("clients")) {
        info << "# Clients\r\n"
             << "connected_clients:" << stats.connectedClients << "\r\n\r\n";
    }
    if(want("stats")) {
        info << "# Stats\r\n"
             << "total_connections_received:" << stats.totalConnections << "\r\n"
             << "total_commands_processed:" << stats.commandsProcessed << "\r\n"
             << "expired_keys:" << db.expiredKeys() << "\r\n\r\n";
    }
    if(want("persistence")) {
        info << "# Persistence\r\n"
             << "dump_file:" << db.getDumpFile() << "\r\n"
             << "changes_since_last_save:" << db.changesSinceSave() << "\r\n"
             << "last_save_time:" << db.lastSaveTime() << "\r\n\r\n";
    }
    if(want("keyspace")) {
        info << "# Keyspace\r\n"
             << "db0:keys=" << db.dbSize() << ",expires=" << db.expiresCount() << "\r\n";
        for(const auto &typeCount : db.typeCounts())
            info << "keys_" << typeCount.first << ":" << typeCount.second << "\r\n";
        info << "\r\n";
    }
    if(everything || section == "commandstats") {
        info << "# Commandstats\r\n";
        lock_guard<mutex> lock(stats.commandMutex);
        for(const auto &call : stats.commandCalls)
            info << "cmdstat_" << call.first << ":calls=" << call.second << "\r\n";
        info << "\r\n";
    }
    return info.str();
}

static string runCommand(const vector<string> &tokens) {
    string cmd = toUpper(tokens[0]);

    ostringstream response;

    RedisDatabase &db = RedisDatabase::getInstance();
    auto writeBulk = [&response](const string &value) {
        response << "$" << value.size() << "\r\n" << value << "\r\n";
    };
    auto writeArray = [&response](const vector<string> &values) {
        response << "*" << values.size() << "\r\n";
        for(const string &value : values)
            response << "$" << value.size() << "\r\n" << value << "\r\n";
    };
    auto writeStatusError = [&response](OpStatus status) {
        if(status == OpStatus::WRONG_TYPE) response << "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
        else if(status == OpStatus::NOT_INTEGER) response << "-Error: value is not an integer or out of range\r\n";
        else if(status == OpStatus::NOT_FLOAT) response << "-Error: value is not a valid float\r\n";
        else if(status == OpStatus::OVERFLOW) response << "-Error: increment or decrement would overflow\r\n";
        else response << "-Error: operation failed\r\n";
    };
    auto argsFrom = [&tokens](size_t first) { return vector<string>(tokens.begin() + first ,tokens.end()); };

    // Check commands

    //Common commands
    if(cmd == "PING") {
        if(tokens.size() >= 2) writeBulk(tokens[1]);
        else response << "+PONG\r\n";
    }
    else if(cmd == "ECHO") {
        if(tokens.size() < 2)
            response << "-Error: Echo requires a message\r\n";
        else
            response << "+" << tokens[1] << "\r\n";
    }
    else if(cmd == "FLUSHALL" || cmd == "FLUSHDB") {
        db.flushAll();
        response << "+OK\r\n";
    }
    else if(cmd == "DBSIZE") {
        response << ":" << db.dbSize() << "\r\n";
    }
    else if(cmd == "INFO") {
        string section = tokens.size() >= 2 ? tokens[1] : "";
        transform(section.begin() ,section.end() ,section.begin() ,::tolower);
        writeBulk(buildInfo(section));
    }
    else if(cmd == "TIME") {
        auto now = chrono::system_clock::now().time_since_epoch();
        long long micros = chrono::duration_cast<chrono::microseconds>(now).count();
        writeArray({to_string(micros / 1000000) ,to_string(micros % 1000000)});
    }
    else if(cmd == "SELECT") {
        // Only a single database (db 0) is supported
        if(tokens.size() != 2) response << "-Error: SELECT requires a db index\r\n";
        else if(toInt(tokens[1]) == 0) response << "+OK\r\n";
        else response << "-Error: DB index is out of range\r\n";
    }
    else if(cmd == "QUIT") {
        response << "+OK\r\n";
    }
    else if(cmd == "COMMAND") {
        // redis-cli sends "COMMAND DOCS" on connect; an empty reply keeps it happy
        if(tokens.size() >= 2 && toUpper(tokens[1]) == "COUNT") response << ":" << SUPPORTED_COMMANDS.size() << "\r\n";
        else response << "*0\r\n";
    }
    else if(cmd == "CONFIG") {
        // Only there so tools like redis-benchmark don't fail on CONFIG GET
        response << "*0\r\n";
    }
    else if(cmd == "HELP") {
        vector<string> names = SUPPORTED_COMMANDS;
        sort(names.begin() ,names.end());
        writeArray(names);
    }

    //Key-Value Operations
    else if(cmd == "SET") {
        if(tokens.size() < 3)
            response << "-Error: SET requires key and value\r\n";
        else {
            // SET key value [EX seconds | PX milliseconds] [NX | XX]
            long long ttlMs = -1;
            SetCondition cond = SetCondition::NONE;
            bool syntaxError = false;
            for(size_t i = 3; i < tokens.size() && !syntaxError; i++) {
                string option = toUpper(tokens[i]);
                if((option == "EX" || option == "PX") && i + 1 < tokens.size() && ttlMs == -1) {
                    long long amount = toLong(tokens[++i]);
                    if(amount <= 0) { response << "-Error: invalid expire time in 'set' command\r\n"; return response.str(); }
                    ttlMs = option == "EX" ? amount * 1000 : amount;
                }
                else if(option == "NX" && cond == SetCondition::NONE) cond = SetCondition::NX;
                else if(option == "XX" && cond == SetCondition::NONE) cond = SetCondition::XX;
                else syntaxError = true;
            }

            if(syntaxError) response << "-Error: syntax error\r\n";
            else if(db.set(tokens[1] ,tokens[2] ,ttlMs ,cond))
                response << "+OK\r\n";
            else
                response << "$-1\r\n";
        }
    }
    else if(cmd == "SETNX") {
        if(tokens.size() != 3) response << "-Error: SETNX requires key and value\r\n";
        else response << ":" << (db.set(tokens[1] ,tokens[2] ,-1 ,SetCondition::NX) ? 1 : 0) << "\r\n";
    }
    else if(cmd == "SETEX" || cmd == "PSETEX") {
        if(tokens.size() != 4) response << "-Error: " << cmd << " requires key, time and value\r\n";
        else {
            long long amount = toLong(tokens[2]);
            if(amount <= 0) response << "-Error: invalid expire time in '" << cmd << "' command\r\n";
            else {
                db.set(tokens[1] ,tokens[3] ,cmd == "SETEX" ? amount * 1000 : amount);
                response << "+OK\r\n";
            }
        }
    }
    else if(cmd == "GET") {
        if(tokens.size() < 2)
            response << "-Error: GET requires key\r\n";
        else {
            string value;
            if(db.get(tokens[1] ,value))
                writeBulk(value);
            else
                response << "$-1\r\n";
        }
    }
    else if(cmd == "MSET") {
        if(tokens.size() < 3 || tokens.size() % 2 == 0) response << "-Error: MSET requires key/value pairs\r\n";
        else {
            vector<pair<string ,string>> keyValues;
            for(size_t i = 1; i < tokens.size(); i += 2) keyValues.emplace_back(tokens[i] ,tokens[i+1]);
            db.mset(keyValues);
            response << "+OK\r\n";
        }
    }
    else if(cmd == "MGET") {
        if(tokens.size() < 2) response << "-Error: MGET requires keys\r\n";
        else {
            response << "*" << tokens.size() - 1 << "\r\n";
            for(size_t i = 1; i < tokens.size(); ++i) {
                string value;
                if(db.get(tokens[i] ,value)) writeBulk(value);
                else response << "$-1\r\n";
            }
        }
    }
    else if(cmd == "GETSET" || cmd == "GETDEL") {
        const size_t expected = cmd == "GETSET" ? 3 : 2;
        if(tokens.size() != expected) response << "-Error: " << cmd << " requires " << (expected == 3 ? "key and value" : "key") << "\r\n";
        else {
            string value;
            OpStatus status = cmd == "GETSET" ? db.getSet(tokens[1] ,tokens[2] ,value) : db.getDel(tokens[1] ,value);
            if(status == OpStatus::OK) writeBulk(value);
            else if(status == OpStatus::NOT_FOUND) response << "$-1\r\n";
            else writeStatusError(status);
        }
    }
    else if(cmd == "INCR" || cmd == "DECR" || cmd == "INCRBY" || cmd == "DECRBY") {
        const bool hasAmount = cmd == "INCRBY" || cmd == "DECRBY";
        if(tokens.size() != (hasAmount ? 3u : 2u)) response << "-Error: " << cmd << " requires key" << (hasAmount ? " and increment" : "") << "\r\n";
        else {
            long long delta = hasAmount ? toLong(tokens[2]) : 1;
            if(cmd[0] == 'D') {
                if(delta == LLONG_MIN) { writeStatusError(OpStatus::OVERFLOW); return response.str(); }
                delta = -delta;
            }
            long long result;
            OpStatus status = db.incrBy(tokens[1] ,delta ,result);
            if(status == OpStatus::OK) response << ":" << result << "\r\n";
            else writeStatusError(status);
        }
    }
    else if(cmd == "INCRBYFLOAT") {
        if(tokens.size() != 3) response << "-Error: INCRBYFLOAT requires key and increment\r\n";
        else {
            long double delta = toLongDouble(tokens[2]);
            string result;
            OpStatus status = db.incrByFloat(tokens[1] ,delta ,result);
            if(status == OpStatus::OK) writeBulk(result);
            else writeStatusError(status);
        }
    }
    else if(cmd == "APPEND") {
        if(tokens.size() != 3) response << "-Error: APPEND requires key and value\r\n";
        else {
            size_t length;
            OpStatus status = db.append(tokens[1] ,tokens[2] ,length);
            if(status == OpStatus::OK) response << ":" << length << "\r\n";
            else writeStatusError(status);
        }
    }
    else if(cmd == "STRLEN") {
        if(tokens.size() != 2) response << "-Error: STRLEN requires key\r\n";
        else {
            size_t length;
            OpStatus status = db.stringLength(tokens[1] ,length);
            if(status == OpStatus::OK) response << ":" << length << "\r\n";
            else writeStatusError(status);
        }
    }
    else if(cmd == "GETRANGE") {
        if(tokens.size() != 4) response << "-Error: GETRANGE requires key, start and end\r\n";
        else {
            string result;
            OpStatus status = db.getRange(tokens[1] ,toLong(tokens[2]) ,toLong(tokens[3]) ,result);
            if(status == OpStatus::OK) writeBulk(result);
            else writeStatusError(status);
        }
    }
    else if(cmd == "KEYS") {
        vector<string> allKeys = db.keys(tokens.size() >= 2 ? tokens[1] : "*");
        writeArray(allKeys);
    }
    else if(cmd == "TYPE") {
        if(tokens.size() < 2)
            response << "-Error: TYPE requires key\r\n";
        else
            response << "+" << db.type(tokens[1]) <<  "\r\n";
    }
    else if(cmd == "DEL" || cmd == "UNLINK") {
        if(tokens.size() < 2)
            response << "-Error: DEL/UNLINK requires key\r\n";
        else {
            int removed = 0;
            for(size_t i = 1; i < tokens.size(); i++) removed += db.del(tokens[i]);
            response << ":" << removed << "\r\n";
        }
    }
    else if(cmd == "EXISTS") {
        if(tokens.size() < 2) response << "-Error: EXISTS requires key\r\n";
        else {
            int found = 0;
            for(size_t i = 1; i < tokens.size(); i++) found += db.exists(tokens[i]);
            response << ":" << found << "\r\n";
        }
    }
    else if(cmd == "EXPIRE" || cmd == "PEXPIRE") {
        if(tokens.size() != 3)
            response << "-Error: " << cmd << " requires key and time\r\n";
        else {
            long long amount = toLong(tokens[2]);
            if(cmd == "EXPIRE") {
                if(amount > LLONG_MAX / 1000 || amount < LLONG_MIN / 1000) throw out_of_range("expire time out of range");
                amount *= 1000;
            }
            response << ":" << (db.pexpire(tokens[1] ,amount) ? 1 : 0) << "\r\n";
        }
    }
    else if(cmd == "TTL" || cmd == "PTTL") {
        if(tokens.size() != 2) response << "-Error: " << cmd << " requires key\r\n";
        else {
            long long ms = db.pttl(tokens[1]);
            if(ms >= 0 && cmd == "TTL") ms = (ms + 500) / 1000;
            response << ":" << ms << "\r\n";
        }
    }
    else if(cmd == "PERSIST") {
        if(tokens.size() != 2) response << "-Error: PERSIST requires key\r\n";
        else response << ":" << (db.persist(tokens[1]) ? 1 : 0) << "\r\n";
    }
    else if(cmd == "RENAME"){
        if(tokens.size() < 3)
            response << "-Error: RENAME requires old key and new key\r\n";
        else {
            if(db.rename(tokens[1] ,tokens[2]))
                response << "+OK\r\n";
            else
                response << "-Error: no such key\r\n";
        }
    }
    else if(cmd == "RANDOMKEY") {
        string key;
        if(db.randomKey(key)) writeBulk(key);
        else response << "$-1\r\n";
    }
    else if(cmd == "DUMP" || cmd == "SAVE" || cmd == "BGSAVE") {
        if(db.dump(db.getDumpFile())) response << (cmd == "BGSAVE" ? "+Background saving started\r\n" : "+OK\r\n");
        else response << "-Unable to dump\r\n";
    }
    else if(cmd == "LOAD") {
        if(db.load(db.getDumpFile())) response << "+OK\r\n";
        else response << "-Unable to load\r\n";
    }

    //List Commands
    else if(cmd == "LPUSH") {
        if(tokens.size() < 3)
            response << "-Error: LPUSH requires key and values\r\n";
        else {
            vector<string> values;

            for(int i=2 ;i<int(tokens.size()) ;i++) values.push_back(tokens[i]);
            string updatedSize = db.lpush(tokens[1] ,values);

            if(stoi(updatedSize) > 0) response <<":" << updatedSize << "\r\n";
            else response << "-Unable to append the values\r\n";
        }
    }
    else if(cmd == "RPUSH") {
        if(tokens.size() < 3)
            response << "-Error: RPUSH requires key and values\r\n";
        else {
            vector<string> values;

            for(int i=2 ;i<int(tokens.size()) ;i++) values.push_back(tokens[i]);
            string updatedSize = db.rpush(tokens[1] ,values);

            if(stoi(updatedSize) > 0) response << ":" << updatedSize << "\r\n";
            else response << "-Unable to append the values\r\n";
        }
    }
    else if(cmd == "LPOP" || cmd == "RPOP") {
        // LPOP key        -> the popped element (or nil)
        // LPOP key count  -> an array of up to `count` elements
        if(tokens.size() < 2 || tokens.size() > 3)
            response << "-Error: " << cmd << " requires a key and an optional count\r\n";
        else {
            const bool withCount = tokens.size() == 3;
            const int count = withCount ? toInt(tokens[2]) : 1;
            if(count < 0) { response << "-Error: value is out of range, must be positive\r\n"; return response.str(); }

            vector<string> values;
            OpStatus status = cmd == "LPOP" ? db.lpop(tokens[1] ,count ,values) : db.rpop(tokens[1] ,count ,values);
            if(status == OpStatus::WRONG_TYPE) writeStatusError(status);
            else if(status == OpStatus::NOT_FOUND) response << (withCount ? "*-1\r\n" : "$-1\r\n");
            else if(withCount) writeArray(values);
            else if(values.empty()) response << "$-1\r\n";
            else writeBulk(values[0]);
        }
    }
    else if(cmd == "LLEN") {
        if(tokens.size() < 2)
            response << "-Error: LLEN requires key\r\n";
        else {
            string updatedSize = db.llen(tokens[1]);

            if(stoi(updatedSize) >= 0) response << ":" << updatedSize << "\r\n";
            else response << "-Unable to get the length of the key\r\n";
        }
    }
    else if(cmd == "LRANGE") {
        if(tokens.size() != 4)
            response << "-Error: LRANGE requires a key and start and end indices\r\n";
        else {
            vector<string> values;
            if(db.lrange(tokens[1] ,toInt(tokens[2]) ,toInt(tokens[3]) ,values)) {
                writeArray(values);
            }
            else response << "-Error: key is not a list\r\n";
        }
    }
    else if(cmd == "LINDEX") {
        if(tokens.size() != 3) response << "-Error: LINDEX requires key and index\r\n";
        else {
            string value;
            if(db.lindex(tokens[1], toInt(tokens[2]), value)) writeBulk(value);
            else response << "$-1\r\n";
        }
    }
    else if(cmd == "LSET") {
        if(tokens.size() != 4) response << "-Error: LSET requires key, index and value\r\n";
        else if(db.lset(tokens[1], toInt(tokens[2]), tokens[3])) response << "+OK\r\n";
        else response << "-Error: no such list element\r\n";
    }
    else if(cmd == "LREM") {
        if(tokens.size() != 4) response << "-Error: LREM requires key, count and value\r\n";
        else response << ":" << db.lrem(tokens[1], toInt(tokens[2]), tokens[3]) << "\r\n";
    }
    else if(cmd == "LTRIM") {
        if(tokens.size() != 4) response << "-Error: LTRIM requires key, start and stop\r\n";
        else if(db.ltrim(tokens[1], toInt(tokens[2]), toInt(tokens[3]))) response << "+OK\r\n";
        else response << "-Error: key is not a list\r\n";
    }
    else if(cmd == "LINSERT") {
        string where = tokens.size() == 5 ? toUpper(tokens[2]) : "";
        if(where != "BEFORE" && where != "AFTER") response << "-Error: LINSERT requires key, BEFORE|AFTER, pivot and value\r\n";
        else {
            long long length;
            OpStatus status = db.linsert(tokens[1], where == "BEFORE", tokens[3], tokens[4], length);
            if(status == OpStatus::WRONG_TYPE) writeStatusError(status);
            else response << ":" << length << "\r\n";
        }
    }
    // Hash Commands
    else if(cmd == "HSET" || cmd == "HMSET") {
        if(tokens.size() < 4 || tokens.size() % 2 != 0)
            response << "-Error: " << cmd << " requires key and field/value pairs\r\n";
        else {
            vector<pair<string, string>> fieldValues;
            for(size_t i = 2; i < tokens.size(); i += 2) fieldValues.emplace_back(tokens[i], tokens[i + 1]);
            int added = 0;
            if(!db.hset(tokens[1], fieldValues, added)) response << "-Error: key is not a hash\r\n";
            else if(cmd == "HMSET") response << "+OK\r\n";
            else response << ":" << added << "\r\n";
        }
    }
    else if(cmd == "HSETNX") {
        if(tokens.size() != 4) response << "-Error: HSETNX requires key, field and value\r\n";
        else {
            bool set = false;
            OpStatus status = db.hsetnx(tokens[1], tokens[2], tokens[3], set);
            if(status == OpStatus::OK) response << ":" << (set ? 1 : 0) << "\r\n";
            else writeStatusError(status);
        }
    }
    else if(cmd == "HGET") {
        if(tokens.size() != 3) response << "-Error: HGET requires key and field\r\n";
        else {
            string value;
            if(db.hget(tokens[1], tokens[2], value)) writeBulk(value);
            else response << "$-1\r\n";
        }
    }
    else if(cmd == "HMGET") {
        if(tokens.size() < 3) response << "-Error: HMGET requires key and fields\r\n";
        else {
            response << "*" << tokens.size() - 2 << "\r\n";
            for(size_t i = 2; i < tokens.size(); ++i) {
                string value;
                if(db.hget(tokens[1], tokens[i], value)) writeBulk(value);
                else response << "$-1\r\n";
            }
        }
    }
    else if(cmd == "HDEL") {
        if(tokens.size() < 3) response << "-Error: HDEL requires key and fields\r\n";
        else response << ":" << db.hdel(tokens[1], argsFrom(2)) << "\r\n";
    }
    else if(cmd == "HEXISTS") {
        if(tokens.size() != 3) response << "-Error: HEXISTS requires key and field\r\n";
        else {
            bool exists = false;
            if(db.hexists(tokens[1], tokens[2], exists)) response << ":" << (exists ? 1 : 0) << "\r\n";
            else response << "-Error: key is not a hash\r\n";
        }
    }
    else if(cmd == "HLEN") {
        if(tokens.size() != 2) response << "-Error: HLEN requires key\r\n";
        else response << ":" << db.hlen(tokens[1]) << "\r\n";
    }
    else if(cmd == "HGETALL" || cmd == "HKEYS" || cmd == "HVALS") {
        if(tokens.size() != 2) response << "-Error: " << cmd << " requires key\r\n";
        else {
            vector<pair<string, string>> fieldValues;
            if(!db.hgetall(tokens[1], fieldValues)) response << "-Error: key is not a hash\r\n";
            else {
                vector<string> values;
                for(const auto &fieldValue : fieldValues) {
                    if(cmd == "HGETALL") { values.push_back(fieldValue.first); values.push_back(fieldValue.second); }
                    else values.push_back(cmd == "HKEYS" ? fieldValue.first : fieldValue.second);
                }
                writeArray(values);
            }
        }
    }
    else if(cmd == "HINCRBY") {
        if(tokens.size() != 4) response << "-Error: HINCRBY requires key, field and increment\r\n";
        else {
            long long result;
            OpStatus status = db.hincrBy(tokens[1], tokens[2], toLong(tokens[3]), result);
            if(status == OpStatus::OK) response << ":" << result << "\r\n";
            else writeStatusError(status);
        }
    }
    // Set Commands
    else if(cmd == "SADD" || cmd == "SREM") {
        if(tokens.size() < 3) response << "-Error: " << cmd << " requires key and members\r\n";
        else {
            int changed = 0;
            OpStatus status = cmd == "SADD" ? db.sadd(tokens[1], argsFrom(2), changed) : db.srem(tokens[1], argsFrom(2), changed);
            if(status == OpStatus::OK) response << ":" << changed << "\r\n";
            else writeStatusError(status);
        }
    }
    else if(cmd == "SISMEMBER") {
        if(tokens.size() != 3) response << "-Error: SISMEMBER requires key and member\r\n";
        else {
            bool isMember = false;
            OpStatus status = db.sismember(tokens[1], tokens[2], isMember);
            if(status == OpStatus::OK) response << ":" << (isMember ? 1 : 0) << "\r\n";
            else writeStatusError(status);
        }
    }
    else if(cmd == "SMEMBERS") {
        if(tokens.size() != 2) response << "-Error: SMEMBERS requires key\r\n";
        else {
            vector<string> members;
            OpStatus status = db.smembers(tokens[1], members);
            if(status == OpStatus::OK) writeArray(members);
            else writeStatusError(status);
        }
    }
    else if(cmd == "SCARD") {
        if(tokens.size() != 2) response << "-Error: SCARD requires key\r\n";
        else {
            size_t count = 0;
            OpStatus status = db.scard(tokens[1], count);
            if(status == OpStatus::OK) response << ":" << count << "\r\n";
            else writeStatusError(status);
        }
    }
    else if(cmd == "SPOP") {
        if(tokens.size() < 2 || tokens.size() > 3) response << "-Error: SPOP requires a key and an optional count\r\n";
        else {
            const bool withCount = tokens.size() == 3;
            const int count = withCount ? toInt(tokens[2]) : 1;
            if(count < 0) { response << "-Error: value is out of range, must be positive\r\n"; return response.str(); }

            vector<string> members;
            OpStatus status = db.spop(tokens[1], count, members);
            if(status == OpStatus::WRONG_TYPE) writeStatusError(status);
            else if(withCount) writeArray(members);
            else if(members.empty()) response << "$-1\r\n";
            else writeBulk(members[0]);
        }
    }
    else if(cmd == "SRANDMEMBER") {
        if(tokens.size() != 2) response << "-Error: SRANDMEMBER requires key\r\n";
        else {
            string member;
            OpStatus status = db.srandmember(tokens[1], member);
            if(status == OpStatus::OK) writeBulk(member);
            else if(status == OpStatus::NOT_FOUND) response << "$-1\r\n";
            else writeStatusError(status);
        }
    }
    else if(cmd == "SINTER" || cmd == "SUNION" || cmd == "SDIFF") {
        if(tokens.size() < 2) response << "-Error: " << cmd << " requires at least one key\r\n";
        else {
            vector<string> result;
            const char op = cmd == "SINTER" ? 'I' : cmd == "SUNION" ? 'U' : 'D';
            OpStatus status = db.setOperation(op, argsFrom(1), result);
            if(status == OpStatus::OK) writeArray(result);
            else writeStatusError(status);
        }
    }
    else
        response << "-Error : Unknown command\r\n";

    return response.str();
}

string RedisCommandHandler::execute(const vector<string> &tokens) {
    if(tokens.empty()) return "-Error: Empty Command\r\n";

    string response;
    try {
        response = runCommand(tokens);
    }
    catch(const invalid_argument &) {
        // Thrown by toInt / toLong / toLongDouble when an argument isn't a number
        response = "-Error: value is not an integer or out of range\r\n";
    }
    catch(const out_of_range &) {
        response = "-Error: value is not an integer or out of range\r\n";
    }
    catch(const exception &e) {
        response = string("-Error: ") + e.what() + "\r\n";
    }

    // Bookkeeping for INFO
    ServerStats &stats = serverStats();
    stats.commandsProcessed++;
    string cmd = toUpper(tokens[0]);
    if(find(SUPPORTED_COMMANDS.begin() ,SUPPORTED_COMMANDS.end() ,cmd) != SUPPORTED_COMMANDS.end()) {
        transform(cmd.begin() ,cmd.end() ,cmd.begin() ,::tolower);
        lock_guard<mutex> lock(stats.commandMutex);
        stats.commandCalls[cmd]++;
        cmd = toUpper(cmd);
    }
    if(WRITE_COMMANDS.count(cmd) && !response.empty() && response[0] != '-')
        RedisDatabase::getInstance().noteWrite();

    return response;
}
