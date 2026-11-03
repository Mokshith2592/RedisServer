# RedisServer

A multi-threaded, Redis-compatible in-memory key-value server written from scratch in C++17.

It speaks **RESP** (the Redis Serialization Protocol), so standard tools such as `redis-cli`
and Redis client libraries can talk to it. It also accepts plain-text **inline commands**, so
`nc` or `telnet` work too. It supports five kinds of values (strings, counters, lists, hashes
and sets), key expiry, persistence to disk, pipelining and runtime statistics.

```
$ ./my_redis_server
Database loaded from dump.my_rdb (9 keys)
Redis Server Listining On Port 6379

$ redis-cli
127.0.0.1:6379> SET session abc123 EX 60
OK
127.0.0.1:6379> TTL session
(integer) 60
127.0.0.1:6379> SADD langs cpp go rust
(integer) 3
127.0.0.1:6379> SINTER langs other-langs
(empty array)
```

---

## Table of contents

1. [Features](#features)
2. [Requirements](#requirements)
3. [Building](#building)
4. [Running the server](#running-the-server)
5. [Connecting to the server](#connecting-to-the-server)
6. [Command reference](#command-reference)
   - [Server and connection](#server-and-connection)
   - [Strings and counters](#strings-and-counters)
   - [Generic key commands](#generic-key-commands)
   - [Lists](#lists)
   - [Hashes](#hashes)
   - [Sets](#sets)
7. [Key expiry](#key-expiry)
8. [Persistence](#persistence)
9. [The INFO command](#the-info-command)
10. [Replies and errors](#replies-and-errors)
11. [Architecture](#architecture)
12. [Testing](#testing)
13. [Differences from real Redis](#differences-from-real-redis)
14. [Project layout](#project-layout)
15. [Ideas for future work](#ideas-for-future-work)

---

## Features

| Area | What you get |
|------|--------------|
| **Data types** | Strings (including integer and float counters), lists, hashes, sets |
| **Commands** | About 75 commands: see the [command reference](#command-reference) |
| **Protocol** | Full RESP request parsing plus inline text commands with `"quoted"` arguments |
| **Pipelining** | Many commands in one TCP write are executed in order and answered in a single write |
| **TCP framing** | Commands split across packets, or several commands in one packet, are handled correctly |
| **Expiry** | `SET ... EX/PX`, `EXPIRE`, `TTL`, `PERSIST`. Keys expire *lazily* on access and *actively* through a background sweeper |
| **Persistence** | Binary-safe dump file that also stores TTLs. Writes are atomic (temp file + rename). Auto-save runs only when data changed. Data is saved on shutdown. Old-format dump files still load |
| **Concurrency** | One thread per client and a mutex-protected database, so commands are atomic |
| **Introspection** | `INFO` reports uptime, clients, command counts, expired keys, persistence state, keyspace and per-command call counts |
| **Robustness** | Bad arguments return an error instead of crashing. Protocol and memory limits apply. SIGPIPE-safe. Graceful shutdown on `SIGINT`/`SIGTERM` |

---

## Requirements

- **Linux or WSL** (Windows Subsystem for Linux). The server uses POSIX sockets and
  `MSG_NOSIGNAL`, so it doesn't build with MinGW or MSVC.
- **g++ with C++17 support** (tested with GCC 13) and **make**.
- **python3** (optional), only for the end-to-end test suite. It uses only the standard library.
- **redis-cli** (optional), the most convenient client: `sudo apt install redis-tools`.

On Windows, open the project from WSL:

```sh
wsl
cd /mnt/d/Projects/RedisServer
```

---

## Building

```sh
make            # build ./my_redis_server (objects go to build/)
make rebuild    # clean + build
make clean      # remove build output and test binaries
make run        # build and start the server
```

The build uses `-std=c++17 -Wall -Wextra -O2 -pthread` and compiles without warnings.
Header dependencies are tracked automatically (`-MMD -MP`), so editing a header rebuilds
the files that include it.

---

## Running the server

```sh
./my_redis_server                       # port 6379, dump.my_rdb, auto-save every 300 s
./my_redis_server 6380                  # old style: port as the first argument
./my_redis_server --port 6380 --dbfilename data.my_rdb --save-interval 60
./my_redis_server --save-interval 0     # disable periodic auto-save
./my_redis_server --help
```

| Option | Default | Description |
|--------|---------|-------------|
| `--port N` | `6379` | TCP port to listen on (all interfaces) |
| `--dbfilename FILE` | `dump.my_rdb` | Dump file loaded at startup and written by saves |
| `--save-interval SECONDS` | `300` | How often the auto-save thread checks for changes. `0` disables it |

### What happens at startup

1. The command-line options are parsed.
2. The dump file is loaded if it exists. If it is missing or corrupt, the server starts empty.
3. Two background threads start:
   - **auto-save**: every `--save-interval` seconds it writes the dump, but only if
     something changed since the last save;
   - **active expiry**: 10 times a second it deletes up to 200 expired keys.
4. The server listens and starts one thread per client connection.

### Stopping the server

Press **Ctrl+C**, or send `SIGTERM` (`kill <pid>`). The server stops accepting connections,
writes the dump file one last time, prints `Server Shutdown Complete` and exits.

---

## Connecting to the server

**redis-cli** (recommended):

```sh
redis-cli -p 6379
127.0.0.1:6379> PING
PONG
```

**netcat / telnet** with inline commands (one command per line):

```sh
printf 'SET greeting "hello world"\r\nGET greeting\r\n' | nc localhost 6379
# +OK
# $11
# hello world
```

Inline argument rules:

- Arguments are separated by spaces.
- `"double quotes"` keep spaces and understand `\n`, `\r`, `\t`, `\"` and `\\`.
- `'single quotes'` keep everything literally (only `\'` is special).
- A closing quote must be followed by a space or the end of the line.
  Unbalanced quotes are a protocol error.

**Any Redis client library** (redis-py, ioredis, Jedis, ...) also works for the
supported commands, because requests are standard RESP arrays.

---

## Command reference

Notation: `key` is a key name, `[...]` is optional, `a | b` means one of them, and `...` means
the argument can repeat. Reply types are described in [Replies and errors](#replies-and-errors).
Command names are case-insensitive.

### Server and connection

| Command | Reply | Description |
|---------|-------|-------------|
| `PING [message]` | `+PONG`, or the message as a bulk string | Health check |
| `ECHO message` | Simple string | Returns the message |
| `INFO [section]` | Bulk string | Server statistics. See [The INFO command](#the-info-command) |
| `DBSIZE` | Integer | Number of keys that aren't expired |
| `TIME` | Array `[seconds, microseconds]` | Current server time (Unix epoch) |
| `SELECT index` | `+OK` | Only database `0` exists. Any other index returns an error |
| `FLUSHALL` / `FLUSHDB` | `+OK` | Deletes every key |
| `SAVE` / `DUMP` | `+OK` | Writes the dump file now |
| `BGSAVE` | `+Background saving started` | Same as `SAVE` (it runs synchronously) |
| `LOAD` | `+OK` | Replaces the in-memory data with the dump file contents |
| `QUIT` | `+OK` | Closes the connection after replying |
| `HELP` | Array | Alphabetical list of every supported command |
| `COMMAND COUNT` | Integer | Number of supported commands |
| `COMMAND ...` / `CONFIG ...` | Empty array | Placeholders so `redis-cli` and `redis-benchmark` don't fail |

### Strings and counters

| Command | Reply | Description |
|---------|-------|-------------|
| `SET key value [EX seconds \| PX ms] [NX \| XX]` | `+OK`, or nil if `NX`/`XX` blocked it | Stores a string. Overwrites any existing key of any type and clears its TTL. `NX` = only if missing, `XX` = only if present |
| `GET key` | Bulk string or nil | Nil if the key is missing or isn't a string |
| `SETNX key value` | `1` if set, `0` if not | Sets the key only if it doesn't exist |
| `SETEX key seconds value` | `+OK` | `SET` with a TTL in seconds |
| `PSETEX key ms value` | `+OK` | `SET` with a TTL in milliseconds |
| `MSET key value [key value ...]` | `+OK` | Sets several keys atomically |
| `MGET key [key ...]` | Array | One entry per key; nil for missing or non-string keys |
| `GETSET key value` | Old value or nil | Sets a new value, returns the old one, clears the TTL |
| `GETDEL key` | Value or nil | Returns the value and deletes the key |
| `INCR key` / `DECR key` | Integer | Adds or subtracts 1. A missing key counts as `0` |
| `INCRBY key n` / `DECRBY key n` | Integer | Adds or subtracts `n` (64-bit, overflow is detected) |
| `INCRBYFLOAT key f` | Bulk string | Adds a floating-point amount, e.g. `INCRBYFLOAT price 1.5` → `"1.5"` |
| `APPEND key value` | Integer (new length) | Appends to a string, creating it if needed |
| `STRLEN key` | Integer | Length of the string (`0` if the key is missing) |
| `GETRANGE key start end` | Bulk string | Substring, both ends inclusive. Negative indices count from the end |

```
> SET counter 10
OK
> INCRBY counter 5
(integer) 15
> SET counter hello
OK
> INCR counter
(error) Error: value is not an integer or out of range
> SET lock owner1 NX PX 30000
OK
> SET lock owner2 NX PX 30000
(nil)
```

### Generic key commands

| Command | Reply | Description |
|---------|-------|-------------|
| `KEYS [pattern]` | Array (sorted) | Keys matching a glob pattern. With no pattern, all keys (`*`) |
| `EXISTS key [key ...]` | Integer | How many of the given keys exist. A repeated key counts each time |
| `DEL key [key ...]` / `UNLINK ...` | Integer | Number of keys actually deleted |
| `TYPE key` | `+string`, `+list`, `+hash`, `+set` or `+none` | Type of the stored value |
| `EXPIRE key seconds` | `1` or `0` | Sets a TTL. `0` means the key doesn't exist. A TTL ≤ 0 deletes the key immediately |
| `PEXPIRE key ms` | `1` or `0` | Same, in milliseconds |
| `TTL key` | Integer | Seconds left. `-1` = no TTL, `-2` = key doesn't exist |
| `PTTL key` | Integer | Milliseconds left, with the same `-1`/`-2` meanings |
| `PERSIST key` | `1` or `0` | Removes the TTL |
| `RENAME old new` | `+OK` or an error | Moves a key (and its TTL), overwriting `new` |
| `RANDOMKEY` | Bulk string or nil | A random key that isn't expired |

**Glob patterns** for `KEYS`:

| Pattern | Matches |
|---------|---------|
| `*` | Any sequence, including an empty one: `user:*` |
| `?` | Exactly one character: `user:?` matches `user:1` but not `user:10` |
| `[abc]` | One of the listed characters |
| `[a-z]` | One character in the range |
| `[^a]` | Any character except the listed ones |
| `\x` | A literal `x`, e.g. `\*` |

### Lists

Lists are double-ended queues. Indices start at `0`, and negative indices count from the end
(`-1` is the last element). A list is deleted automatically when its last element is removed.

| Command | Reply | Description |
|---------|-------|-------------|
| `LPUSH key value [value ...]` | Integer (new length) | Pushes to the head. Values are pushed one at a time, so `LPUSH k a b` gives `[b, a]` |
| `RPUSH key value [value ...]` | Integer (new length) | Pushes to the tail |
| `LPOP key [count]` | Bulk string or nil; with `count`, an array | Removes from the head |
| `RPOP key [count]` | Bulk string or nil; with `count`, an array | Removes from the tail |
| `LLEN key` | Integer | Length (`0` if missing) |
| `LRANGE key start stop` | Array | Elements from `start` to `stop`, inclusive. `LRANGE k 0 -1` returns everything |
| `LINDEX key index` | Bulk string or nil | Element at `index` |
| `LSET key index value` | `+OK` or an error | Replaces the element at `index` |
| `LREM key count value` | Integer | Removes occurrences of `value`: `count > 0` from the head, `count < 0` from the tail, `0` all of them |
| `LTRIM key start stop` | `+OK` | Keeps only the given range |
| `LINSERT key BEFORE\|AFTER pivot value` | Integer | New length; `-1` if the pivot isn't found, `0` if the key doesn't exist |

```
> RPUSH queue job1 job2 job3
(integer) 3
> LPOP queue
"job1"
> RPOP queue 5
1) "job3"
2) "job2"
> LPOP queue
(nil)
```

### Hashes

A hash maps fields to string values inside a single key, e.g. a user record.
`HGETALL`, `HKEYS` and `HVALS` return fields sorted by name.

| Command | Reply | Description |
|---------|-------|-------------|
| `HSET key field value [field value ...]` | Integer | Number of *new* fields created |
| `HMSET key field value [...]` | `+OK` | Same as `HSET` (legacy form) |
| `HSETNX key field value` | `1` or `0` | Sets the field only if it doesn't exist |
| `HGET key field` | Bulk string or nil | Value of one field |
| `HMGET key field [field ...]` | Array | Values, with nil for missing fields |
| `HDEL key field [field ...]` | Integer | Number of fields removed. An empty hash is deleted |
| `HEXISTS key field` | `1` or `0` | Whether the field exists |
| `HLEN key` | Integer | Number of fields |
| `HGETALL key` | Array | `field1, value1, field2, value2, ...` |
| `HKEYS key` / `HVALS key` | Array | Only the fields / only the values |
| `HINCRBY key field n` | Integer | Adds `n` to an integer field, creating it if needed |

```
> HSET user:1 name Ada role admin
(integer) 2
> HINCRBY user:1 logins 1
(integer) 1
> HGETALL user:1
1) "logins"
2) "1"
3) "name"
4) "Ada"
5) "role"
6) "admin"
```

### Sets

Unordered collections of unique strings. `SMEMBERS` and the set-algebra commands return
members sorted. A missing key behaves like an empty set, and an empty set is deleted.

| Command | Reply | Description |
|---------|-------|-------------|
| `SADD key member [member ...]` | Integer | Number of members actually added (duplicates are ignored) |
| `SREM key member [member ...]` | Integer | Number of members removed |
| `SISMEMBER key member` | `1` or `0` | Membership test |
| `SMEMBERS key` | Array | All members |
| `SCARD key` | Integer | Number of members |
| `SPOP key [count]` | Bulk string or nil; with `count`, an array | Removes and returns random members |
| `SRANDMEMBER key` | Bulk string or nil | A random member, which stays in the set |
| `SINTER key [key ...]` | Array | Members present in **every** set |
| `SUNION key [key ...]` | Array | Members present in **any** set |
| `SDIFF key [key ...]` | Array | Members of the first set that aren't in the others |

```
> SADD alice:skills cpp python sql
(integer) 3
> SADD bob:skills go python
(integer) 2
> SINTER alice:skills bob:skills
1) "python"
> SDIFF alice:skills bob:skills
1) "cpp"
2) "sql"
```

---

## Key expiry

Any key can have a time-to-live (TTL). TTLs are measured with a monotonic clock, so changing
the system time doesn't affect them. Keys are removed in two ways:

1. **Lazy expiry.** Every command that touches a key first checks whether it has expired.
   If it has, the key is deleted and the command behaves as if the key never existed.
2. **Active expiry.** A background thread wakes every 100 ms and deletes up to 200 expired
   keys, so keys nobody reads again don't sit in memory forever.

Both paths increase the `expired_keys` counter in `INFO stats`.

Rules:

| Action | Effect on TTL |
|--------|---------------|
| `SET`, `SETEX`, `MSET`, `GETSET` | Replaces the key; the old TTL is removed (unless `SET` gets `EX`/`PX`) |
| `INCR`, `APPEND`, `LPUSH`, `HSET`, `SADD`, ... | Modify the value and keep the TTL |
| `RENAME` | The TTL moves with the key |
| `PERSIST` | Removes the TTL |
| `EXPIRE key 0` (or negative) | Deletes the key immediately |

TTLs are saved in the dump file and restored on restart (see below).

---

## Persistence

The whole database is periodically saved to a single file (`dump.my_rdb` by default).

### When the database is saved

| Trigger | Notes |
|---------|-------|
| Auto-save thread | Every `--save-interval` seconds, **only if** something changed since the last save |
| `SAVE`, `BGSAVE`, `DUMP` commands | Immediately |
| Shutdown (`Ctrl+C`, `SIGTERM`) | Always |

### How a save works

1. The database lock is held only while the data is serialized into a memory buffer, so
   clients are blocked for as short a time as possible.
2. The buffer is written to `<file>.tmp`.
3. The temp file is renamed over the real file. Renaming is atomic, so a crash during a save
   never leaves a half-written dump: you get either the old file or the new one.

### File format (v2)

```
MYRDB 2
<type> <expire-at> <len>:<key> <count> <len>:<item> <len>:<item> ...
```

- `type`: `K` string, `L` list, `H` hash, `S` set
- `expire-at`: absolute Unix time in milliseconds, or `-1` for no TTL
- every string is **length-prefixed** (`5:hello`), so keys and values may contain spaces,
  colons, newlines or any binary data
- `count`: number of items. A string has 1, a list or set has one per element, and a hash
  has 2 per field (field, value, field, value, ...)

Example:

```
MYRDB 2
K -1 4:name 1 8:Mokshith
K 1767225600000 7:session 1 6:abc123
L -1 6:fruits 3 5:apple 6:banana 6:orange
H -1 8:user:100 4 4:name 3:Bob 3:age 2:30
S -1 5:langs 2 3:cpp 2:go
```

### Loading

- Keys whose TTL passed while the server was down are skipped.
- If the file is corrupt, loading fails and the data already in memory is kept
  (important for the `LOAD` command).
- Files in the **old v1 format** (`K key value`, `L key a b c`, `H key f:v`) are still read,
  and the next save converts them to v2 automatically.

---

## The INFO command

`INFO` returns a text report. `INFO <section>` returns a single section.

```
# Server
server_name:my_redis_server
server_version:1.1.0
tcp_port:6379
uptime_in_seconds:3600
uptime_in_days:0

# Clients
connected_clients:2

# Stats
total_connections_received:57
total_commands_processed:10432
expired_keys:12

# Persistence
dump_file:dump.my_rdb
changes_since_last_save:4
last_save_time:1762166400

# Keyspace
db0:keys=42,expires=3
keys_hash:5
keys_list:7
keys_set:2
keys_string:28
```

| Section | Shown by default | Contents |
|---------|------------------|----------|
| `server` | yes | Name, version, port, uptime |
| `clients` | yes | Currently connected clients |
| `stats` | yes | Total connections, total commands, keys expired so far |
| `persistence` | yes | Dump file, unsaved changes, last save time (Unix seconds, `0` = never) |
| `keyspace` | yes | Key count, keys with a TTL, keys per type |
| `commandstats` | no (`INFO commandstats` or `INFO everything`) | `cmdstat_<name>:calls=N` for each command used |

---

## Replies and errors

Replies use the standard RESP types:

| Type | Wire format | Shown by redis-cli as |
|------|-------------|-----------------------|
| Simple string | `+OK\r\n` | `OK` |
| Error | `-Error: ...\r\n` | `(error) Error: ...` |
| Integer | `:42\r\n` | `(integer) 42` |
| Bulk string | `$5\r\nhello\r\n` | `"hello"` |
| Nil | `$-1\r\n` | `(nil)` |
| Array | `*2\r\n$1\r\na\r\n$1\r\nb\r\n` | `1) "a"  2) "b"` |

Common errors:

| Error | Cause |
|-------|-------|
| `-Error: <CMD> requires ...` | Wrong number of arguments |
| `-Error: value is not an integer or out of range` | A numeric argument or stored value isn't a valid number |
| `-Error: increment or decrement would overflow` | A counter would leave the 64-bit range |
| `-WRONGTYPE Operation against a key holding the wrong kind of value` | E.g. `SADD` on a string key |
| `-Error: syntax error` | Unknown `SET` option, or `NX` and `XX` used together |
| `-Error : Unknown command` | Command isn't supported |
| `-Error: Protocol error` | Malformed RESP or unbalanced quotes. The connection is closed |

A bad command never crashes the server or affects other clients; it just returns an error.

---

## Architecture

```
            ┌─────────────────────────── my_redis_server ────────────────────────────┐
 client ──▶ │ RedisServer (accept loop)                                               │
 client ──▶ │   └─ one detached thread per client                                     │
 client ──▶ │        recv() → per-connection buffer                                   │
            │        RedisCommandHandler::parseNext()  (RESP or inline, incremental)  │
            │        RedisCommandHandler::execute()    (dispatch, errors, stats)      │
            │        send() all replies for the batch in one write                    │
            │                                                                         │
            │ RedisDatabase (singleton, one mutex)                                    │
            │   kv_store   list_store   hash_store   set_store                        │
            │   type_store (key → type)   expiry_map (key → deadline)                 │
            │                                                                         │
            │ background threads: auto-save (every N s) · active expiry (every 100 ms)│
            └─────────────────────────────────────────────────────────────────────────┘
```

### Components

**`RedisServer`** ([src/RedisServer.cpp](src/RedisServer.cpp))
- Opens the listening socket (`SO_REUSEADDR`, backlog `SOMAXCONN`) and accepts clients.
- Each client gets a detached thread running `handleClient()`.
- Turns on `TCP_NODELAY` so small replies are sent immediately.
- Keeps a growing buffer per connection. On every `recv()`, it parses and executes **all**
  complete commands in the buffer and keeps any incomplete remainder for the next read.
  This is what makes pipelining and split packets work.
- `sendAll()` loops until every byte is written, retries on `EINTR`, and uses
  `MSG_NOSIGNAL` so a client that disappears can't kill the process with `SIGPIPE`.
- Signal handling is async-signal-safe: the handler only clears a flag and calls
  `shutdown()` on the listening socket to wake `accept()`. The main thread then performs
  the final save.

**`RedisCommandHandler`** ([src/RedisCommandHandler.cpp](src/RedisCommandHandler.cpp))
- `parseNext()` parses one command starting at a given buffer offset and returns
  `COMPLETE` (with the number of bytes used), `INCOMPLETE` (wait for more data) or
  `PROTOCOL_ERROR`.
- `execute()` runs a parsed command, catches any exception (e.g. from number parsing) and
  turns it into an error reply. It also updates the counters used by `INFO`.
- `processCommand()` is a convenience wrapper for one complete command string, used by the
  unit tests.

**`RedisDatabase`** ([src/RedisDatabase.cpp](src/RedisDatabase.cpp))
- A singleton holding one map per data type, plus `type_store` (the type of each key) and
  `expiry_map` (TTL deadlines).
- Every public method takes `db_mutex`, so each command is atomic. For example,
  20 clients running `INCR` at the same time never lose an update.
- Methods that can fail in several ways return an `OpStatus`
  (`OK`, `NOT_FOUND`, `WRONG_TYPE`, `NOT_INTEGER`, `NOT_FLOAT`, `OVERFLOW`).
- Also holds the glob matcher, strict integer parsing, and dump/load.

**`main`** ([src/main.cpp](src/main.cpp))
- Parses options, loads the dump, starts the auto-save and active-expiry threads, then runs
  the server.

### Safety limits

| Limit | Value | What happens when exceeded |
|-------|-------|----------------------------|
| Arguments per RESP command | 1,048,576 | Protocol error, connection closed |
| Size of one argument | 512 MB | Protocol error, connection closed |
| Unparsed data per connection | 1 GB | Error reply, connection closed |

---

## Testing

```sh
make test          # unit tests
make integration   # end-to-end tests against the real binary (needs python3)
```

### Unit tests ([tests/](tests/))

- **`DatabaseTests.cpp`** tests the storage layer directly: every data type, `SET NX/XX`,
  counters and overflow, TTLs, lazy and active expiry, glob patterns, set algebra, and
  dump/load round trips with spaces, colons and newlines in values. It also checks that a
  corrupt dump is rejected without losing data.
- **`CommandHandlerTests.cpp`** tests exact RESP replies for commands, error messages,
  inline quoting, rejection of invalid numbers and truncated RESP, `INFO` output, and the
  incremental parser used for pipelining.

Each suite prints `all tests passed` or one `FAIL:` line per failed check, and exits non-zero
on failure.

### Integration test ([tests/integration_test.py](tests/integration_test.py))

Starts `my_redis_server` on a free port with a temporary dump file and talks to it over real
TCP sockets using a small built-in RESP client. It checks:

- 500 pipelined commands sent in one write are all answered, in order
- a command sent one byte at a time is reassembled correctly
- a 2 MB random binary value round-trips unchanged
- a bad argument returns an error and the connection keeps working
- inline commands with quotes
- 20 concurrent clients × 200 `INCR` = exactly 4000 (atomicity)
- `INFO` counts clients and connections
- active expiry removes a key nobody reads
- `QUIT` closes the connection
- after a `SIGTERM` restart, strings, lists, hashes, sets, binary values **and TTLs** survive
- an old v1 dump file loads correctly

### Sanitizers

To check for memory errors and data races, build with sanitizers and run the same tests:

```sh
g++ -std=c++17 -pthread -Iinclude -g -O1 -fsanitize=address,undefined src/*.cpp -o asan_server
python3 tests/integration_test.py ./asan_server

g++ -std=c++17 -pthread -Iinclude -g -O1 -fsanitize=thread src/*.cpp -o tsan_server
python3 tests/integration_test.py ./tsan_server
```

On some WSL kernels ThreadSanitizer aborts with "unexpected memory mapping". Running the
binary through `setarch -R` (which disables address randomization) fixes that.

---

## Differences from real Redis

This is a learning project, not a drop-in replacement. Main differences:

- **Single database.** Only `SELECT 0` works.
- **No** transactions (`MULTI`/`EXEC`), pub/sub, Lua scripting, streams, sorted sets,
  replication, clustering, AOF log or authentication.
- **Threading model.** Redis runs commands on a single event-loop thread. This server uses
  one OS thread per client and one global lock. That is simple and correct, but it scales
  worse with thousands of connections.
- **Persistence format** is its own text-based format (`MYRDB 2`), not Redis' RDB.
  `BGSAVE` runs synchronously.
- **Active expiry** scans the keys that have a TTL instead of sampling them randomly.
- `DUMP` saves the whole database (in Redis it serializes a single key).
- `ECHO` replies with a simple string instead of a bulk string.
- Some errors use `-Error: ...` instead of Redis' `-ERR ...`.
- `KEYS`, `SMEMBERS` and `HGETALL` return sorted results (Redis doesn't guarantee an order).
- `COMMAND` and `CONFIG` return empty replies; they exist only so client tools don't fail.

---

## Project layout

```
RedisServer/
├── Makefile                        build, test, integration, clean, run
├── README.md
├── dump.my_rdb                     sample data file
├── include/
│   ├── RedisServer.h               TCP server class
│   ├── RedisCommandHandler.h       parser, command execution, ServerStats
│   └── RedisDatabase.h             data store API, RedisType, OpStatus, SetCondition
├── src/
│   ├── main.cpp                    options, startup, background threads
│   ├── RedisServer.cpp             sockets, client threads, pipelining, shutdown
│   ├── RedisCommandHandler.cpp     RESP/inline parsing, all commands, INFO
│   └── RedisDatabase.cpp           storage, expiry, glob matching, persistence
└── tests/
    ├── TestHelpers.h               minimal TestSuite helper
    ├── DatabaseTests.cpp           storage layer unit tests
    ├── CommandHandlerTests.cpp     protocol and command unit tests
    └── integration_test.py         end-to-end TCP tests
```

### Adding a new command

1. Add the storage operation to `RedisDatabase` (header plus implementation). Take
   `db_mutex`, check the key's type with `findTypeUnlocked()`, and return an `OpStatus`.
2. Add a branch for it in `runCommand()` in `RedisCommandHandler.cpp`. Validate the
   argument count, parse numbers with `toInt` or `toLong`, and write the reply.
3. Add the name to `SUPPORTED_COMMANDS`, and also to `WRITE_COMMANDS` if it modifies data.
4. Write tests in both unit test files.

---

## Ideas for future work

- Transactions: `MULTI`, `EXEC`, `DISCARD`, `WATCH`
- Pub/sub: `SUBSCRIBE`, `PUBLISH`
- Sorted sets: `ZADD`, `ZRANGE`, `ZSCORE`, ...
- Blocking list operations: `BLPOP`, `BRPOP`
- `SCAN` for cursor-based iteration of large keyspaces
- An append-only log (AOF) for durability between saves
- An `epoll` event loop instead of one thread per client
- `AUTH` with a configurable password
