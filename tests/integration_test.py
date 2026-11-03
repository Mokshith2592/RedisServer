#!/usr/bin/env python3
"""End-to-end tests: start the real server and talk to it over TCP.

Usage: python3 tests/integration_test.py ./my_redis_server
Only uses the standard library (no redis-py needed).
"""
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

SERVER = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./my_redis_server")
failures = 0


def check(condition, message):
    global failures
    if not condition:
        failures += 1
        print(f"IntegrationTests: FAIL: {message}")


def encode(*args):
    out = f"*{len(args)}\r\n".encode()
    for arg in args:
        data = arg if isinstance(arg, bytes) else str(arg).encode()
        out += b"$%d\r\n%s\r\n" % (len(data), data)
    return out


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""

    def _line(self):
        while b"\r\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def _exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def read(self):
        line = self._line()
        kind, rest = line[:1], line[1:]
        if kind in (b"+", b"-"):
            return line.decode()
        if kind == b":":
            return int(rest)
        if kind == b"$":
            n = int(rest)
            if n < 0:
                return None
            data = self._exact(n + 2)[:-2]
            return data
        if kind == b"*":
            n = int(rest)
            return None if n < 0 else [self.read() for _ in range(n)]
        raise ValueError(f"bad reply {line!r}")

    def cmd(self, *args):
        self.sock.sendall(encode(*args))
        return self.read()

    def close(self):
        self.sock.close()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start_server(port, dump_file):
    proc = subprocess.Popen([SERVER, "--port", str(port), "--dbfilename", dump_file, "--save-interval", "0"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return proc
        except OSError:
            time.sleep(0.05)
    proc.kill()
    raise RuntimeError("server did not start")


def stop_server(proc):
    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=5)


def main():
    tmp = tempfile.mkdtemp()
    dump_file = os.path.join(tmp, "dump.my_rdb")
    port = free_port()
    proc = start_server(port, dump_file)

    try:
        c = Client(port)
        check(c.cmd("PING") == "+PONG", "PING")

        # Pipelining: 500 commands in a single write, 500 replies back in order
        c.sock.sendall(b"".join(encode("RPUSH", "pipe", i) for i in range(500)))
        replies = [c.read() for _ in range(500)]
        check(replies == list(range(1, 501)), "pipelined commands are all answered in order")

        # A command split across many tiny TCP writes
        for byte in encode("SET", "slow", "value"):
            c.sock.sendall(bytes([byte]))
            time.sleep(0.001)
        check(c.read() == "+OK", "a command sent byte by byte is reassembled")

        # Large and binary-unsafe values
        big = os.urandom(2 * 1024 * 1024)
        check(c.cmd("SET", "big", big) == "+OK", "SET a 2MB binary value")
        check(c.cmd("GET", "big") == big, "GET returns the 2MB value intact")

        # Bad input gets an error reply and the connection keeps working
        check(c.cmd("LINDEX", "pipe", "abc").startswith("-"), "bad number gives an error")
        check(c.cmd("PING") == "+PONG", "server still answers after bad input")

        # Inline (telnet style) commands with quotes
        c.sock.sendall(b'SET inline "hello world"\r\nGET inline\r\n')
        check(c.read() == "+OK" and c.read() == b"hello world", "inline commands with quotes")

        # Concurrency: 20 clients x 200 INCR
        def worker():
            w = Client(port)
            for _ in range(200):
                w.cmd("INCR", "counter")
            w.close()
        threads = [threading.Thread(target=worker) for _ in range(20)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        check(c.cmd("GET", "counter") == b"4000", "INCR is atomic across 20 concurrent clients")

        info = c.cmd("INFO").decode()
        check("connected_clients:1" in info, "INFO reports connected clients")
        # 1 readiness probe from start_server + this client + 20 workers
        check("total_connections_received:22" in info, "INFO counts connections")

        # Active expiry removes keys nobody reads again
        c.cmd("SET", "shortlived", "x", "PX", "50")
        time.sleep(0.4)
        check("expired_keys:1" in c.cmd("INFO", "stats").decode(), "active expiry purges untouched keys")

        # QUIT closes the connection
        q = Client(port)
        check(q.cmd("QUIT") == "+OK", "QUIT replies OK")
        check(q.sock.recv(10) == b"", "QUIT closes the connection")
        q.close()

        # Persistence across a restart, including TTLs and awkward values
        c.cmd("SET", "spaced", "a value with spaces: and colons\nand newlines", "EX", "1000")
        c.cmd("HSET", "user:1", "full name", "Ada Lovelace")
        c.cmd("SADD", "tags", "x", "y")
        c.close()
        stop_server(proc)

        proc = start_server(port, dump_file)
        c = Client(port)
        check(c.cmd("GET", "spaced") == b"a value with spaces: and colons\nand newlines", "values survive a restart")
        check(900 < c.cmd("TTL", "spaced") <= 1000, "TTLs survive a restart")
        check(c.cmd("HGET", "user:1", "full name") == b"Ada Lovelace", "hashes survive a restart")
        check(c.cmd("SCARD", "tags") == 2, "sets survive a restart")
        check(c.cmd("LLEN", "pipe") == 500, "lists survive a restart")
        check(c.cmd("GET", "big") == big, "binary values survive a restart")
        c.close()
        stop_server(proc)

        # Old v1 dump files still load
        with open(dump_file, "w") as f:
            f.write("K name Mokshith\nL fruits apple banana\nH user:100 age:30 name:Bob\n")
        proc = start_server(port, dump_file)
        c = Client(port)
        check(c.cmd("GET", "name") == b"Mokshith", "legacy dump: strings")
        check(c.cmd("LRANGE", "fruits", 0, -1) == [b"apple", b"banana"], "legacy dump: lists")
        check(c.cmd("HGET", "user:100", "name") == b"Bob", "legacy dump: hashes")
        c.close()
    finally:
        if proc.poll() is None:
            stop_server(proc)

    if failures == 0:
        print("IntegrationTests: all tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
