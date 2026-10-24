#!/usr/bin/env python3
"""Offline native TCP/WebSocket transport regression (Python stdlib + Apple clang only).

  python3 Tools/MacDev/test-epoch-relay.py [--sanitize] [--dll /path/EpochConnection.dll]
  python3 Tools/MacDev/test-epoch-relay.py --dll /path/EpochConnection.dll --probe

No game is launched and no player settings are touched. Default tests use fake credentials
and loopback sockets exclusively. --probe explicitly adds two real TLS/WebSocket upgrades
using the supplied DLL, WITHOUT sending a game packet, account name or password.
"""
import argparse
import base64
import hashlib
import os
from pathlib import Path
import selectors
import shutil
import socket
import socketserver
import ssl
import struct
import subprocess
import tempfile
import threading
import time
from urllib.parse import urlsplit, parse_qs

ROOT = Path(__file__).resolve().parents[2]
GREETING = b"\x00epoch\xff"


def readn(sock, size):
    result = bytearray()
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise EOFError(f"short stream: expected {size}, received {len(result)}")
        result.extend(chunk)
    return bytes(result)


def frame(sock, opcode, data=b"", final=True):
    header = bytes([(0x80 if final else 0) | opcode])
    n = len(data)
    header += bytes([n]) if n < 126 else (b"\x7e" + struct.pack("!H", n) if n <= 65535 else b"\x7f" + struct.pack("!Q", n))
    sock.sendall(header + data)


