#!/usr/bin/env python3
"""Live debugger checks. Start an offline title stopped at its entry point.

This briefly changes and restores a register, floating register, and instruction
bytes while paused, then single-steps. --breakpoint additionally tests a known
recurring function address three times. It does not change online settings.
"""
import argparse
import json
import time

from cemu_debug import CemuDebug


def check(client, report):
    registers = client.registers()
    pc = registers["pc"]
    assert pc is not None and pc % 4 == 0, f"Invalid instruction pointer {pc!r}"
    assert client.register("pc") == pc, "Bulk and individual register reads disagree"
    report["pc"] = hex(pc)
    report["threads"] = len(client.threads())
    report["modules"] = client.modules()
    for name, temporary in [("r0", 0xa5127e49), ("f31", 0xfff0000000000000)]:
        original = client.register(name)
        try:
            client.register(name, temporary)
            assert client.register(name) == temporary, f"{name} write failed"
        finally:
            client.register(name, original)
        assert client.register(name) == original
    report["register_write_and_restore"] = True
    packet = client._request("g")
    client._request("G" + packet, ok=True)
    assert client._request("g") == packet, "Full register packet did not round-trip"
    report["full_register_packet_roundtrip"] = True
    original = client.read(pc, 4)
    # Test a write starting inside an instruction, including breakpoint shadow bytes.
    replacement = bytes([original[1] ^ 1])
    with client.patch(pc + 1, replacement, expected=original[1:2]):
        assert client.read(pc, 4) == original[:1] + replacement + original[2:]
    assert client.read(pc, 4) == original
    report["unaligned_code_patch_and_restore"] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--breakpoint", type=lambda value: int(value, 0))
    parser.add_argument("--skip-step", action="store_true")
    args = parser.parse_args()
    report = {}
    try:
        with CemuDebug(args.port) as client:
            check(client, report)
            if not args.skip_step:
                before = client.register("pc")
                client.step()
                after = client.register("pc")
                assert before != after, "Step did not advance (self-branching instruction?)"
                report["single_step"] = [hex(before), hex(after)]
            if args.breakpoint is not None:
                address = args.breakpoint
                instruction = client.read(address, 4)
                client.breakpoint(address)
                assert client.read(address, 4) == instruction, "Breakpoint hid original bytes"
                for _ in range(3):
                    client.resume()
                    client.wait_stop(20)
                    assert client.register("pc") == address, "Stopped at an unexpected instruction"
                report["persistent_breakpoint_hits"] = 3
                check(client, report)
                client.breakpoint(address, remove=True)
                assert client.read(address, 4) == instruction
        report["detach"] = True
        # Repeat attach/detach to check that the server accepts another client.
        time.sleep(0.2)
        with CemuDebug(args.port) as client:
            assert client.register("pc") is not None
        report["reattach"] = True
    finally:
        print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    main()
