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
                    pdo_read, pdo_write, status, cleanup, shutdown,
                    pdo_mapping

PDO mapping
-----------
The "pdo_mapping" command asks the server to dynamically walk the CoE PDO
assign objects of every slave (via SDO reads, no hardcoded C structs) and
return a description of every mapped field: name, CoE index/subindex, its
absolute byte/bit offset inside the same buffer used by pdo_read/pdo_write,
its bit length, EtherCAT data type, and (when the field is byte-aligned) a
Python `struct` format string.

All binary encoding/decoding of individual fields happens on the Python
side (see PdoField/PdoImage below) -- the C server only ever moves whole,
opaque, hex-encoded process-data buffers, exactly like before.
"""

import argparse
import json
import queue
import shlex
import socket
import struct
import threading
import time
from pprint import pprint


class PdoField:
    """Metadata + codec for a single PDO field, as returned by the server's
    "pdo_mapping" command.

    Every field carries an absolute byte/bit position inside the *same*
    buffer returned by ``pdo_read()`` / expected by ``pdo_write()``, so a
    field can be decoded from (or encoded into) that buffer directly.

    Byte-aligned standard-size fields (int8/16/32/64, uint8/16/32/64,
    float32/64) come with ``struct_fmt`` (a little-endian ``struct`` format
    string) for a fast path. Sub-byte fields (BOOLEAN, BITn), 24-bit
    integers and strings have ``struct_fmt`` set to None and are
    decoded/encoded generically via bit-shifting on
    ``bit_offset``/``bit_len``.
    """

    def __init__(self, meta: dict):
        self.name = meta["name"]
        self.index = meta["index"]
        self.subindex = meta["subindex"]
        self.byte_offset = meta["byte_offset"]
        self.bit_offset = meta["bit_offset"]
        self.bit_len = meta["bit_len"]
        self.size_bytes = meta["size_bytes"]
        self.dtype = meta["dtype"]
        self.dtype_id = meta.get("dtype_id")
        self.signed = meta["signed"]
        self.is_float = meta["is_float"]
        self.struct_fmt = meta.get("struct_fmt")

    def __repr__(self):
        return (
            f"PdoField(name={self.name!r}, dtype={self.dtype!r}, "
            f"byte_offset={self.byte_offset}, bit_offset={self.bit_offset}, "
            f"bit_len={self.bit_len})"
        )

    def to_dict(self):
        """Return the field's own metadata as a plain dict (type/size info)."""
        return {
            "name": self.name,
            "index": self.index,
            "subindex": self.subindex,
            "byte_offset": self.byte_offset,
            "bit_offset": self.bit_offset,
            "bit_len": self.bit_len,
            "size_bytes": self.size_bytes,
            "dtype": self.dtype,
            "dtype_id": self.dtype_id,
            "signed": self.signed,
            "is_float": self.is_float,
            "struct_fmt": self.struct_fmt,
        }

    @property
    def end_byte_offset(self) -> int:
        """First byte offset *past* this field -- the minimum buffer length
        required to decode/encode it."""
        return self.byte_offset + (self.bit_offset + self.bit_len + 7) // 8

    def _check_buffer(self, buf, action: str):
        if len(buf) < self.end_byte_offset:
            raise ValueError(
                f"Cannot {action} PDO field {self.name!r}: buffer has "
                f"{len(buf)} byte(s), but this field needs at least "
                f"{self.end_byte_offset} byte(s) (byte_offset={self.byte_offset}, "
                f"bit_offset={self.bit_offset}, bit_len={self.bit_len}). "
                f"Did you forget to call PdoImage.update(client.pdo_read()) "
                f"before reading, or is the mapping stale (re-run scan/pdo_mapping)?"
            )

    def decode(self, buf: bytes):
        """Extract this field's value out of a full process-data buffer."""
        self._check_buffer(buf, "decode")

        if self.struct_fmt:
            return struct.unpack_from(self.struct_fmt, buf, self.byte_offset)[0]

        # Generic path: sub-byte fields, 24-bit ints, or anything without a
        # direct struct format. Read just enough whole bytes to cover the
        # bit range, then mask/shift.
        nbytes = (self.bit_offset + self.bit_len + 7) // 8
        raw = int.from_bytes(
            buf[self.byte_offset : self.byte_offset + nbytes], "little"
        )
        raw >>= self.bit_offset
        raw &= (1 << self.bit_len) - 1
        if self.signed and (raw & (1 << (self.bit_len - 1))):
            raw -= 1 << self.bit_len
        return raw

    def encode(self, buf: bytearray, value):
        """Write this field's value into a full (mutable) process-data buffer."""
        self._check_buffer(buf, "encode")

        if self.struct_fmt:
            struct.pack_into(self.struct_fmt, buf, self.byte_offset, value)
            return

        nbytes = (self.bit_offset + self.bit_len + 7) // 8
        window = int.from_bytes(
            buf[self.byte_offset : self.byte_offset + nbytes], "little"
        )
        mask = ((1 << self.bit_len) - 1) << self.bit_offset
        window &= ~mask
        window |= (int(value) << self.bit_offset) & mask
        buf[self.byte_offset : self.byte_offset + nbytes] = window.to_bytes(
            nbytes, "little"
        )


