#include <algorithm>
#include <string>
#include <vector>

#include "RedisDatabase.h"
#include "TestHelpers.h"

int main() {
    TestSuite tests("DatabaseTests");
    RedisDatabase& db = RedisDatabase::getInstance();
    std::string value;

    db.flushAll();
    tests.expect(db.set("name", "Alice"), "SET creates a new key");
    tests.expect(db.get("name", value) && value == "Alice", "GET returns the stored value");
    tests.expect(!db.set("name", "Bob"), "SET rejects duplicate keys");
    tests.expect(db.get("name", value) && value == "Alice", "duplicate SET preserves the value");
    tests.expect(db.type("name") == "string", "TYPE reports string keys");
    tests.expect(db.type("missing") == "none", "TYPE reports missing keys");

    tests.expect(db.set("city", "Berlin"), "creates a second key");
    std::vector<std::string> keys = db.keys();
    std::sort(keys.begin(), keys.end());
    tests.expect(keys == std::vector<std::string>({"city", "name"}), "KEYS returns all stored keys");

    tests.expect(db.rename("name", "user"), "RENAME moves an existing key");
    tests.expect(!db.get("name", value), "RENAME removes the source key");
    tests.expect(db.get("user", value) && value == "Alice", "RENAME preserves the source value");
    tests.expect(db.rename("user", "city"), "RENAME replaces an existing destination");
    tests.expect(db.get("city", value) && value == "Alice", "RENAME overwrites the destination value");
    tests.expect(!db.rename("missing", "other"), "RENAME rejects a missing source key");
    tests.expect(db.rename("city", "city"), "RENAME accepts the same existing key");

    tests.expect(db.del("city"), "DEL removes an existing key");
    tests.expect(!db.del("city"), "DEL reports a missing key");
    tests.expect(db.flushAll(), "FLUSHALL succeeds");
    tests.expect(db.keys().empty(), "FLUSHALL removes every key");

    return tests.result();
}
