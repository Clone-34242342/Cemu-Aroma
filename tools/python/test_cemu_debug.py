"""Protocol tests use a TCP peer, including fragmented and damaged packets.

Run: python3 -m unittest discover -s tools/python -v
Live validation is separate: these tests don't emulate Cemu's CPU scheduler.
"""
from contextlib import contextmanager
import socket
import threading
import time
import unittest

from cemu_debug import CemuDebug, DebugError, _decode, _frame, dispatch


class Peer:
    def __init__(self, commands):
        self.commands = commands
        self.error = None
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    @staticmethod
    def read(connection):
        def byte():
            data = connection.recv(1)
            if not data:
                raise EOFError("Client closed early")
            return data
        prefix = byte()
        while prefix == b"+":
            prefix = byte()
        if prefix == b"\x03":
            return "interrupt"
        if prefix != b"$":
            raise AssertionError(f"Unexpected prefix {prefix!r}")
        payload = b""
        while (c := byte()) != b"#":
            payload += c
        checksum = byte() + byte()
        assert int(checksum, 16) == sum(payload) & 255
        return _decode(payload).decode()

    def run(self):
        try:
            with self.listener, self.listener.accept()[0] as connection:
                connection.settimeout(2)
                for expected, response in self.commands:
                    actual = self.read(connection)
                    if expected != actual:
                        raise AssertionError(f"Expected {expected!r}, got {actual!r}")
                    if callable(response):
                        response(connection)
                    elif response is not None:
                        # Deliberately split both packet framing and payload across sends.
                        packet = b"+" + _frame(response.encode())
                        for start in range(0, len(packet), 7):
                            connection.sendall(packet[start:start + 7])
        except BaseException as error:
            self.error = error

    def join(self):
        self.thread.join(3)
        if self.thread.is_alive():
            raise AssertionError("Peer did not exit")
        if self.error:
            raise self.error


@contextmanager
def session(commands):
    peer = Peer([("qSupported:multiprocess-", "PacketSize=1000;cemu-python-debug+"),
                 ("interrupt", "T05thread:1000;"), ("Hg1000", "OK")]
                + commands + [("D", "OK")])
    try:
        with CemuDebug(peer.port, timeout=1) as client:
            yield client
    finally:
        peer.join()