class PdoImage:
    """Hierarchical, per-slave view over a raw PDO process-image buffer,
    using the dynamic mapping returned by ``EcatClient.pdo_mapping()``.

    The data the user actually interacts with is a plain nested dict of
    already-decoded Python values -- one entry per slave, each split into
    "inputs" and "outputs", mirroring how the C side models each slave as
    a ``struct { inputs; outputs; }``:

        image.slaves = {
            1: {"inputs": {"motor_pos": 123, ...}, "outputs": {"target_pos": 0, ...}},
            2: {"inputs": {...},                   "outputs": {...}},
            ...
        }

    Changing a value, e.g. ``image.slaves[1]["outputs"]["target_pos"] = 1000``,
    is a plain dict assignment -- it only ever touches that dict entry and
    has no side effects on the binary buffer or any other slave/field.

    The binary process-data buffer (as read from / written to the server)
    is only produced or consumed at the edges, like a struct <-> byte-array
    cast in C:

      * ``update(data)`` decodes a freshly read buffer (e.g. from
        ``client.pdo_read()``) into ``self.slaves``.
      * ``bytes()`` walks ``self.slaves`` and packs every field's *current*
        value back into a buffer at its correct byte/bit offset, ready to
        hand to ``client.pdo_write()``.

    Example
    -------
        mapping = client.pdo_mapping()
        image = PdoImage(mapping)

        image.update(client.pdo_read())                    # load latest process data
        print(image.slaves[1]["inputs"]["ActualPosition"])  # read a single field

        image.slaves[1]["outputs"]["target_pos"] = 1000    # stage a write
        client.pdo_write(image.bytes())                     # send the whole buffer back
    """

    def __init__(self, mapping: dict, data: bytes = b""):
        self.slave_meta = mapping.get("slaves", [])
        self._field_map = {}  # (slave_id, direction, name) -> PdoField
        self.slaves = {}  # slave_id -> {"inputs": {name: value}, "outputs": {name: value}}

        for slave in self.slave_meta:
            slave_id = slave.get("slave")
            entry = self.slaves.setdefault(slave_id, {"inputs": {}, "outputs": {}})
            for direction in ("outputs", "inputs"):
                for raw_field in slave.get(direction, []):
                    name = raw_field.get("name")
                    if not name:
                        continue  # unnamed / filler entry, nothing to bind to
                    field = PdoField(raw_field)
                    self._field_map[(slave_id, direction, name)] = field
                    entry[direction][name] = 0

        # Minimum buffer length needed to decode/encode every mapped field.
        self.min_size = max(
            (f.end_byte_offset for f in self._field_map.values()), default=0
        )
        self.data = bytearray()
        self.update(data)

    def update(self, data: bytes):
        """Replace the underlying buffer (e.g. with the latest pdo_read())
        and decode every mapped field into ``self.slaves``.

        `data` is truncated/zero-padded to exactly ``self.min_size`` bytes
        -- the size actually covered by the mapping -- regardless of how
        large the buffer handed in is (e.g. pdo_read() may return the
        server's full, oversized I/O map buffer). This keeps ``self.data``
        (and therefore ``bytes()``) sized to just the mapped process image,
        never to the server's raw buffer capacity. Field access never fails
        with a confusing low-level struct/index error -- reads simply
        return 0 for not-yet-loaded fields until a real pdo_read() is
        applied.
        """
        buf = bytearray(data[: self.min_size])
        if len(buf) < self.min_size:
            buf.extend(b"\x00" * (self.min_size - len(buf)))
        self.data = buf

        for (slave_id, direction, name), field in self._field_map.items():
            self.slaves[slave_id][direction][name] = field.decode(self.data)

    def bytes(self) -> bytes:
        """Pack the current values held in ``self.slaves`` into a
        process-data buffer of exactly ``self.min_size`` bytes -- the size
        actually covered by the mapping -- ready to hand to
        ``pdo_write()``. This is the reverse of ``update()``, analogous to
        reinterpreting a C struct as a flat byte array.
        """
        buf = bytearray(self.min_size)
        buf[: len(self.data)] = self.data[: self.min_size]
        for (slave_id, direction, name), field in self._field_map.items():
            value = self.slaves[slave_id][direction][name]
            field.encode(buf, value)
        self.data = buf
        return bytes(buf)

    def describe(self, slave_id=None, direction=None) -> dict:
        """Return {(slave_id, direction, name): type/size metadata} for
        every mapped field, optionally filtered by slave_id and/or
        direction, so a caller can inspect types/sizes without decoding
        any values."""
        return {
            key: field.to_dict()
            for key, field in self._field_map.items()
            if (slave_id is None or key[0] == slave_id)
            and (direction is None or key[1] == direction)
        }


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

    def pdo_mapping(self) -> dict:
        """Return the raw dynamic PDO mapping dict (as sent by the server):

            {"slaves": [{"slave": 1, "name": "...",
                         "outputs_bits": N, "inputs_bits": M,
                         "outputs": [ {field}, ... ],
                         "inputs":  [ {field}, ... ]}, ...]}

        Each field dict has "name", "index", "subindex", "byte_offset",
        "bit_offset", "bit_len", "size_bytes", "dtype", "dtype_id",
        "signed", "is_float" and "struct_fmt".

        Use ``PdoImage(client.pdo_mapping())`` for a convenient dict-like
        read/write wrapper instead of consuming this raw structure directly.
        """
        resp = self.send_cmd(cmd="pdo_mapping")
        # print("========= pdo_mapping =========")
        # pprint(resp)
        # print("==============================")

        return resp.get("mapping", {})

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
# Drive test helpers -- Python port of the hand-written test routines in
# ecat_cli.c (fsrt / nfsrt / nfspd / helper_spd_limiter / nrelays_set /
# relays_set / prtall).
#
# ecat_cli.c hardcoded two drives directly into a `process_data_t` struct
# with "doubled" field names (target_pos/target_pos1, motor_pos/motor_pos1,
# Max_speed/Max_speed1, relays/relays1, control_flags/control_flags1, ...)
# and picked between them with an integer `n` (0 or 1).
#
# Here there is no hardcoded struct: every slave gets its own entry in
# ``PdoImage.slaves`` keyed by its real EtherCAT slave number, each with its
# own plain "target_pos"/"motor_pos"/... fields -- the same field name is
# naturally disambiguated by slave id instead of by a "1" suffix. So `n` in
# ecat_cli.c simply becomes `slave_id` here (the actual slave numbers, as
# returned by ``EcatClient.pdo_mapping()`` / ``PdoImage.slaves``).
#
# The bus exchange itself runs continuously in the server's own background
# thread (ecat_thread_func() in ecat_eth.c); ecat_cli.c's
# simple_communication_cycle() (10 x ecx_send/receive_processdata + 10ms
# sleep) is therefore reproduced here simply as a few pdo_read() polls with
# a short delay.
# ----------------------------------------------------------------------


