#!/usr/bin/env python3
"""
wol.py — wake fleet boxes on demand (Wake-on-LAN magic packets).

Runs on an always-on box (the 13700K — homeserver + Integrator). Sends a magic
packet to a sleeping/off box by NAME (from fleet_boxes.json) or raw MAC, so the
forge/render boxes can sleep to save power and wake only when there's a gen job.

Usage:
    python wol.py 14900k                 # wake by fleet name
    python wol.py all                    # wake every box in the registry
    python wol.py --mac AA:BB:CC:DD:EE:FF # wake a raw MAC
    python wol.py --list                 # show the registry

Requires: the target's MAC in fleet_boxes.json AND WoL enabled on that box
(BIOS "Power On by PCI-E/WOL" + Windows NIC "Wake on Magic Packet" — see
WOL_SETUP.md). Broadcasts to the LAN so all boxes must share the subnet.
"""

import argparse
import json
import socket
import struct
import sys
import time
from pathlib import Path

REGISTRY = Path(__file__).with_name("fleet_boxes.json")
BROADCAST = "255.255.255.255"
PORTS = (9, 7)  # discard + echo; some NICs listen on one or the other


def load_registry() -> dict:
    if REGISTRY.exists():
        return json.loads(REGISTRY.read_text(encoding="utf-8"))
    return {"boxes": {}}


def magic_packet(mac: str) -> bytes:
    """6x 0xFF followed by the 6-byte MAC repeated 16 times."""
    clean = mac.replace(":", "").replace("-", "").replace(".", "").strip()
    if len(clean) != 12:
        raise ValueError(f"bad MAC: {mac!r}")
    mac_bytes = bytes.fromhex(clean)
    return b"\xff" * 6 + mac_bytes * 16


def wake_mac(mac: str, label: str = "") -> None:
    pkt = magic_packet(mac)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    for port in PORTS:
        s.sendto(pkt, (BROADCAST, port))
    s.close()
    print(f"  ⚡ magic packet -> {label or mac} ({mac})")


def main() -> None:
    ap = argparse.ArgumentParser(description="Wake fleet boxes (Wake-on-LAN).")
    ap.add_argument("target", nargs="?", help="fleet box name, or 'all'")
    ap.add_argument("--mac", help="wake a raw MAC address directly")
    ap.add_argument("--list", action="store_true", help="show the box registry")
    args = ap.parse_args()

    reg = load_registry()
    boxes = reg.get("boxes", {})

    if args.list:
        print("  fleet box registry:")
        for name, info in boxes.items():
            print(f"    {name:12} MAC={info.get('mac','?'):18} IP={info.get('ip','?'):15} GPU={info.get('gpu','?')}")
        if not boxes:
            print("    (empty — add boxes to fleet_boxes.json)")
        return

    if args.mac:
        wake_mac(args.mac)
        return

    if not args.target:
        ap.error("give a box name, 'all', or --mac")

    if args.target == "all":
        if not boxes:
            sys.exit("registry is empty — add boxes to fleet_boxes.json first")
        for name, info in boxes.items():
            mac = info.get("mac")
            if mac:
                wake_mac(mac, name)
                time.sleep(0.2)
            else:
                print(f"  ⚠ {name}: no MAC in registry, skipped")
        return

    info = boxes.get(args.target)
    if not info:
        sys.exit(f"unknown box '{args.target}'. Known: {', '.join(boxes) or '(none)'}")
    mac = info.get("mac")
    if not mac:
        sys.exit(f"'{args.target}' has no MAC in the registry yet — add it to fleet_boxes.json")
    wake_mac(mac, args.target)
    print(f"  sent. '{args.target}' should POST in ~15-60s if WoL is enabled in its BIOS + NIC.")


if __name__ == "__main__":
    main()
