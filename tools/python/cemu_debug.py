#!/usr/bin/env python3
"""Python 3.10+ client for this fork's loopback PowerPC debugger.

Use CemuDebug as a context manager, or run this file with --json for an
agent-friendly, persistent JSON-lines session. No third-party packages needed.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import socket
import struct
import sys
import time
import xml.etree.ElementTree as ET


class DebugError(RuntimeError):
    pass


class TitleNotReady(DebugError):
    pass


REGISTER_NAMES = ([f"r{i}" for i in range(32)] + [f"f{i}" for i in range(32)]
                  + ["pc", "msr", "cr", "lr", "ctr", "xer", "fpscr"])
REGISTER_IDS = {name: i for i, name in enumerate(REGISTER_NAMES)}


def _frame(payload: bytes) -> bytes:
    escaped = bytearray()
    for c in payload:
        if c in b"$#}*":
            escaped.extend((ord("}"), c ^ 0x20))
        else:
            escaped.append(c)
    return b"$" + escaped + f"#{sum(escaped) & 255:02x}".encode()


def _decode(payload: bytes) -> bytes:
    output = bytearray()
    i = 0
    while i < len(payload):
        c = payload[i]
        i += 1
        if c == ord("}"):
            if i == len(payload):
                raise DebugError("Truncated packet escape")
            output.append(payload[i] ^ 0x20)
            i += 1
        elif c == ord("*"):
            if not output or i == len(payload) or payload[i] < 29:
                raise DebugError("Invalid packet run length")
            output.extend([output[-1]] * (payload[i] - 29))
            i += 1
        else:
            output.append(c)
    return bytes(output)


class CemuDebug:
    """One synchronous debugger session. Addresses and integers are guest big endian.

    Connecting pauses the title; leaving the context removes this session's
    breakpoints and resumes it. Keep pauses short when testing online.
    """

    def __init__(self, port: int = 1337, timeout: float = 10.0):
        self.port = port
        self.timeout = timeout
        self.socket: socket.socket | None = None
        self._buffer = bytearray()
        self._last_frame = b""
        self.running = False
        self.stop: str | None = None

    def connect(self):
        if self.socket is not None:
            raise DebugError("Already connected")
        self.socket = socket.create_connection(("127.0.0.1", self.port), self.timeout)
        self.socket.settimeout(self.timeout)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            features = self._request("qSupported:multiprocess-")
            if "cemu-python-debug+" not in features:
                raise DebugError("This client needs the cemu-python-debug fork")
            self.running = True
            deadline = time.monotonic() + self.timeout
            while True:
                try:
                    self.pause()
                    return self
                except TitleNotReady:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.02)
        except BaseException:
            self.socket.close()
            self.socket = None
            raise

    def __enter__(self):
        return self.connect()

    def __exit__(self, exception_type, exception, traceback):
        try:
            self.close()
        except (OSError, DebugError):
            if exception_type is None:
                raise

    def _send(self, payload: str):
        if self.socket is None:
            raise DebugError("Not connected")
        self._last_frame = _frame(payload.encode("ascii"))
        self.socket.sendall(self._last_frame)

    def _receive(self) -> str:
        """Retain incomplete packets across timeouts; check checksum before decoding."""
        if self.socket is None:
            raise DebugError("Not connected")
        retries = 0
        while True:
            while self._buffer and self._buffer[0] != ord("$"):
                prefix = self._buffer.pop(0)
                if prefix == ord("-"):
                    retries += 1
                    if retries > 3:
                        raise DebugError("Server rejected packet checksum repeatedly")
                    self.socket.sendall(self._last_frame)
            end = self._buffer.find(b"#", 1)
            if end >= 0 and len(self._buffer) >= end + 3:
                payload = bytes(self._buffer[1:end])
                checksum = bytes(self._buffer[end + 1:end + 3])
                del self._buffer[:end + 3]
                try:
                    valid = int(checksum, 16) == sum(payload) & 255
                except ValueError:
                    valid = False
                if not valid:
                    self.socket.sendall(b"-")
                    continue
                self.socket.sendall(b"+")
                # Keep a one-character-per-byte representation until xfer chunks
                # are joined: UTF-8 names can split across packet boundaries.
                return _decode(payload).decode("latin1")
            data = self.socket.recv(8192)
            if not data:
                raise DebugError("Cemu closed the debugger connection")
            self._buffer.extend(data)
            if len(self._buffer) > 1_048_576:
                raise DebugError("Debugger packet exceeded 1 MiB")

    def _request(self, payload: str, *, ok: bool = False) -> str:
        self._send(payload)
        response = self._receive()
        # A breakpoint can race Ctrl-C after an interrupted wait_stop(). Both
        # stop replies may precede the response to this ordinary RSP request.
        # Consume those notifications so an Hg/OK exchange stays synchronized.
        for _ in range(16):
            if len(response) < 3 or response[0] not in "TS":
                break
            try:
                int(response[1:3], 16)
            except ValueError:
                break
            self.stop = response
            self.running = False
            response = self._receive()
        else:
            raise DebugError("Too many stop notifications before command response")
        if (len(response) == 3 and response.startswith("E")) or (ok and response != "OK"):
            raise DebugError(f"{payload[:40]}: {response or 'unsupported'}")
        return response

    def _paused(self):
        if self.running:
            raise DebugError("Pause the title before inspecting or editing its state")

    def wait_stop(self, timeout: float | None = None) -> str:
        if self.socket is None:
            raise DebugError("Not connected")
        previous = self.socket.gettimeout()
        self.socket.settimeout(self.timeout if timeout is None else timeout)
        try:
            response = self._receive()
            if response == "E01":
                raise TitleNotReady("Title has no guest thread yet")
            if not response.startswith(("T", "S", "W", "X")):
                raise DebugError(f"Expected stop reply, received {response!r}")
            self.stop = response
            self.running = False
            if "thread:" in response:
                thread_id = int(response.split("thread:", 1)[1].split(";", 1)[0], 16)
                self.select_thread(thread_id)
            return response
        finally:
            self.socket.settimeout(previous)

    def pause(self) -> str:
        if self.socket is None:
            raise DebugError("Not connected")
        if not self.running and self.stop is not None:
            return self.stop
        self.socket.sendall(b"\x03")
        return self.wait_stop()

    def resume(self):
        self._paused()
        self._send("vCont;c")
        self.running = True
        self.stop = None

    def step(self, thread_id: int | None = None, timeout: float | None = None) -> str:
        self._paused()
        if thread_id is None:
            thread_id = int(self._request("qC")[2:], 16)
        self._send(f"vCont;s:{thread_id:x}")
        self.running = True
        self.stop = None
        return self.wait_stop(timeout)

    def close(self):
        if self.socket is None:
            return
        try:
            if self.running:
                self.pause()
            self._request("D", ok=True)
        finally:
            self.socket.close()
            self.socket = None
            self._buffer.clear()

    def threads(self) -> list[dict]:
        self._paused()
        xml = self._xfer("threads", "")
        return [dict(node.attrib, id=int(node.attrib["id"], 16))
                for node in ET.fromstring(xml).findall("thread")]

    def _xfer(self, kind: str, annex: str) -> str:
        output = ""
        while True:
            response = self._request(f"qXfer:{kind}:read:{annex}:{len(output):x},400")
            if not response or response[0] not in "ml":
                raise DebugError(f"Unsupported {kind} transfer")
            output += response[1:]
            if response[0] == "l":
                return output.encode("latin1").decode("utf-8")
            if len(response) == 1:
                raise DebugError("Empty intermediate transfer")

    def modules(self) -> list[dict]:
        self._paused()
        root = ET.fromstring(self._xfer("libraries", ""))
        return [{"name": n.attrib["name"], "address": int(n.find("segment").attrib["address"], 0)}
                for n in root.findall("library")]

    def select_thread(self, thread_id: int):
        self._paused()
        if thread_id <= 0:
            raise ValueError("Choose a concrete thread ID from threads()")
        self._request(f"Hg{thread_id:x}", ok=True)

    def registers(self) -> dict[str, int | None]:
        self._paused()
        data = self._request("g")
        if len(data) != 824:
            raise DebugError(f"Unexpected register packet length: {len(data)}")
        output = {}
        offset = 0
        for i, name in enumerate(REGISTER_NAMES):
            width = 16 if 32 <= i < 64 else 8
            value = data[offset:offset + width]
            output[name] = None if "x" in value else int(value, 16)
            offset += width
        return output

    def register(self, name: str, value: int | None = None) -> int | None:
        self._paused()
        index = REGISTER_IDS[name.lower()]
        width = 8 if 32 <= index < 64 else 4
        if value is None:
            result = self._request(f"p{index:x}")
            if len(result) != width * 2:
                raise DebugError("Register unavailable")
            return None if "x" in result else int(result, 16)
        if not 0 <= value < 1 << (width * 8):
            raise ValueError("Register value out of range")
        self._request(f"P{index:x}={value:0{width * 2}x}", ok=True)
        return value

    @staticmethod
    def _range(address: int, length: int):
        if address < 0 or length < 0 or address + length > 1 << 32:
            raise ValueError("Guest memory range exceeds 32-bit address space")

    def read(self, address: int, length: int) -> bytes:
        self._paused()
        self._range(address, length)
        output = bytearray()
        while len(output) < length:
            count = min(1024, length - len(output))
            data = bytes.fromhex(self._request(f"m{address + len(output):x},{count:x}"))
            if len(data) != count:
                raise DebugError("Short memory read")
            output.extend(data)
        return bytes(output)

    def write(self, address: int, data: bytes):
        self._paused()
        self._range(address, len(data))
        for offset in range(0, len(data), 1024):
            chunk = data[offset:offset + 1024]
            self._request(f"M{address + offset:x},{len(chunk):x}:{chunk.hex()}", ok=True)

    def u32(self, address: int, value: int | None = None) -> int:
        if value is None:
            return struct.unpack(">I", self.read(address, 4))[0]
        self.write(address, struct.pack(">I", value))
        return value

    def f32(self, address: int, value: float | None = None) -> float:
        if value is None:
            return struct.unpack(">f", self.read(address, 4))[0]
        self.write(address, struct.pack(">f", value))
        return value

    def breakpoint(self, address: int, *, remove: bool = False, kind: int = 0, size: int = 4):
        self._paused()
        self._range(address, size)
        if kind not in range(5) or address % 4 or size != 4:
            raise ValueError("Cemu breakpoints require an aligned 4-byte range")
        self._request(f"{'z' if remove else 'Z'}{kind},{address:x},{size:x}", ok=True)

    @contextmanager
    def patch(self, address: int, data: bytes, *, expected: bytes | None = None):
        """Temporarily patch memory; restore it even if the experiment fails."""
        original = self.read(address, len(data))
        if expected is not None and original != expected:
            raise DebugError(f"Original bytes at {address:#x} do not match this experiment")
        try:
            self.write(address, data)
            yield original
        finally:
            if self.running:
                self.pause()
            self.write(address, original)


def dispatch(client: CemuDebug, request: dict):
    """Deliberate method allowlist: the JSON interface does not evaluate Python."""
    op = request["op"]
    args = request.get("args", {})
    if op == "read":
        return client.read(**args).hex()
    if op == "write":
        return client.write(args["address"], bytes.fromhex(args["hex"]))
    if op not in {"pause", "resume", "wait_stop", "step", "threads", "modules",
                  "select_thread", "registers", "register", "u32", "f32", "breakpoint"}:
        raise ValueError(f"Unknown operation: {op}")
    return getattr(client, op)(**args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--json", action="store_true", help="Persistent JSON-lines session on stdin/stdout")
    options = parser.parse_args()
    with CemuDebug(options.port, options.timeout) as client:
        if not options.json:
            print(json.dumps({"stop": client.stop, "threads": client.threads(), "registers": client.registers()}, indent=2))
            return
        for line in sys.stdin:
            request = {}
            try:
                request = json.loads(line)
                result = dispatch(client, request)
                reply = {"id": request.get("id"), "result": result}
            except Exception as error:
                reply = {"id": request.get("id") if isinstance(request, dict) else None,
                         "error": f"{type(error).__name__}: {error}"}
            print(json.dumps(reply), flush=True)


if __name__ == "__main__":
    main()