class ClientTests(unittest.TestCase):
    def test_register_layout_and_unsigned_double_bits(self):
        values = list(range(32)) + [0xfff0000000000000 + i for i in range(32)]
        values += [0x0267766c, None, 0x20000000, 0x02123450, 4, 5, 6]
        packet = "".join("xxxxxxxx" if v is None else f"{v:0{16 if 32 <= i < 64 else 8}x}"
                         for i, v in enumerate(values))
        with session([("g", packet), ("p40", "0267766c"),
                      ("P20=fff0000000000000", "OK")]) as client:
            registers = client.registers()
            self.assertEqual(registers["f0"], 0xfff0000000000000)
            self.assertEqual(registers["pc"], 0x0267766c)
            self.assertIsNone(registers["msr"])
            self.assertEqual(client.register("pc"), 0x0267766c)
            client.register("f0", 0xfff0000000000000)

    def test_chunked_memory_and_big_endian(self):
        data = bytes(range(256)) * 5
        with session([("m1000,400", data[:1024].hex()), ("m1400,100", data[1024:].hex()),
                      ("m2000,4", "ef010203"), ("M2000,4:12345678", "OK"),
                      ("m2004,4", "3fc00000")]) as client:
            self.assertEqual(client.read(0x1000, len(data)), data)
            self.assertEqual(client.u32(0x2000), 0xef010203)
            client.u32(0x2000, 0x12345678)
            self.assertEqual(client.f32(0x2004), 1.5)

    def test_patch_restores_after_exception(self):
        with session([("m2000000,4", "60000000"), ("M2000000,4:4e800020", "OK"),
                      ("M2000000,4:60000000", "OK")]) as client:
            with self.assertRaisesRegex(RuntimeError, "experiment"):
                with client.patch(0x2000000, bytes.fromhex("4e800020"),
                                  expected=bytes.fromhex("60000000")):
                    raise RuntimeError("experiment failed")

    def test_resume_pause_step_and_breakpoint(self):
        commands = [("Z0,2000000,4", "OK"), ("vCont;c", None),
                    ("interrupt", "T05thread:1000;"), ("Hg1000", "OK"),
                    ("qC", "QC1000"), ("vCont;s:1000", "T05thread:1000;swbreak:;"),
                    ("Hg1000", "OK"), ("z0,2000000,4", "OK")]
        with session(commands) as client:
            client.breakpoint(0x2000000)
            client.resume()
            with self.assertRaises(DebugError):
                client.read(0, 4)
            client.pause()
            self.assertIn("swbreak", client.step())
            client.breakpoint(0x2000000, remove=True)

    def test_checksum_retransmit(self):
        def damaged(connection):
            connection.sendall(b"+$01020304#00")
            self.assertEqual(connection.recv(1), b"-")
            connection.sendall(_frame(b"01020304"))
        with session([("m1000,4", damaged)]) as client:
            self.assertEqual(client.u32(0x1000), 0x01020304)

    def test_partial_packet_survives_timeout(self):
        def slow(connection):
            packet = _frame(b"T05thread:1000;")
            connection.sendall(b"+" + packet[:8])
            time.sleep(0.1)
            connection.sendall(packet[8:])
        with session([("vCont;c", slow), ("Hg1000", "OK")]) as client:
            client.resume()
            with self.assertRaises(TimeoutError):
                client.wait_stop(0.02)
            self.assertIn("thread:1000", client.wait_stop(1))

    def test_threads_transfer(self):
        xml = '<threads><thread id="1234" name="Main &amp; Update" core="1"/></threads>'
        with session([("qXfer:threads:read::0,400", "l" + xml)]) as client:
            self.assertEqual(client.threads()[0], {"id": 0x1234, "name": "Main & Update", "core": "1"})

    def test_unicode_name_split_between_transfer_chunks(self):
        xml = '<threads><thread id="1234" name="ゲーム"/></threads>'.encode()
        split = xml.index('ゲ'.encode()) + 1
        def send_first(connection):
            connection.sendall(b"+" + _frame(b"m" + xml[:split]))
        def send_last(connection):
            connection.sendall(b"+" + _frame(b"l" + xml[split:]))
        with session([("qXfer:threads:read::0,400", send_first),
                      (f"qXfer:threads:read::{split:x},400", send_last)]) as client:
            self.assertEqual(client.threads()[0]["name"], "ゲーム")

    def test_errors_validation_and_allowlist(self):
        with session([("m0,4", "E01")]) as client:
            with self.assertRaises(DebugError):
                client.u32(0)
            for address, length in [(-1, 4), (0xffffffff, 2), (0, -1)]:
                with self.assertRaises(ValueError):
                    client.read(address, length)
            with self.assertRaises(ValueError):
                client.breakpoint(0x2000001)
            with self.assertRaises(ValueError):
                dispatch(client, {"op": "__getattribute__"})

    def test_escape_and_run_length(self):
        raw = b'$#}*'
        packet = _frame(raw)
        self.assertEqual(_decode(packet[1:packet.rindex(b"#")]), raw)
        self.assertEqual(_decode(b"0* "), b"0000")

    def test_failed_detach_preserves_original_exception(self):
        peer = Peer([("qSupported:multiprocess-", "cemu-python-debug+"),
                     ("interrupt", "T05thread:1000;"), ("Hg1000", "OK"),
                     ("D", None)])
        try:
            with self.assertRaisesRegex(RuntimeError, "original experiment failure"):
                with CemuDebug(peer.port, timeout=1):
                    raise RuntimeError("original experiment failure")
        finally:
            peer.join()

    def test_connect_waits_for_title_initialization(self):
        peer = Peer([("qSupported:multiprocess-", "cemu-python-debug+"),
                     ("interrupt", "E01"), ("interrupt", "T05thread:1000;"),
                     ("Hg1000", "OK"), ("D", "OK")])
        try:
            with CemuDebug(peer.port, timeout=1) as client:
                self.assertFalse(client.running)
        finally:
            peer.join()


if __name__ == "__main__":
    unittest.main()