def helper_spd_limiter(spd: int) -> int:
    """Clamp a speed setpoint, mirroring ecat_cli.c's helper_spd_limiter()."""
    spd = max(200, min(spd, 1000))  # debug-mode limits
    return max(0, min(spd, 1000))


def simple_communication_cycle(
    client: EcatClient, image: PdoImage, cycles: int = 10, delay: float = 0.01
):
    """Let a few PDO cycles elapse and refresh `image` from the bus.

    Mirrors ecat_cli.c's simple_communication_cycle(): there, 10 explicit
    ecx_send/receive_processdata() calls (10ms apart) were needed to drive
    the bus. Here the exchange already runs continuously in the server's
    background thread, so this just polls pdo_read() a few times with a
    short delay to let a few of those cycles elapse and to refresh `image`.
    """
    for _ in range(cycles):
        print("PDO WRITE")
        client.pdo_write(image.bytes())
        print("PDO READ")
        image.update(client.pdo_read())
        time.sleep(delay)


def relays_set(client: EcatClient, image: PdoImage, relays_val: int, slave_ids=None):
    """Set the 'relays' output on one or more slaves and push it to the bus.

    Mirrors ecat_cli.c's relays_set(), which set relays/relays1 on both
    hardcoded drives at once; here it defaults to every mapped slave that
    has a 'relays' output.
    """
    ids = list(image.slaves.keys()) if slave_ids is None else list(slave_ids)
    for sid in ids:
        outputs = image.slaves.get(sid, {}).get("outputs", {})
        if "relays" in outputs:
            outputs["relays"] = relays_val
    simple_communication_cycle(client, image)


