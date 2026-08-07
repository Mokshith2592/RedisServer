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

    return tests.result();
}
