#include "RedisCommandHandler.h"
#include "RedisDatabase.h"
#include "TestHelpers.h"

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

    tests.expect(handler.processCommand("SET") == "-Error: SET reuires key and value\r\n", "validates SET arguments");
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

    return tests.result();
}
