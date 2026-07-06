#!/usr/bin/env python3
"""Range-capable HTTP server for the remote-SQLite tests, plus a test command runner.

DuckDB's httpfs reads remote files with HTTP range requests, so the stock
``python -m http.server`` (which ignores ``Range:``) is not enough. This server:

  * serves files from ``data/db`` with HEAD + ranged GET (206 / Content-Range), and
  * exposes ``/status/<code>`` endpoints that return an arbitrary HTTP status,

so the whole remote suite runs hermetically with no third-party URLs.

It starts a server, exports the relevant env vars into the child's environment, runs the child, tears
the server down, and exits with the child's return code.

Usage:
    sqlite_http_test_server.py [--server MODE] <test-binary> [args...]

MODE (default ``stdlib``):
  * ``stdlib``      -- the built-in threaded server above (HTTP + /status + /walerr error injection).
                       Reliable for the serial + error-injection tests.
  * ``rclone-http`` -- ``rclone serve http`` (a robust Go server) for the concurrency stress test, which
                       overwhelms the stdlib server. Sets SQLITE_HTTP_TEST_URL + SQLITE_HTTP_ROBUST.
  * ``rclone-s3``   -- ``rclone serve s3`` so the suite can exercise the ``s3://`` path (incl. the
                       credentialed ``-wal``/``-journal`` sidecar probe). Sets SQLITE_S3_TEST_*.
The rclone modes require the ``rclone`` binary on PATH; tests gate on the env vars above so they skip
where it is not wired.
"""

import contextlib
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SERVE_DIR = (Path(__file__).resolve().parent.parent / "data" / "db").resolve()
STATUS_RE = re.compile(r"^/status/(\d{3})$")
# /walerr/<code>/<file>: serve <file> normally, but return <code> for its "-wal"/"-journal" sidecar.
# Lets the WAL fail-closed guard be tested when the sidecar's state is unverifiable (non-404 error).
WALERR_RE = re.compile(r"^/walerr/(\d{3})/(.+)$")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # silence per-request logging
        pass

    def _resolve(self, rel):
        # Map a relative path to a file under SERVE_DIR, refusing path traversal. Returns None if it
        # escapes SERVE_DIR or is not a file.
        target = (SERVE_DIR / rel).resolve()
        if SERVE_DIR not in target.parents and target != SERVE_DIR:
            return None
        return target if target.is_file() else None

    def _status_endpoint(self):
        m = STATUS_RE.match(self.path.split("?", 1)[0])
        return int(m.group(1)) if m else None

    def _send_simple(self, code, body=b""):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self._serve(head_only=True)

    def do_GET(self):
        self._serve(head_only=False)

    def _serve(self, head_only):
        code = self._status_endpoint()
        if code is not None:
            # Emulate httpbin-style status endpoints for error-mapping tests.
            self._send_simple(code, f"status {code}\n".encode())
            return

        walerr = WALERR_RE.match(self.path.split("?", 1)[0])
        if walerr:
            forced, rel = int(walerr.group(1)), walerr.group(2)
            if rel.endswith("-wal") or rel.endswith("-journal"):
                # The WAL guard's sidecar probe lands here: return the forced (non-404) error so the
                # probe cannot confirm the sidecar absent, and the open must fail closed.
                self._send_simple(forced, f"status {forced}\n".encode())
                return
            target = self._resolve(rel)
            if target is None:
                self._send_simple(404, b"not found\n")
                return
            self._serve_file(target, head_only)
            return

        target = self._resolve(self.path.lstrip("/").split("?", 1)[0])
        if target is None:
            self._send_simple(404, b"not found\n")
            return

        self._serve_file(target, head_only)

    def _serve_file(self, target, head_only):
        # Seek+read only the requested span rather than reading the whole file per request: DuckDB's
        # CachingFileSystem issues many ranged GETs (more so under parallel scans), and the fixtures
        # can be several MB. Only the single `bytes=start-end` (or open-ended `bytes=start-`) form
        # httpfs issues is handled; suffix (`bytes=-N`) and multi-range requests are not.
        size = target.stat().st_size
        rng = self.headers.get("Range")
        if rng:
            m = re.match(r"bytes=(\d*)-(\d*)", rng.strip())
            if m:
                start = int(m.group(1)) if m.group(1) else 0
                end = int(m.group(2)) if m.group(2) else size - 1
                end = min(end, size - 1)
                if start > end or start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                length = end - start + 1
                self.send_response(206)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
                self.send_header("Content-Length", str(length))
                self.end_headers()
                if not head_only:
                    with open(target, "rb") as f:
                        f.seek(start)
                        self.wfile.write(f.read(length))
                return

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        if not head_only:
            with open(target, "rb") as f:
                self.wfile.write(f.read())


