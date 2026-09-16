# RedisServer

A small C++ Redis-compatible server supporting strings, lists, and hashes.

Supported hash commands: `HSET`, `HMSET`, `HGET`, `HMGET`, `HDEL`, `HEXISTS`,
`HLEN`, `HGETALL`, `HKEYS`, and `HVALS`.

Additional list commands: `LINDEX`, `LSET`, `LREM`, and `LTRIM`. `LRANGE` also
accepts Redis-style negative indices and returns an empty array for a missing key.

Run the checks with `make test`.