def read_frame(sock):
    first, second = readn(sock, 2)
    size = second & 127
    if size == 126:
        size = struct.unpack("!H", readn(sock, 2))[0]
    elif size == 127:
        size = struct.unpack("!Q", readn(sock, 8))[0]
    assert second & 128, "client WebSocket frames must be masked"
    assert size <= 2 * 1024 * 1024, "bounded test frame"
    key = readn(sock, 4)
    data = readn(sock, size)
    return first & 15, bytes(c ^ key[i % 4] for i, c in enumerate(data))


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        sock, server = self.request, self.server
        sock.settimeout(8)
        try:
            raw = bytearray()
            while not raw.endswith(b"\r\n\r\n"):
                raw.extend(readn(sock, 1))
                assert len(raw) < 16384
            lines = raw.decode("ascii").split("\r\n")
            method, target, version = lines[0].split()
            headers = {k.lower(): v.strip() for k, v in (l.split(":", 1) for l in lines[1:] if l)}
            server.requests.append((target, headers))
            assert method == "GET" and version == "HTTP/1.1"
            assert headers.get("authorization") == "Bearer test-relay-key"
            assert headers.get("user-agent") == "EpochTunnelTestLauncher/0.1 (+https://project-epoch.org)"
            if server.mode == "reject":
                sock.sendall(b"HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                return
            if server.mode == "redirect":
                sock.sendall(f"HTTP/1.1 302 Found\r\nLocation: ws://127.0.0.1:{server.redirect_port}/capture\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".encode())
                return
            if server.mode == "stall":
                time.sleep(2)
                return
            accept = base64.b64encode(hashlib.sha1((headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
            sock.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
            if server.mode == "text":
                frame(sock, 1, b"not game data")
            elif server.mode == "oversize":
                frame(sock, 2, b"x" * (1024 * 1024 + 1))
            else:
                # Server-first data, empty messages and continuation frames exercise the
                # stream abstraction. Game packets need not align with WS messages/frames.
                frame(sock, 2)
                frame(sock, 2, GREETING[:3], final=False)
                frame(sock, 0, GREETING[3:])
            while True:
                opcode, data = read_frame(sock)
                if opcode == 8:
                    frame(sock, 8, data)
                    return
                if opcode == 9:
                    frame(sock, 10, data)
                elif opcode == 2:
                    half = len(data) // 2
                    frame(sock, 2, data[:half], final=False)
                    frame(sock, 0, data[half:])
                elif opcode not in (0, 10):
                    raise AssertionError(f"unexpected opcode {opcode}")
        except (EOFError, ConnectionError, TimeoutError, OSError):
            pass
        except Exception as error:
            server.errors.append(str(error))


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, mode="echo", tls=None):
        self.mode, self.requests, self.errors = mode, [], []
        self.redirect_port = 0
        self.tls = tls
        super().__init__(("127.0.0.1", 0), Handler)
        if tls:
            self.socket = tls.wrap_socket(self.socket, server_side=True)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return f"{'wss' if self.tls else 'ws'}://127.0.0.1:{self.server_address[1]}"

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join()
        assert not self.errors, self.errors


class Harness:
    def __init__(self, binary, cwd, url, timeout=1):
        self.log = tempfile.TemporaryFile(mode="w+")
        self.proc = subprocess.Popen([str(binary), "--serve", url, str(timeout)], cwd=cwd,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.log, text=True, bufsize=1)
        self.ready()

    def line(self):
        with selectors.DefaultSelector() as sel:
            sel.register(self.proc.stdout, selectors.EVENT_READ)
            assert sel.select(8), "native harness response timed out"
        result = self.proc.stdout.readline().strip()
        assert result, f"native harness exited: {self.proc.poll()}"
        return result

    def ready(self):
        line = self.line().split()
        assert line[0] == "READY", line
        self.auth, self.world = map(int, line[1:])
        assert self.auth and self.world and self.auth != self.world

    def command(self, command):
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()
        return self.line()

    def stats(self):
        line = self.command("stats").split()
        assert line[0] == "STATS"
        return tuple(map(int, line[1:]))

    def idle(self):
        for _ in range(200):
            state = self.stats()
            if not state[0]:
                return state
            time.sleep(0.01)
        raise AssertionError(f"streams did not drain: {state}")

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
                raise
        self.log.seek(0)
        logs = self.log.read()
        self.log.close()
        assert self.proc.returncode == 0, logs
        assert "test-relay-key" not in logs, "credential leaked into diagnostics"
        assert "AddressSanitizer" not in logs and "runtime error:" not in logs, logs


def credential_tests(binary, cwd, dll):
    def run(path, valid=False):
        p = subprocess.run([str(binary), "--credential", str(path)], cwd=cwd, capture_output=True, text=True, timeout=5)
        assert (p.returncode == 0) == valid, p.stderr
        return p
    run(cwd / "missing.dll")
    bad = cwd / "wrong.dll"
    for size in (0, 64, 160768, 160769):
        bad.write_bytes(b"x" * size)
        run(bad)
    fifo = cwd / "fifo.dll"
    os.mkfifo(fifo)
    run(fifo)  # no blocking FIFO reads
    if dll:
        run(dll, True)
        data = bytearray(dll.read_bytes())
        # Change outside the credential: EXACT artifact identity is required, not just a marker.
        data[0] ^= 1
        bad.write_bytes(data)
        run(bad)
        linked = cwd / "linked.dll"
        linked.symlink_to(dll)
        run(linked)
    print("PASS missing / malformed / changed / symlink / FIFO DLL imports")


def tls_rejection_test(binary, cwd):
    config, cert, key = (cwd / name for name in ("tls.cnf", "tls.crt", "tls.key"))
    config.write_text("[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=127.0.0.1\n[ext]\nsubjectAltName=IP:127.0.0.1\n")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-config", str(config), "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server = Server(tls=context)
    h = Harness(binary, cwd, server.url, 2)
    try:
        with socket.create_connection(("127.0.0.1", h.auth), 3) as client:
            client.settimeout(5)
            assert client.recv(1) == b""
        assert h.idle() == (0, 0, 1)
        assert not server.requests, "HTTP credential sent before server trust was established"
        print("PASS untrusted TLS certificate rejected before HTTP / credential transmission")
    finally:
        h.close()
        server.close()


def transport_tests(binary, cwd):
    server = Server()
    h = Harness(binary, cwd, server.url)
    try:
        # Both channels, many WS fragments, binary NULs, sustained bidirectional backpressure.
        payload = bytes(range(256)) * 12288  # 3 MiB, much larger than either socket's buffers
        for port in (h.auth, h.world):
            with socket.create_connection(("127.0.0.1", port), 3) as client:
                client.settimeout(10)
                assert readn(client, len(GREETING)) == GREETING
                failures = []
                def send():
                    try:
                        client.sendall(payload)
                    except Exception as error:
                        failures.append(error)
                thread = threading.Thread(target=send)
                thread.start()
                assert readn(client, len(payload)) == payload
                thread.join(10)
                assert not thread.is_alive() and not failures, failures
        active, opened, closed = h.idle()
        assert (active, opened, closed) == (0, 2, 2), (active, opened, closed)
        first, second = server.requests[:2]
        target = urlsplit(first[0])
        assert target.path == "/relay/auth" and parse_qs(target.query) == {"worldPort": [str(h.world)]}
        assert second[0] == "/relay/world"
        print("PASS login + world: 6 MiB byte-exact, fragmentation, empty frames, server-first data")

        # Stop during live I/O, and restart. Sources / sessions / local sockets must be retired.
        for _ in range(3):
            with socket.create_connection(("127.0.0.1", h.auth), 3) as client:
                client.settimeout(5)
                assert readn(client, len(GREETING)) == GREETING
                old = (h.auth, h.world)
                assert h.command("stop") == "STOPPED"
                assert client.recv(1) == b""
                time.sleep(0.05)
                for port in old:
                    try:
                        sock = socket.create_connection(("127.0.0.1", port), 0.2)
                    except OSError:
                        pass
                    else:
                        sock.close()
                        raise AssertionError("listener remained open after stop")
                h.proc.stdin.write("start\n")
                h.proc.stdin.flush()
                h.ready()
        print("PASS active-stream cancellation, listener cleanup and restart (3 cycles)")
    finally:
        h.close()
        server.close()

    for mode in ("text", "oversize", "reject", "redirect", "stall"):
        server = Server(mode)
        sink = Server() if mode == "redirect" else None
        if sink:
            server.redirect_port = sink.server_address[1]
        h = Harness(binary, cwd, server.url, 0.5)
        try:
            with socket.create_connection(("127.0.0.1", h.auth), 3) as client:
                client.settimeout(5)
                try:
                    assert client.recv(1) == b"", mode
                except ConnectionResetError:
                    pass
            active, opened, closed = h.idle()
            assert active == 0 and closed == 1, (mode, active, opened, closed)
            if mode in ("reject", "redirect", "stall"):
                assert opened == 0, mode
            if sink:
                assert not sink.requests, "credential redirect was followed"
            print(f"PASS fail-closed: {mode}")
        finally:
            h.close()
            server.close()
            if sink:
                sink.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dll", type=Path)
    p.add_argument("--probe", action="store_true")
    p.add_argument("--sanitize", action="store_true")
    args = p.parse_args()
    if args.probe and not args.dll:
        p.error("--probe requires --dll")
    dll = args.dll.resolve() if args.dll else None
    with tempfile.TemporaryDirectory(prefix="wrathmodern-epoch-test-") as temp:
        cwd = Path(temp)
        (cwd / "Logs").mkdir()
        binary = cwd / "test-relay"
        command = ["clang++", "-std=c++17", "-fobjc-arc", "-DWRATHMODERN_EPOCH_RELAY_TESTING", "-I", str(ROOT),
                   "-Wall", "-Wextra", "-Wno-unused-parameter", "-g"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        command += [str(ROOT / file) for file in ("Platform/Mac/EpochRelay.mm", "Platform/Mac/EpochRelayConfig.cpp", "Tools/MacDev/test-epoch-relay.mm")]
        command += ["-framework", "Foundation", "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary), "--self-test"], check=True, cwd=cwd)
        credential_tests(binary, cwd, dll)
        transport_tests(binary, cwd)
        tls_rejection_test(binary, cwd)
        if args.probe:
            shutil.copy2(dll, cwd / "EpochConnection.dll")
            subprocess.run([str(binary), "--probe"], check=True, cwd=cwd, timeout=20)
        print("PASS all Epoch transport regressions")


if __name__ == "__main__":
    main()
