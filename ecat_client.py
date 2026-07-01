"""
ecat_client.py — Python client for the ecat-server TCP/JSON protocol.

Protocol overview
-----------------
All messages are newline-delimited JSON objects over a persistent TCP connection.

Client → server (commands):
  {"id": <int>, "cmd": "<name>", ...extra fields...}

Server → client (responses):
  {"id": <int>, "ok": true|false, ...extra fields...}

Server → client (async events, no "id"):
  {"event": "<name>", ...extra fields...}

Supported commands: init, scan, state, pdo_start, pdo_stop,
                    pdo_read, pdo_write, status, cleanup, shutdown
"""

import json
import queue
import socket
import threading
import time


class EcatClient:
    """Thread-safe client for ecat-server."""

    def __init__(self, host="127.0.0.1", port=7777):
        self.host = host
        self.port = port
        self._sock = None
        self._file = None
        self._next_id = 0
        self._id_lock = threading.Lock()
        self._pending = {}  # id -> Queue; protected by _pending_lock
        self._pending_lock = threading.Lock()
        self._events = queue.Queue()
        self._reader = None
        self._connected = False

    # ------------------------------------------------------------------
    # Connection management
    # ------------------------------------------------------------------

    def connect(self, timeout=5.0):
        """Open TCP connection and start the background reader thread."""
        self._sock = socket.create_connection((self.host, self.port), timeout=timeout)
        self._sock.settimeout(None)  # blocking I/O inside the reader thread
        self._file = self._sock.makefile("r", encoding="utf-8")
        self._connected = True
        self._reader = threading.Thread(
            target=self._read_loop, daemon=True, name="ecat-reader"
        )
        self._reader.start()

    def disconnect(self):
        """Close the connection and wait for the reader thread to finish."""
        self._connected = False
        if self._sock:
            try:
                self._sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self._sock.close()
            self._sock = None
        if self._reader and self._reader.is_alive():
            self._reader.join(timeout=2.0)
        self._reader = None

    def _ensure_connected(self):
        """Reconnect automatically if the socket was dropped."""
        if not self._connected or self._sock is None:
            self.connect()

    # ------------------------------------------------------------------
    # Reader thread
    # ------------------------------------------------------------------

    def _read_loop(self):
        """Dispatch incoming JSON lines to per-id response queues or the event queue."""
        try:
            for line in self._file:
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if "id" in msg:
                    with self._pending_lock:
                        q = self._pending.get(msg["id"])
                    if q is not None:
                        q.put(msg)
                elif "event" in msg:
                    self._events.put(msg)
        except (OSError, ValueError):
            pass
        finally:
            self._connected = False

    # ------------------------------------------------------------------
    # Core command send / receive
    # ------------------------------------------------------------------

    def send_cmd(self, timeout=10.0, **kwargs):
        """Assign an id, send the command, wait for and return the matching response."""
        self._ensure_connected()
        with self._id_lock:
            mid = self._next_id
            self._next_id += 1
        kwargs["id"] = mid
        resp_q = queue.Queue()
        with self._pending_lock:
            self._pending[mid] = resp_q
        try:
            self._sock.sendall((json.dumps(kwargs) + "\n").encode())
            try:
                return resp_q.get(timeout=timeout)
            except queue.Empty:
                raise TimeoutError(f"No response for id={mid} after {timeout}s")
        finally:
            with self._pending_lock:
                self._pending.pop(mid, None)

    # ------------------------------------------------------------------
    # Protocol convenience methods
    # ------------------------------------------------------------------

    def init(self, adapter):
        return self.send_cmd(cmd="init", adapter=adapter)

    def scan(self):
        return self.send_cmd(cmd="scan")

    def state(self, target, timeout_ms=5000):
        return self.send_cmd(cmd="state", target=target, timeout_ms=timeout_ms)

    def pdo_start(self):
        return self.send_cmd(cmd="pdo_start")

    def pdo_stop(self):
        return self.send_cmd(cmd="pdo_stop")

    def pdo_read(self):
        """Returns process-data bytes decoded from the hex 'hex' field."""
        resp = self.send_cmd(cmd="pdo_read")
        return bytes.fromhex(resp["hex"])

    def pdo_write(self, data: bytes):
        """Sends process data; encodes bytes as uppercase hex."""
        return self.send_cmd(cmd="pdo_write", hex=data.hex().upper())

    def status(self):
        return self.send_cmd(cmd="status")

    def cleanup(self):
        return self.send_cmd(cmd="cleanup")

    def shutdown(self):
        return self.send_cmd(cmd="shutdown")

    # ------------------------------------------------------------------
    # Async events
    # ------------------------------------------------------------------

    def get_event(self, timeout=0):
        """Return the next async event dict, or None if none is available."""
        try:
            return (
                self._events.get(timeout=timeout)
                if timeout > 0
                else self._events.get_nowait()
            )
        except queue.Empty:
            return None

    # ------------------------------------------------------------------
    # Context manager
    # ------------------------------------------------------------------

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *_):
        self.disconnect()


# ----------------------------------------------------------------------
# Demo
# ----------------------------------------------------------------------

if __name__ == "__main__":
    with EcatClient("127.0.0.1", 7777) as client:
        print("status   :", client.status())
        print("init     :", client.init("eth0"))
        print("scan     :", client.scan())
        print("pdo_start:", client.pdo_start())

        for i in range(5):
            data = client.pdo_read()
            print(f"  pdo_read [{i}]: {data.hex()}")
            time.sleep(0.1)

        print("pdo_stop :", client.pdo_stop())
        print("cleanup  :", client.cleanup())
    print("Disconnected.")