def nrelays_set(client: EcatClient, image: PdoImage, slave_id, relays_val: int):
    """Set the 'relays' output on a single slave, mirroring ecat_cli.c's
    nrelays_set(n, relays_val) (there `n` selected drive 0/1; here
    `slave_id` directly selects the slave)."""
    outputs = image.slaves.get(slave_id, {}).get("outputs")
    if not outputs or "relays" not in outputs:
        raise KeyError(f"Slave {slave_id} has no 'relays' output field")
    outputs["relays"] = relays_val
    simple_communication_cycle(client, image)


def fsrt(client: EcatClient, image: PdoImage, rel_target: int):
    """Move every mapped drive by `rel_target`, relative to its current
    position, mirroring ecat_cli.c's fsrt() (which drove both hardcoded
    axes at once)."""
    simple_communication_cycle(client, image)
    for sid, slave in image.slaves.items():
        inputs, outputs = slave.get("inputs", {}), slave.get("outputs", {})
        if "target_pos" not in outputs or "motor_pos" not in inputs:
            continue
        outputs["target_pos"] = inputs["motor_pos"] + rel_target
        if "control_flags" in outputs:
            outputs["control_flags"] = 1 if rel_target != 0 else 0
        print(
            f"[slave {sid}] motor_pos={inputs['motor_pos']}  "
            f"target_pos={outputs['target_pos']}"
        )
    simple_communication_cycle(client, image)


def nfsrt(client: EcatClient, image: PdoImage, slave_id, rel_target: int):
    """Move a single drive by `rel_target`, mirroring ecat_cli.c's
    nfsrt(n, rel_target)."""
    simple_communication_cycle(client, image)
    slave = image.slaves.get(slave_id)
    if slave is None:
        raise KeyError(f"No such slave: {slave_id}")
    inputs, outputs = slave.get("inputs", {}), slave.get("outputs", {})
    control_flags = 1 if rel_target != 0 else 0
    if rel_target != 0 and "target_pos" in outputs and "motor_pos" in inputs:
        outputs["target_pos"] = inputs["motor_pos"] + rel_target
    if "control_flags" in outputs:
        outputs["control_flags"] = control_flags
    simple_communication_cycle(client, image)


