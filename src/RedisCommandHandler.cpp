#include <bits/stdc++.h>
using namespace std;

#include "../include/RedisCommandHandler.h"
#include "../include/RedisDatabase.h"

// RESP(Redis Serialization Protocol) parser: 
// *2\r\n$4\r\n\PING\r\n$4\r\TEST\r\n
// *2 -> array has 2 elements 
// $4 -> next string has 4 characters 
// PING
// TEST

vector<string> parseRespCommand(const string &input) {
    vector<string> tokens;

    if(input.empty()) return tokens;

    //If it doesnt start with '*' ,fallback splitting by whitespaces.
    if(input[0] != '*') {
        istringstream iss(input);

        string token;
        while(iss >> token) {
            tokens.push_back(token);
        }

        return tokens;
    }

    size_t pos = 0;
    // Expect '*' followed by number of elements
    if(input[pos] != '*') return tokens;
    pos++;

    // crlf = Carriage Return (\r) ,Line Feed (\n)
    size_t crlf = input.find("\r\n" ,pos);
    if(crlf == string::npos) return tokens;

    int numElements = stoi(input.substr(pos ,crlf - pos));
    pos = crlf + 2;

    for(int i=0 ;i<numElements ;i++) {
        if(pos >= input.size() || input[pos] != '$') break;
        pos++;

        crlf = input.find("\r\n" ,pos);
        if(crlf == string::npos) break;

        int len = stoi(input.substr(pos ,crlf-pos));

        pos = crlf+2;
        if(pos + len > input.size()) break;

        string token = input.substr(pos ,len);
        tokens.push_back(token);

        pos += (len + 2);
    }

    return tokens;
}

RedisCommandHandler::RedisCommandHandler() {}

