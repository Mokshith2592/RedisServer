#include "RedisCommandHandler.h"
#include "RedisDatabase.h"
#include "TestHelpers.h"

#include <string>
#include <vector>

int main() {
    TestSuite tests("CommandHandlerTests");
    RedisDatabase::getInstance().flushAll();
    RedisCommandHandler handler;

    tests.expect(handler.processCommand("") == "-Error: Empty Command\r\n", "rejects empty commands");
    tests.expect(handler.processCommand("UNKNOWN") == "-Error : Unknown command\r\n", "rejects unknown commands");
    tests.expect(handler.processCommand("PING") == "+PONG\r\n", "handles PING");
    tests.expect(handler.processCommand("ECHO") == "-Error: Echo requires a message\r\n", "validates ECHO arguments");
    tests.expect(handler.processCommand("*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n") == "+hello\r\n",
                 "handles RESP-formatted commands");

    tests.expect(handler.processCommand("SET") == "-Error: SET requires key and value\r\n", "validates SET arguments");
    tests.expect(handler.processCommand("*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nAlice\r\n") == "+OK\r\n",
                 "sets a value from a RESP command");
    tests.expect(handler.processCommand("GET name") == "$5\r\nAlice\r\n", "gets a stored value");
    tests.expect(handler.processCommand("GET missing") == "$-1\r\n", "reports a missing value");
    tests.expect(handler.processCommand("KEYS") == "*1\r\n$4\r\nname\r\n", "serializes KEYS response as RESP");

    tests.expect(handler.processCommand("RENAME name user") == "+OK\r\n", "renames a key");
    tests.expect(handler.processCommand("RENAME missing user") == "-Error: no such key\r\n",
                 "reports a missing RENAME source");
    tests.expect(handler.processCommand("DEL user") == ":1\r\n", "deletes a key");
    tests.expect(handler.processCommand("DEL user") == ":0\r\n", "reports a missing key on DEL");

    tests.expect(handler.processCommand("HSET user name Alice role admin") == ":2\r\n", "sets multiple hash fields");
    tests.expect(handler.processCommand("HGET user name") == "$5\r\nAlice\r\n", "gets a hash field");
    tests.expect(handler.processCommand("HMGET user role missing") == "*2\r\n$5\r\nadmin\r\n$-1\r\n",
                 "gets multiple hash fields");
    tests.expect(handler.processCommand("HLEN user") == ":2\r\n", "counts hash fields");
    tests.expect(handler.processCommand("HGETALL user") == "*4\r\n$4\r\nname\r\n$5\r\nAlice\r\n$4\r\nrole\r\n$5\r\nadmin\r\n",
                 "serializes hash fields as RESP");

    tests.expect(handler.processCommand("RPUSH queue first second third") == ":3\r\n", "pushes list values");
    tests.expect(handler.processCommand("LINDEX queue -1") == "$5\r\nthird\r\n", "indexes a list value");
    tests.expect(handler.processCommand("LREM queue 1 second") == ":1\r\n", "removes list values");
    tests.expect(handler.processCommand("LTRIM queue 0 0") == "+OK\r\n", "trims a list");
    tests.expect(handler.processCommand("LPOP queue") == "$5\r\nfirst\r\n", "LPOP returns the element");
    tests.expect(handler.processCommand("LPOP queue") == "$-1\r\n", "LPOP on a missing list returns nil");
    tests.expect(handler.processCommand("RPUSH q a b c") == ":3\r\n", "pushes another list");
    tests.expect(handler.processCommand("RPOP q 2") == "*2\r\n$1\r\nc\r\n$1\r\nb\r\n", "RPOP with a count");
    tests.expect(handler.processCommand("LINSERT q AFTER a z") == ":2\r\n", "LINSERT");

    // Robustness: bad numbers must produce an error, not crash the server
    tests.expect(handler.processCommand("LINDEX queue abc") == "-Error: value is not an integer or out of range\r\n",
                 "rejects non-numeric indices");
    tests.expect(handler.processCommand("LRANGE queue 0 99999999999") == "-Error: value is not an integer or out of range\r\n",
                 "rejects out of range indices");
    tests.expect(handler.processCommand("*2\r\n$4\r\nECHO\r\n$10\r\nhi\r\n") == "-Error: Protocol error\r\n",
                 "rejects truncated RESP");

    // Inline commands with quotes
    tests.expect(handler.processCommand("SET greeting \"hello world\"") == "+OK\r\n", "inline quoted values");
    tests.expect(handler.processCommand("GET greeting") == "$11\r\nhello world\r\n", "quoted values keep spaces");
    tests.expect(handler.processCommand("SET bad \"unterminated") == "-Error: Protocol error\r\n", "rejects unbalanced quotes");

    // SET options and string commands
    tests.expect(handler.processCommand("SET greeting x NX") == "$-1\r\n", "SET NX returns nil when the key exists");
    tests.expect(handler.processCommand("SET session abc EX 100") == "+OK\r\n", "SET EX");
    tests.expect(handler.processCommand("TTL session") == ":100\r\n", "TTL reports seconds left");
    tests.expect(handler.processCommand("SET session abc EX 0") == "-Error: invalid expire time in 'set' command\r\n",
                 "SET rejects a zero TTL");
    tests.expect(handler.processCommand("SET session abc FOO") == "-Error: syntax error\r\n", "SET rejects unknown options");
    tests.expect(handler.processCommand("PERSIST session") == ":1\r\n", "PERSIST");
    tests.expect(handler.processCommand("TTL session") == ":-1\r\n", "TTL without expiry");
    tests.expect(handler.processCommand("TTL nothing") == ":-2\r\n", "TTL of a missing key");
    tests.expect(handler.processCommand("EXPIRE nothing 10") == ":0\r\n", "EXPIRE replies for missing keys");
    tests.expect(handler.processCommand("INCR visits") == ":1\r\n", "INCR creates a counter");
    tests.expect(handler.processCommand("INCRBY visits 9") == ":10\r\n", "INCRBY");
    tests.expect(handler.processCommand("DECR visits") == ":9\r\n", "DECR");
    tests.expect(handler.processCommand("INCR greeting") == "-Error: value is not an integer or out of range\r\n",
                 "INCR rejects non-numeric values");
    tests.expect(handler.processCommand("INCRBYFLOAT price 1.5") == "$3\r\n1.5\r\n", "INCRBYFLOAT");
    tests.expect(handler.processCommand("INCRBYFLOAT price 1.5abc") == "-Error: value is not an integer or out of range\r\n",
                 "INCRBYFLOAT rejects junk");
    tests.expect(handler.processCommand("MSET a 1 b 2") == "+OK\r\n", "MSET");
    tests.expect(handler.processCommand("MGET a missing b") == "*3\r\n$1\r\n1\r\n$-1\r\n$1\r\n2\r\n", "MGET");
    tests.expect(handler.processCommand("APPEND a 23") == ":3\r\n", "APPEND");
    tests.expect(handler.processCommand("STRLEN a") == ":3\r\n", "STRLEN");
    tests.expect(handler.processCommand("GETRANGE a 0 1") == "$2\r\n12\r\n", "GETRANGE");
    tests.expect(handler.processCommand("GETSET a new") == "$3\r\n123\r\n", "GETSET returns the old value");
    tests.expect(handler.processCommand("EXISTS a b missing a") == ":3\r\n", "EXISTS counts every key");
    tests.expect(handler.processCommand("DEL a b missing") == ":2\r\n", "DEL removes several keys");

    // Sets and wrong type errors
    tests.expect(handler.processCommand("SADD langs cpp go rust") == ":3\r\n", "SADD");
    tests.expect(handler.processCommand("SCARD langs") == ":3\r\n", "SCARD");
    tests.expect(handler.processCommand("SMEMBERS langs") == "*3\r\n$3\r\ncpp\r\n$2\r\ngo\r\n$4\r\nrust\r\n", "SMEMBERS");
    tests.expect(handler.processCommand("SISMEMBER langs go") == ":1\r\n", "SISMEMBER");
    tests.expect(handler.processCommand("TYPE langs") == "+set\r\n", "TYPE reports sets");
    tests.expect(handler.processCommand("SADD greeting x") == "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n",
                 "WRONGTYPE for sets on strings");
    tests.expect(handler.processCommand("HINCRBY user visits 3") == ":3\r\n", "HINCRBY");
    tests.expect(handler.processCommand("HSETNX user name Bob") == ":0\r\n", "HSETNX keeps existing fields");

    // Server commands
    tests.expect(handler.processCommand("PING hi") == "$2\r\nhi\r\n", "PING with a message");
    tests.expect(handler.processCommand("SELECT 0") == "+OK\r\n", "SELECT 0");
    tests.expect(handler.processCommand("SELECT 1") == "-Error: DB index is out of range\r\n", "only db 0 exists");
    tests.expect(handler.processCommand("KEYS lang*") == "*1\r\n$5\r\nlangs\r\n", "KEYS with a pattern");
    tests.expect(handler.processCommand("INFO keyspace").find("db0:keys=") != std::string::npos, "INFO keyspace");
    tests.expect(handler.processCommand("INFO commandstats").find("cmdstat_set:calls=") != std::string::npos, "INFO commandstats");
    tests.expect(handler.processCommand("INFO persistence").find("changes_since_last_save:") != std::string::npos,
                 "INFO persistence");

    // Incremental parsing (used for pipelining in the server)
    std::string pipeline = "*1\r\n$4\r\nPING\r\nPING\r\n*2\r\n$3\r\nGET";
    std::vector<std::string> tokens;
    size_t consumed = 0;
    tests.expect(RedisCommandHandler::parseNext(pipeline, 0, tokens, consumed) == ParseStatus::COMPLETE && consumed == 14,
                 "parses the first pipelined command");
    tests.expect(RedisCommandHandler::parseNext(pipeline, 14, tokens, consumed) == ParseStatus::COMPLETE && consumed == 6,
                 "parses an inline command after it");
    tests.expect(RedisCommandHandler::parseNext(pipeline, 20, tokens, consumed) == ParseStatus::INCOMPLETE,
                 "waits for the rest of a partial command");

    return tests.result();
}