def nfspd(client: EcatClient, image: PdoImage, slave_id, spd: int):
    """Set the max-speed output for one drive, or every drive if `slave_id`
    is None, mirroring ecat_cli.c's nfspd(n, spd) (there any `n` other than
    0/1 meant "set both")."""
    simple_communication_cycle(client, image)
    spd = helper_spd_limiter(spd)
    print(f"speed set to {spd}")
    ids = list(image.slaves.keys()) if slave_id is None else [slave_id]
    for sid in ids:
        outputs = image.slaves.get(sid, {}).get("outputs", {})
        if "Max_speed" in outputs:
            outputs["Max_speed"] = spd
    simple_communication_cycle(client, image)


def prtall(
    client: EcatClient, image: PdoImage, num_iterations: int = 1, delay: float = 0.1
):
    """Repeatedly print position/target/diff/speed for every slave that
    exposes those fields, mirroring ecat_cli.c's prtall()."""
    for _ in range(num_iterations):
        image.update(client.pdo_read())
        for sid, slave in sorted(image.slaves.items()):
            inputs, outputs = slave.get("inputs", {}), slave.get("outputs", {})
            if "motor_pos" not in inputs:
                continue
            pos = inputs["motor_pos"]
            target = outputs.get("target_pos", 0)
            spd = inputs.get("Motor_speed", 0)
            print(f"######## slave {sid}")
            print(f"POS:    {pos}")
            print(f"TARGET: {target}")
            print(f"DIFF:   {pos - target}")
            print(f"SPD:    {spd}")
        time.sleep(delay)


# ----------------------------------------------------------------------
# Command-line interface
#
# A small REPL, in the spirit of ecat_cli.c's REPL, but scoped to what the
# server (ecat_eth.c) actually understands over the wire (init, scan,
# state, pdo_start/stop/read/write, status, cleanup, shutdown,
# pdo_mapping), plus the dynamic PdoImage and the drive test helpers above.
# Unlike ecat_cli.c, this does *not* re-implement every command from that
# file (read-config, text-write, wap, motor-enable, pdo-loop, ...) since
# those poke at a hardcoded C struct that no longer exists on the server
# side.
# ----------------------------------------------------------------------

HELP_TEXT = """\
Connection is established automatically to <host>:<port> on startup; the
server auto-initializes SOEM, scans the bus and starts PDO exchange for you
as soon as the TCP connection is accepted (one client == one EtherCAT
master), so most sessions only ever need pdo-read / get / set / pdo-write.

Protocol commands (ecat_eth.c):
  init <adapter>                 Force re-init SOEM on a network adapter
  scan                           Re-scan the EtherCAT bus
  state <target> [timeout_ms]    Request slave state (INIT/PREOP/SAFEOP/OP/BOOT)
  pdo-start                      Start cyclic PDO exchange
  pdo-stop                       Stop cyclic PDO exchange
  pdo-read                       Read + decode the PDO image, print it
  pdo-write                      Push the currently staged image to the bus
  mapping                        (Re)fetch PDO mapping, reset the image
  status                         Print server/bus status
  cleanup                        Stop PDO + release the SOEM master
  shutdown                       Ask the server process to exit

PDO image helpers:
  dump                           Print the whole decoded PDO image
  get <slave> <inputs|outputs> <field>       Print one field's last-read value
  set <slave> <inputs|outputs> <field> <v>   Stage a field value locally
                                              (run pdo-write to actually send it)

Drive test helpers (see ecat_cli.c: fsrt/nfsrt/nfspd/nrelays_set/relays_set/prtall):
  relays <val> [slave...]        Set 'relays' output on given slaves (default: all)
  nrelays <slave> <val>          Set 'relays' output on a single slave
  fsrt <rel_target>              Move every drive by rel_target (relative)
  nfsrt <slave> <rel_target>     Move a single drive by rel_target (relative)
  nfspd <slave|all> <speed>      Set Max_speed (clamped 0..1000) on a slave, or all
  prtall [iterations] [delay_s]  Print pos/target/diff/speed repeatedly

General:
  help / ?                       Show this help
  quit / exit                    Disconnect and exit
"""