class Server(ThreadingHTTPServer):
    # Many DuckDB connections (each parallel-scanning) can hit this server at once. socketserver's
    # default listen backlog is 5, so excess simultaneous connections would be reset; raise it, reap
    # threads as daemons, and allow address reuse.
    request_queue_size = 128
    daemon_threads = True
    allow_reuse_address = True


# rclone-s3 credentials: the s3 test's CREATE SECRET must use these exact values (see http_sqlite_16).
S3_KEY = "testkey"
S3_SECRET = "testsecret"


def _free_port():
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait_for_port(port, timeout=30.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
            s.settimeout(0.5)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.1)
    return False


def run_stdlib(cmd):
    server = Server(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = dict(os.environ)
    env["SQLITE_HTTP_TEST_URL"] = f"http://127.0.0.1:{port}"
    try:
        return subprocess.run(cmd, env=env).returncode
    finally:
        server.shutdown()
        server.server_close()


def run_rclone(cmd, scheme):
    # scheme "http": rclone serve http (robust file server for the concurrency stress test).
    # scheme "s3":   rclone serve s3 over data/'s parent so data/db is the "db" bucket (s3://db/<file>).
    port = _free_port()
    env = dict(os.environ)
    if scheme == "http":
        args = ["rclone", "serve", "http", "--addr", f"127.0.0.1:{port}", "--read-only", str(SERVE_DIR)]
        env["SQLITE_HTTP_TEST_URL"] = f"http://127.0.0.1:{port}"
        env["SQLITE_HTTP_ROBUST"] = "1"  # marker so the concurrency test runs only on the robust server
    else:  # s3
        args = [
            "rclone",
            "serve",
            "s3",
            "--addr",
            f"127.0.0.1:{port}",
            "--auth-key",
            f"{S3_KEY},{S3_SECRET}",
            str(SERVE_DIR.parent),
        ]
        env["SQLITE_S3_TEST_ENDPOINT"] = f"127.0.0.1:{port}"
        env["SQLITE_S3_TEST_URL"] = f"s3://{SERVE_DIR.name}"  # s3://db
    proc_err = tempfile.TemporaryFile()
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=proc_err)
    try:
        if not _wait_for_port(port):
            proc_err.seek(0)
            detail = proc_err.read().decode(errors="replace").strip()
            print(f"rclone serve {scheme} did not become ready on port {port}: {detail}", file=sys.stderr)
            return 3
        return subprocess.run(cmd, env=env).returncode
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    args = sys.argv[1:]
    mode = "stdlib"
    if args and args[0] == "--server":
        if len(args) < 3:
            print("usage: --server <stdlib|rclone-http|rclone-s3> <command> [args...]", file=sys.stderr)
            return 2
        mode = args[1]
        args = args[2:]
    if not args:
        print("usage: sqlite_http_test_server.py [--server MODE] <command> [args...]", file=sys.stderr)
        return 2
    if mode == "stdlib":
        return run_stdlib(args)
    if mode == "rclone-http":
        return run_rclone(args, "http")
    if mode == "rclone-s3":
        return run_rclone(args, "s3")
    print(f"unknown server mode: {mode}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
