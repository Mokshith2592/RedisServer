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

    cout << commandLine << "\n";
    for(auto &t : tokens) cout << t << "\n";

    string cmd = tokens[0];
    transform(cmd.begin() ,cmd.end() ,cmd.begin() ,::toupper);

    ostringstream response;

    RedisDatabase &db = RedisDatabase::getInstance();

    // Check commands
    if(cmd == "PING") {
        response << "+PONG\r\n";
    }
    else if(cmd == "ECHO") {

    }
    else {
        response << "-Error : Unknown command\r\n";
    }

    return response.str();
} 