class EcatCli:
    """Simple interactive REPL wrapping EcatClient + PdoImage."""

    def __init__(self, host: str, port: int):
        self.client = EcatClient(host, port)
        self.image = None  # PdoImage, once 'mapping' has succeeded

    # -- lifecycle -----------------------------------------------------

    def connect(self):
        self.client.connect()
        print(f"Connected to {self.client.host}:{self.client.port}")
        # The server auto-inits/scans/starts PDO on connect; try to fetch
        # the mapping right away so the image is ready to use immediately.
        try:
            self._refresh_mapping()
        except Exception as exc:
            print(f"(mapping not ready yet: {exc}; run 'mapping' once the bus is up)")

    def disconnect(self):
        self.client.disconnect()
        print("Disconnected.")

    def _refresh_mapping(self):
        mapping = self.client.pdo_mapping()
        self.image = PdoImage(mapping)
        self.image.update(self.client.pdo_read())
        print(f"PDO mapping loaded: {len(self.image.slaves)} slave(s)")

    def _require_image(self) -> PdoImage:
        if self.image is None:
            raise RuntimeError("No PDO mapping yet -- run 'mapping' first")
        return self.image

    # -- command handlers ------------------------------------------------

    def cmd_help(self, args):
        print(HELP_TEXT)

    def cmd_init(self, args):
        if not args:
            print("Usage: init <adapter>")
            return
        print(self.client.init(args[0]))

    def cmd_scan(self, args):
        print(self.client.scan())

    def cmd_state(self, args):
        if not args:
            print("Usage: state <target> [timeout_ms]")
            return
        timeout_ms = int(args[1]) if len(args) > 1 else 5000
        print(self.client.state(args[0], timeout_ms))

    def cmd_pdo_start(self, args):
        print(self.client.pdo_start())

    def cmd_pdo_stop(self, args):
        print(self.client.pdo_stop())

    def cmd_pdo_read(self, args):
        image = self._require_image()
        image.update(self.client.pdo_read())
        pprint(image.slaves)

    def cmd_pdo_write(self, args):
        image = self._require_image()
        print(self.client.pdo_write(image.bytes()))

    def cmd_mapping(self, args):
        self._refresh_mapping()

    def cmd_status(self, args):
        pprint(self.client.status())

    def cmd_cleanup(self, args):
        print(self.client.cleanup())
        self.image = None

    def cmd_shutdown(self, args):
        print(self.client.shutdown())

    def cmd_dump(self, args):
        pprint(self._require_image().slaves)

    def cmd_get(self, args):
        if len(args) < 3:
            print("Usage: get <slave> <inputs|outputs> <field>")
            return
        slave_id, direction, field = int(args[0]), args[1], args[2]
        image = self._require_image()
        try:
            print(image.slaves[slave_id][direction][field])
        except KeyError as exc:
            print(f"Error: no such {exc}")

    def cmd_set(self, args):
        if len(args) < 4:
            print("Usage: set <slave> <inputs|outputs> <field> <value>")
            return
        slave_id, direction, field, value = int(args[0]), args[1], args[2], args[3]
        image = self._require_image()
        try:
            bucket = image.slaves[slave_id][direction]
        except KeyError as exc:
            print(f"Error: no such {exc}")
            return
        if field not in bucket:
            print(f"Error: no field {field!r} on slave {slave_id} ({direction})")
            return
        bucket[field] = int(value, 0)

    def cmd_relays(self, args):
        if not args:
            print("Usage: relays <val> [slave...]")
            return
        val = int(args[0], 0)
        slave_ids = [int(a) for a in args[1:]] if len(args) > 1 else None
        relays_set(self.client, self._require_image(), val, slave_ids)

    def cmd_nrelays(self, args):
        if len(args) < 2:
            print("Usage: nrelays <slave> <val>")
            return
        nrelays_set(self.client, self._require_image(), int(args[0]), int(args[1], 0))

    def cmd_fsrt(self, args):
        if not args:
            print("Usage: fsrt <rel_target>")
            return
        fsrt(self.client, self._require_image(), int(args[0]))

    def cmd_nfsrt(self, args):
        if len(args) < 2:
            print("Usage: nfsrt <slave> <rel_target>")
            return
        nfsrt(self.client, self._require_image(), int(args[0]), int(args[1]))

    def cmd_nfspd(self, args):
        if len(args) < 2:
            print("Usage: nfspd <slave|all> <speed>")
            return
        slave_id = None if args[0] == "all" else int(args[0])
        nfspd(self.client, self._require_image(), slave_id, int(args[1]))

    def cmd_prtall(self, args):
        iterations = int(args[0]) if len(args) > 0 else 1
        delay = float(args[1]) if len(args) > 1 else 0.1
        prtall(self.client, self._require_image(), iterations, delay)

    # -- dispatch / REPL --------------------------------------------------

    def dispatch(self, name, args):
        handler = self.DISPATCH.get(name)
        if handler is None:
            print(f"Unknown command: {name!r} (try 'help')")
            return
        handler(self, args)

    def run(self):
        self.connect()
        try:
            while True:
                try:
                    line = input("ecat> ").strip()
                except (EOFError, KeyboardInterrupt):
                    print()
                    break
                if not line:
                    continue
                try:
                    parts = shlex.split(line)
                except ValueError as exc:
                    print(f"Parse error: {exc}")
                    continue
                name, args = parts[0].lower(), parts[1:]
                if name in ("quit", "exit"):
                    break
                try:
                    self.dispatch(name, args)
                except Exception as exc:
                    print(f"Error: {exc}")
        finally:
            self.disconnect()


