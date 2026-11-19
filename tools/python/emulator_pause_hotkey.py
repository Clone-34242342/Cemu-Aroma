#!/usr/bin/env python3
"""X11 F4 shortcut for emulator builds with a SIGUSR2 pause handler."""
import argparse
import os
from pathlib import Path
import select
import signal
import time

from Xlib import X, XK, display, error


def targets(display_conn, root, executables, sig=signal.SIGUSR2):
    windows = root.get_full_property(display_conn.intern_atom("_NET_CLIENT_LIST"), X.AnyPropertyType)
    if windows is None:
        return set()
    pid_atom = display_conn.intern_atom("_NET_WM_PID")
    handlers = 1 << (sig - 1)
    result = set()
    for wid in windows.value:
        try:
            prop = display_conn.create_resource_object("window", wid).get_full_property(
                pid_atom, X.AnyPropertyType
            )
            if not prop or not len(prop.value):
                continue
            pid = int(prop.value[0])
            proc = Path("/proc") / str(pid)
            if proc.stat().st_uid != os.getuid() or Path(os.readlink(proc / "exe")) not in executables:
                continue
            caught = next(line.split()[1] for line in (proc / "status").read_text().splitlines()
                          if line.startswith("SigCgt:"))
            if int(caught, 16) & handlers:
                result.add(pid)
        except (OSError, StopIteration, error.BadWindow):
            continue
    return result


def signal_targets(pids, display_conn, root, executables, sig=signal.SIGUSR2):
    sent = 0
    for pid in pids:
        try:
            fd = os.pidfd_open(pid)
            try:
                if pid in targets(display_conn, root, executables, sig):
                    signal.pidfd_send_signal(fd, sig)
                    sent += 1
            finally:
                os.close(fd)
        except ProcessLookupError:
            continue
    return sent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("emulator", type=Path, nargs="+")
    parser.add_argument("--resume", action="store_true", help="Resume matching emulators once, then exit")
    args = parser.parse_args()
    executables = {path.resolve() for path in args.emulator}
    conn = display.Display()
    root = conn.screen().root
    if args.resume:
        active = targets(conn, root, executables, signal.SIGUSR1)
        print(f"Resume requested for {signal_targets(active, conn, root, executables, signal.SIGUSR1)} emulator(s).")
        return

    key = conn.keysym_to_keycode(XK.string_to_keysym("F4"))
    lock_keys = {conn.keysym_to_keycode(XK.string_to_keysym(name))
                 for name in ("Caps_Lock", "Num_Lock", "Scroll_Lock")}
    modifiers = [0]
    for index, keys in enumerate(conn.get_modifier_mapping()):
        if set(keys).intersection(lock_keys):
            modifiers += [mask | (1 << index) for mask in modifiers]

    grabbed = False
    pressed = False
    released_at = None
    while True:
        active = targets(conn, root, executables)
        if bool(active) != grabbed:
            if active:
                failures = []
                for mod in modifiers:
                    root.grab_key(key, mod, False, X.GrabModeAsync, X.GrabModeAsync,
                                  onerror=lambda *args: failures.append(args))
                conn.sync()
                if failures:
                    raise RuntimeError("Another application has already registered global F4")
            else:
                for mod in modifiers:
                    root.ungrab_key(key, mod)
                conn.sync()
                pressed = False
                released_at = None
            grabbed = bool(active)

        if not conn.pending_events():
            select.select([conn.fileno()], [], [], 0.04 if released_at is not None else 0.25)
        while conn.pending_events():
            event = conn.next_event()
            if event.type == X.KeyRelease and event.detail == key:
                released_at = time.monotonic()
            elif event.type == X.KeyPress and event.detail == key:
                if not pressed:
                    signal_targets(active, conn, root, executables)
                pressed = True
                released_at = None
        if released_at is not None and time.monotonic() - released_at >= 0.025:
            pressed = False
            released_at = None


if __name__ == "__main__":
    main()
