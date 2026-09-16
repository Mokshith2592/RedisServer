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

    int added = 0;
    tests.expect(db.hset("user:1", {{"name", "Alice"}, {"role", "admin"}}, added) && added == 2,
                 "HSET creates hash fields");
    tests.expect(db.hget("user:1", "name", value) && value == "Alice", "HGET returns a hash field");
    tests.expect(db.hset("user:1", {{"name", "Alicia"}}, added) && added == 0,
                 "HSET updates an existing field");
    bool exists = false;
    tests.expect(db.hexists("user:1", "role", exists) && exists, "HEXISTS finds an existing field");
    tests.expect(db.hlen("user:1") == 2, "HLEN returns field count");
    std::vector<std::pair<std::string, std::string>> fields;
    tests.expect(db.hgetall("user:1", fields) && fields.size() == 2, "HGETALL returns all fields");
    tests.expect(db.hdel("user:1", {"role", "missing"}) == 1, "HDEL removes matching fields");

    tests.expect(db.rpush("numbers", {"one", "two", "two", "three"}) == "4", "RPUSH creates a list");
    tests.expect(db.lindex("numbers", -1, value) && value == "three", "LINDEX supports negative indices");
    tests.expect(db.lset("numbers", 0, "zero"), "LSET replaces a list item");
    tests.expect(db.lrem("numbers", 1, "two") == 1, "LREM obeys positive count");
    tests.expect(db.ltrim("numbers", 0, 1), "LTRIM keeps the requested range");
    std::vector<std::string> listValues;
    tests.expect(db.lrange("numbers", 0, -1, listValues) &&
                 listValues == std::vector<std::string>({"zero", "two"}), "LRANGE supports negative end index");

    return tests.result();
}