EcatCli.DISPATCH = {
    "help": EcatCli.cmd_help,
    "?": EcatCli.cmd_help,
    "init": EcatCli.cmd_init,
    "scan": EcatCli.cmd_scan,
    "state": EcatCli.cmd_state,
    "pdo-start": EcatCli.cmd_pdo_start,
    "pdo-stop": EcatCli.cmd_pdo_stop,
    "pdo-read": EcatCli.cmd_pdo_read,
    "pdo-write": EcatCli.cmd_pdo_write,
    "mapping": EcatCli.cmd_mapping,
    "status": EcatCli.cmd_status,
    "cleanup": EcatCli.cmd_cleanup,
    "shutdown": EcatCli.cmd_shutdown,
    "dump": EcatCli.cmd_dump,
    "get": EcatCli.cmd_get,
    "set": EcatCli.cmd_set,
    "relays": EcatCli.cmd_relays,
    "nrelays": EcatCli.cmd_nrelays,
    "fsrt": EcatCli.cmd_fsrt,
    "nfsrt": EcatCli.cmd_nfsrt,
    "nfspd": EcatCli.cmd_nfspd,
    "prtall": EcatCli.cmd_prtall,
}


def main():
    parser = argparse.ArgumentParser(description="Simple CLI for ecat-server")
    parser.add_argument(
        "--host", default="127.0.0.1", help="ecat-server host (default: 127.0.0.1)"
    )
    parser.add_argument(
        "--port", type=int, default=20281, help="ecat-server port (default: 7777)"
    )
    args = parser.parse_args()

    EcatCli(args.host, args.port).run()


if __name__ == "__main__":
    main()
