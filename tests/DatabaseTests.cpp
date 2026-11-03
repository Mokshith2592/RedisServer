#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
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
    tests.expect(!db.set("name", "Bob", -1, SetCondition::NX), "SET NX keeps an existing key");
    tests.expect(db.get("name", value) && value == "Alice", "SET NX preserves the value");
    tests.expect(db.set("name", "Bob") && db.get("name", value) && value == "Bob", "SET overwrites an existing key");
    tests.expect(db.set("name", "Alice"), "SET restores the original value");
    tests.expect(!db.set("ghost", "x", -1, SetCondition::XX), "SET XX needs an existing key");
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

    std::vector<std::string> popped;
    tests.expect(db.lpop("numbers", 1, popped) == OpStatus::OK && popped == std::vector<std::string>({"zero"}),
                 "LPOP returns the popped element");
    popped.clear();
    tests.expect(db.rpop("numbers", 5, popped) == OpStatus::OK && popped == std::vector<std::string>({"two"}),
                 "RPOP with a count stops at the end of the list");
    tests.expect(db.type("numbers") == "none", "popping the last element deletes the list");
    tests.expect(db.lpop("numbers", 1, popped) == OpStatus::NOT_FOUND, "LPOP reports a missing list");
    long long length = 0;
    db.rpush("letters", {"a", "c"});
    tests.expect(db.linsert("letters", true, "c", "b", length) == OpStatus::OK && length == 3, "LINSERT inserts before a pivot");
    tests.expect(db.linsert("letters", true, "zz", "b", length) == OpStatus::OK && length == -1, "LINSERT reports a missing pivot");

    // Strings
    long long counter = 0;
    tests.expect(db.incrBy("hits", 5, counter) == OpStatus::OK && counter == 5, "INCRBY creates a counter");
    tests.expect(db.incrBy("hits", -2, counter) == OpStatus::OK && counter == 3, "INCRBY accepts negative deltas");
    db.set("word", "hello");
    tests.expect(db.incrBy("word", 1, counter) == OpStatus::NOT_INTEGER, "INCRBY rejects non-integers");
    db.set("big", "9223372036854775807");
    tests.expect(db.incrBy("big", 1, counter) == OpStatus::OVERFLOW, "INCRBY detects overflow");
    tests.expect(db.incrBy("letters", 1, counter) == OpStatus::WRONG_TYPE, "INCRBY rejects lists");
    std::string floatResult;
    tests.expect(db.incrByFloat("price", 10.5L, floatResult) == OpStatus::OK && floatResult == "10.5", "INCRBYFLOAT works");
    size_t newLength = 0;
    tests.expect(db.append("word", " world", newLength) == OpStatus::OK && newLength == 11, "APPEND returns the new length");
    tests.expect(db.getRange("word", -5, -1, value) == OpStatus::OK && value == "world", "GETRANGE supports negative indices");
    tests.expect(db.getDel("word", value) == OpStatus::OK && value == "hello world" && !db.exists("word"), "GETDEL removes the key");

    // Expiry
    db.set("temp", "1");
    tests.expect(db.pttl("temp") == -1, "PTTL is -1 without an expiry");
    tests.expect(db.pttl("nothing") == -2, "PTTL is -2 for missing keys");
    tests.expect(db.expire("temp", 100) && db.pttl("temp") > 99000, "EXPIRE sets a TTL");
    tests.expect(db.persist("temp") && db.pttl("temp") == -1, "PERSIST removes the TTL");
    tests.expect(!db.expire("nothing", 10), "EXPIRE reports a missing key");
    db.pexpire("temp", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    tests.expect(!db.exists("temp"), "keys expire lazily");
    db.set("temp2", "1", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    tests.expect(db.purgeExpired(100) == 1, "active expiry purges expired keys");
    tests.expect(db.set("temp", "again"), "an expired key can be SET again");

    // KEYS patterns
    db.flushAll();
    db.set("user:1", "a"); db.set("user:2", "b"); db.set("user:10", "c"); db.set("order:1", "d");
    tests.expect(db.keys("user:*") == std::vector<std::string>({"user:1", "user:10", "user:2"}), "KEYS supports *");
    tests.expect(db.keys("user:?") == std::vector<std::string>({"user:1", "user:2"}), "KEYS supports ?");
    tests.expect(db.keys("[ou]*:1") == std::vector<std::string>({"order:1", "user:1"}), "KEYS supports [..]");
    tests.expect(RedisDatabase::globMatch("h[^e]llo", "hallo") && !RedisDatabase::globMatch("h[^e]llo", "hello"), "glob negation");
    tests.expect(RedisDatabase::globMatch("a\\*b", "a*b") && !RedisDatabase::globMatch("a\\*b", "axb"), "glob escapes");
    tests.expect(db.dbSize() == 4, "DBSIZE counts keys");

    // Sets
    int changed = 0;
    tests.expect(db.sadd("tags", {"c++", "redis", "c++"}, changed) == OpStatus::OK && changed == 2, "SADD ignores duplicates");
    db.sadd("other", {"redis", "go"}, changed);
    bool member = false;
    tests.expect(db.sismember("tags", "redis", member) == OpStatus::OK && member, "SISMEMBER finds a member");
    std::vector<std::string> setResult;
    tests.expect(db.setOperation('I', {"tags", "other"}, setResult) == OpStatus::OK &&
                 setResult == std::vector<std::string>({"redis"}), "SINTER");
    setResult.clear();
    tests.expect(db.setOperation('U', {"tags", "other"}, setResult) == OpStatus::OK &&
                 setResult == std::vector<std::string>({"c++", "go", "redis"}), "SUNION");
    setResult.clear();
    tests.expect(db.setOperation('D', {"tags", "other"}, setResult) == OpStatus::OK &&
                 setResult == std::vector<std::string>({"c++"}), "SDIFF");
    tests.expect(db.setOperation('I', {"tags", "user:1"}, setResult) == OpStatus::WRONG_TYPE, "set ops reject other types");
    tests.expect(db.srem("tags", {"c++", "redis"}, changed) == OpStatus::OK && changed == 2 && !db.exists("tags"),
                 "SREM deletes an emptied set");

    // Persistence round trip with awkward values and TTLs
    db.flushAll();
    const std::string awkward = "hello world: with\nnewline\r\n and colons";
    db.set("spaced", awkward);
    db.rpush("list", {"a b", "", "c:d"});
    db.hset("hash", {{"field one", "value:1"}}, added);
    db.sadd("set", {"x y", "z"}, changed);
    db.set("ttl", "soon", 100000);
    tests.expect(db.dump("test_dump.tmp_rdb"), "DUMP writes a file");
    db.flushAll();
    tests.expect(db.load("test_dump.tmp_rdb"), "LOAD reads the file back");
    tests.expect(db.get("spaced", value) && value == awkward, "dump keeps special characters");
    listValues.clear();
    tests.expect(db.lrange("list", 0, -1, listValues) && listValues == std::vector<std::string>({"a b", "", "c:d"}), "dump keeps lists");
    tests.expect(db.hget("hash", "field one", value) && value == "value:1", "dump keeps hashes");
    tests.expect(db.sismember("set", "x y", member) == OpStatus::OK && member, "dump keeps sets");
    tests.expect(db.pttl("ttl") > 90000, "dump keeps TTLs");
    {
        std::ofstream corrupt("test_dump.tmp_rdb", std::ios::trunc);
        corrupt << "MYRDB 2\nK -1 5:broken";
    }
    tests.expect(!db.load("test_dump.tmp_rdb") && db.exists("spaced"), "a corrupt dump is rejected and data is kept");
    std::remove("test_dump.tmp_rdb");

    return tests.result();
}
