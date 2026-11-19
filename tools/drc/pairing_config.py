#!/usr/bin/env python3
"""Generate a console-side GamePad pairing profile for DRC-patched hostapd."""
import argparse
import os
import re
import secrets
import struct
from pathlib import Path


def attribute(kind, value):
    return struct.pack(">HH", kind, len(value)) + value


def profile(mac, psk):
    suffix = mac.hex()
    normal = f"WiiU{suffix}"
    pairing = f"WiiU{suffix[:-1]}{suffix}_STA1"
    symbols = "".join(str((mac[-1] >> shift) & 3) for shift in (6, 4, 2, 0))
    credential = b"".join((
        attribute(0x1026, b"\x01"),
        attribute(0x1045, normal.encode()),
        attribute(0x1003, b"\x00\x20"),
        attribute(0x100F, b"\x00\x08"),
        attribute(0x1027, psk.encode()),
        attribute(0x1020, mac),
    ))
    return normal, pairing, symbols, attribute(0x100E, credential)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("interface")
    parser.add_argument("ap_mac")
    parser.add_argument("directory", type=Path)
    parser.add_argument("--channel", type=int, default=36)
    args = parser.parse_args()
    if not re.fullmatch(r"[a-zA-Z0-9_.-]{1,15}", args.interface):
        parser.error("invalid interface name")
    if not re.fullmatch(r"(?:[a-fA-F0-9]{2}:){5}[a-fA-F0-9]{2}", args.ap_mac):
        parser.error("expected a colon-separated MAC address")
    if args.channel not in (36, 40, 44, 48, 149, 153, 157, 161):
        parser.error("select a supported non-DFS 5 GHz channel")
    mac = bytes.fromhex(args.ap_mac.replace(":", ""))
    if not any(mac) or mac[0] & 1:
        parser.error("the access point needs a unicast MAC address")
    directory = args.directory.resolve()
    if any(character.isspace() for character in str(directory)):
        parser.error("profile directory must contain no whitespace")
    os.umask(0o077)
    directory.mkdir(mode=0o700)
    psk = secrets.token_hex(32)
    normal, pairing, symbols, credential = profile(mac, psk)
    (directory / "credential.bin").write_bytes(credential)
    common = (
        f"interface={args.interface}\ndriver=nl80211\n"
        f"hw_mode=a\nchannel={args.channel}\nieee80211n=1\nwmm_enabled=1\n"
        f"ctrl_interface={directory}/ctrl\n"
    )
    (directory / "pairing.conf").write_text(
        common + f"ssid={pairing}\nieee8021x=1\neap_server=1\neapol_version=1\n"
        f"wps_state=2\ndevice_name={normal}\nmanufacturer=Broadcom\n"
        f"device_type=6-a4c0e1f4-1\nconfig_methods=label\n"
        f"skip_cred_build=1\nextra_cred={directory}/credential.bin\nwps_cred_processing=1\n"
    )
    (directory / "runtime.conf").write_text(
        common + f"ssid={normal}\nignore_broadcast_ssid=1\nwpa=2\n"
        f"wpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\nwpa_psk={psk}\n"
    )
    print(symbols)


if __name__ == "__main__":
    main()
