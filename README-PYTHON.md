# Cemu Python debugging

Based on upstream Cemu `main` at `3b1031612eb21302a8032e2f7a6db31549bd269b`.
Build Cemu using [BUILD.md](BUILD.md), then launch:

```sh
./bin/Cemu_debug --python-debug -g /path/to/game.rpx
```

Use the executable name produced by your build. The game initially stops at its
entry point. No PPC debugger window is opened. This option does not change
accounts, saves, graphics packs, or online settings.

Python 3.10+ can control Cemu through the loopback debugger, normally port 1337.
No Python packages or embedded Python runtime are required:

```sh
PYTHONPATH=tools/python python3 my_experiment.py
```

```python
from cemu_debug import CemuDebug

with CemuDebug() as debug:
    print(debug.threads())
    print(debug.modules())
    print(debug.registers())
    pc = debug.register("pc")
    print(debug.read(pc, 16).hex())
    # debug.u32(address, value)       # big-endian integer write
    # debug.f32(address, value)       # big-endian float write
    # debug.register("r3", value)
    # debug.breakpoint(address)
    # debug.resume(); debug.wait_stop()
    # debug.step()
# Detaches, removes breakpoints, and resumes the title.
```

For reversible code experiments, `debug.patch(address, replacement_bytes,
expected=original_bytes)` is a context manager. It checks the original bytes,
applies the patch, and restores them on exit, including exceptions. Code writes
invalidate Cemu's recompiled code cache. Pause before reading or writing state;
`resume()` and `pause()` control execution. Online servers continue running while
the local game is paused, so use brief pauses during online research.

For agent tools, use a persistent JSON-lines process:

```sh
python3 tools/python/cemu_debug.py --json
```

```json
{"id":1,"op":"read","args":{"address":33554432,"length":16}}
{"id":2,"op":"registers"}
{"id":3,"op":"resume"}
{"id":4,"op":"pause"}
```

Each request returns its `id` and a `result` or `error`. Memory is hex encoded.
Closing stdin detaches and resumes. Python scripts can use the full API directly;
JSON dispatch exposes only the documented debugger methods.

The native debugger fixes include complete register packets, unsigned floating
register writes, breakpoint byte visibility and restoration, instruction-cache
invalidation across disjoint translated ranges, checksummed fragmented TCP input, disconnect cleanup, and resuming
only thread suspensions owned by the debugger.

Stepping uses Cemu's software breakpoint mechanism. Self-branching instructions
and concurrent multicore execution need care; this is not cycle-accurate stepping.
MSR is unavailable. Hardware memory watchpoints are inherited from upstream,
limited to one aligned four-byte range on x86, and depend on host ptrace support.
Use `--force-interpreter` when you need more accurate memory-watchpoint locations;
the recompiler may report the surrounding dispatch code.
No graphics, frame-rate, online-idle, or CPU-driver modifications are enabled by
this fork itself.

Run client protocol tests:

```sh
python3 -m unittest discover -s tools/python -v
```

These tests cover the wire protocol and client behavior. They do not replace
live checks against a compiled Cemu and a running title.

Live checks passed on Linux with MK8 US v81 and the multi-core recompiler:
register round-trips, executing and restoring a temporary instruction, stepping,
repeated software breakpoints, 20 consecutive hardware watchpoint hits,
detach/reconnect, interrupting with a breakpoint stop already pending, fragmented
packets, and invalid requests. Standard GDB also
connects. Alternating native code hooks were checked against the executing
physics timestep. A core can finish an already-running translated block after a
write, so wait for the changed code to execute before taking measurements.
Other operating systems have not been tested.
