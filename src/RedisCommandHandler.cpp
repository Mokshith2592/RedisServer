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
                response << "*" << values.size() << "\r\n";

                for(const auto &key : values) {
                    response << "$" << key.size() << "\r\n" << key << "\r\n";
                }
            }
        }
    }
    else
        response << "-Error : Unknown command\r\n";

    return response.str();
}