string RedisCommandHandler::processCommand(const string &commandLine) {
    // Use RESP Parser
    vector<string> tokens = parseRespCommand(commandLine);
    if(tokens.empty()) return "-Error: Empty Command\r\n";

    string cmd = tokens[0];
    transform(cmd.begin() ,cmd.end() ,cmd.begin() ,::toupper);

    ostringstream response;

    RedisDatabase &db = RedisDatabase::getInstance();
    auto writeArray = [&response](const vector<string> &values) {
        response << "*" << values.size() << "\r\n";
        for(const string &value : values)
            response << "$" << value.size() << "\r\n" << value << "\r\n";
    };

    // Check commands

    //Common commands
    if(cmd == "PING") {
        response << "+PONG\r\n";
    }
    else if(cmd == "ECHO") {
        if(tokens.size() < 2) 
            response << "-Error: Echo requires a message\r\n";
        else 
            response << "+" << tokens[1] << "\r\n";
    }
    else if(cmd == "FLUSHALL") {
        db.flushAll();
        response << "+OK\r\n";
    }

    //Key-Value Operations
    else if(cmd == "SET") {
        if(tokens.size() < 3) 
            response << "-Error: SET reuires key and value\r\n";
        else {
            if(db.set(tokens[1] ,tokens[2]))
                response << "+OK\r\n";
            else 
                response << "$-1\r\n";
        }
    }
    else if(cmd == "GET") {
        if(tokens.size() < 2) 
            response << "-Error: GET reuires key\r\n";
        else {
            string value;
            if(db.get(tokens[1] ,value))
                response << "$" << value.size() << "\r\n" << value << "\r\n";
            else 
                response << "$-1\r\n";
        }
    }
    else if(cmd == "KEYS") {
        vector<string> allKeys = db.keys();
        response << "*" << allKeys.size() << "\r\n";

        for(const auto &key : allKeys) {
            response << "$" << key.size() << "\r\n" << key << "\r\n";
        }
    }
    else if(cmd == "TYPE") {
        if(tokens.size() < 2) 
            response << "-Error: TYPE reuires key\r\n";
        else 
            response << "+" << db.type(tokens[1]) <<  "\r\n";
    }
    else if(cmd == "DEL" || cmd == "UNLINK") {
        if(tokens.size() < 2) 
            response << "-Error: DEL/UNLINK reuires key\r\n";
        else {
            bool res = db.del(tokens[1]);
            response << ":" << (res ? 1 : 0) << "\r\n";
        }
    } 
    else if(cmd == "EXPIRE") {
        if(tokens.size() < 3) 
            response << "-Error: EXPIRE reuires key and time(seconds)\r\n";
        else {
            if(db.expire(tokens[1] ,stoi(tokens[2])))
                response << "+OK\r\n";
        }
    }
    else if(cmd == "RENAME"){
        if(tokens.size() < 3) 
            response << "-Error: RENAME reuires old key and new key\r\n";
        else {
            if(db.rename(tokens[1] ,tokens[2]))
                response << "+OK\r\n";
            else
                response << "-Error: no such key\r\n";
        }
    }
    else if(cmd == "DUMP") {
        if(db.dump("dump.my_rdb")) response << "+OK\r\n";
        else response << "-Unable to dump\r\n";
    }
    else if(cmd == "LOAD") {
        if(db.load("dump.my_rdb")) response << "+OK\r\n";
        else response << "-Unable to load\r\n";
    }

    //List Commands
    else if(cmd == "LPUSH") {
        if(tokens.size() < 3) 
            response << "-Error: LPUSH reuires key and values\r\n";
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
            response << "-Error: RPUSH reuires key and values\r\n";
        else {
            vector<string> values;

            for(int i=2 ;i<int(tokens.size()) ;i++) values.push_back(tokens[i]);
            string updatedSize = db.rpush(tokens[1] ,values);
            
            if(stoi(updatedSize) > 0) response << ":" << updatedSize << "\r\n";
            else response << "-Unable to append the values\r\n";
        } 
    }
    else if(cmd == "LPOP") {
        if(tokens.size() < 2) 
            response << "-Error: LPOP reuires a key\r\n";
        else {
            string updatedSize = db.lpop(tokens[1]);
            
            if(stoi(updatedSize) >= 0) response << ":" << updatedSize << "\r\n";
            else response << "-Unable to pop the value from front\r\n";
        }
    }
    else if(cmd == "RPOP") {
        if(tokens.size() < 2) 
            response << "-Error: RPUSH reuires a key\r\n";
        else {
            string updatedSize = db.rpop(tokens[1]);
            
            if(stoi(updatedSize) >= 0) response << ":" << updatedSize << "\r\n";
            else response << "-Unable to pop the value from back\r\n";
        }
        
    }
    else if(cmd == "LLEN") {
        if(tokens.size() < 2) 
            response << "-Error: LLEN reuires key\r\n";
        else {
            string updatedSize = db.llen(tokens[1]);
            
            if(stoi(updatedSize) >= 0) response << ":" << updatedSize << "\r\n";
            else response << "-Unable to get the length of the key\r\n";
        }
    }
    else if(cmd == "LRANGE") {
        if(tokens.size() != 4) 
            response << "-Error: LRANGE reuires a key and start and end indices\r\n";
        else {
            vector<string> values;
            if(db.lrange(tokens[1] ,stoi(tokens[2]) ,stoi(tokens[3]) ,values)) {
                writeArray(values);
            }
            else response << "-Error: key is not a list\r\n";
        }
    }
    else if(cmd == "LINDEX") {
        if(tokens.size() != 3) response << "-Error: LINDEX requires key and index\r\n";
        else {
            string value;
            if(db.lindex(tokens[1], stoi(tokens[2]), value)) response << "$" << value.size() << "\r\n" << value << "\r\n";
            else response << "$-1\r\n";
        }
    }
    else if(cmd == "LSET") {
        if(tokens.size() != 4) response << "-Error: LSET requires key, index and value\r\n";
        else if(db.lset(tokens[1], stoi(tokens[2]), tokens[3])) response << "+OK\r\n";
        else response << "-Error: no such list element\r\n";
    }
    else if(cmd == "LREM") {
        if(tokens.size() != 4) response << "-Error: LREM requires key, count and value\r\n";
        else response << ":" << db.lrem(tokens[1], stoi(tokens[2]), tokens[3]) << "\r\n";
    }
    else if(cmd == "LTRIM") {
        if(tokens.size() != 4) response << "-Error: LTRIM requires key, start and stop\r\n";
        else if(db.ltrim(tokens[1], stoi(tokens[2]), stoi(tokens[3]))) response << "+OK\r\n";
        else response << "-Error: key is not a list\r\n";
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
    else if(cmd == "HGET") {
        if(tokens.size() != 3) response << "-Error: HGET requires key and field\r\n";
        else {
            string value;
            if(db.hget(tokens[1], tokens[2], value)) response << "$" << value.size() << "\r\n" << value << "\r\n";
            else response << "$-1\r\n";
        }
    }
    else if(cmd == "HMGET") {
        if(tokens.size() < 3) response << "-Error: HMGET requires key and fields\r\n";
        else {
            response << "*" << tokens.size() - 2 << "\r\n";
            for(size_t i = 2; i < tokens.size(); ++i) {
                string value;
                if(db.hget(tokens[1], tokens[i], value)) response << "$" << value.size() << "\r\n" << value << "\r\n";
                else response << "$-1\r\n";
            }
        }
    }
    else if(cmd == "HDEL") {
        if(tokens.size() < 3) response << "-Error: HDEL requires key and fields\r\n";
        else response << ":" << db.hdel(tokens[1], vector<string>(tokens.begin() + 2, tokens.end())) << "\r\n";
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
    else
        response << "-Error : Unknown command\r\n";

    return response.str();
}
