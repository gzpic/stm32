"""Verify the exact I2C command bytes captured by STM32 through CMSIS-DAP."""
import argparse
import re
import subprocess
import time
from pathlib import Path


def crc16(data):
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc & 0xFFFF


def command_frame(cmd, subcmd, payload):
    total = len(payload) + 8
    frame = [0x30, cmd, total & 0xFF, total >> 8, subcmd, *payload]
    checksum = crc16(frame)
    return frame + [checksum & 0xFF, checksum >> 8, 0x0A]


def response_frame(status, data):
    frame = [0x60, status, *data]
    checksum = crc16(frame)
    return frame + [checksum & 0xFF, checksum >> 8, 0x0A]


def command_rx_address(map_path):
    match = re.search(r"^\s*command_rx\s+0x([0-9a-fA-F]+)",
                      map_path.read_text(encoding="utf-8"), re.MULTILINE)
    if not match:
        raise RuntimeError("command_rx symbol was not found in the map file")
    return int(match.group(1), 16)


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, capture_output=True, **kwargs)


def read_target_bytes(probe_id, address, size):
    output = run([
        "pyocd", "commander", "-t", "stm32f407zgtx", "-u", probe_id,
        "-f", "100000", "-c", f"rb 0x{address:08x} {size}", "-c", "exit",
    ]).stdout
    captured = []
    for line in output.splitlines():
        match = re.match(r"^[0-9a-fA-F]{8}:\s+([^|]+)", line)
        if match:
            captured.extend(int(value, 16) for value in match.group(1).split())
    if len(captured) != size:
        raise RuntimeError(f"DAP returned {len(captured)} bytes, expected {size}\n{output}")
    return captured


def send_frame(host, frame):
    transfer = ["i2ctransfer", "-y", "7", f"w{len(frame)}@0x42"]
    transfer.extend(f"0x{value:02x}" for value in frame)
    run(["ssh", "-o", "BatchMode=yes", host, *transfer])


def consume_response(host):
    deadline = time.monotonic() + 1.0
    while True:
        output = run([
            "ssh", "-o", "BatchMode=yes", host,
            "i2ctransfer", "-y", "7", "r256@0x42",
        ]).stdout.split()
        values = [int(value, 16) for value in output]
        if len(values) == 256 and values[:2] == [0x60, 0x06]:
            if time.monotonic() >= deadline:
                raise RuntimeError("STM32 remained BUSY for more than one second")
            time.sleep(0.001)
            continue
        if len(values) != 256:
            raise RuntimeError(f"STM32 returned {len(values)} bytes, expected 256")
        return values


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="jetson")
    parser.add_argument("--probe", default="ATK-03262021")
    parser.add_argument("--map", type=Path,
                        default=Path("stm32/Output/i2c_slave.map"))
    args = parser.parse_args()
    address = command_rx_address(args.map)
    cases = [
        ("empty", []),
        ("special-bytes", [0x30, 0xFF, 0x0A, 0x00, 0x55]),
        ("maximum", list(range(248))),
    ]
    for name, payload in cases:
        frame = command_frame(1, 0, payload)
        send_frame(args.host, frame)
        captured = read_target_bytes(args.probe, address, len(frame))
        if captured != frame:
            raise RuntimeError(
                f"{name}: expected {' '.join(f'{b:02X}' for b in frame)}, "
                f"got {' '.join(f'{b:02X}' for b in captured)}")
        received = consume_response(args.host)
        expected_reply = response_frame(0, payload)
        if received != expected_reply + [0xFF] * (256 - len(expected_reply)):
            raise RuntimeError(
                f"{name}: response bytes differ from expected frame")
        print(f"PASS {name}: write={len(frame)} bytes read={len(expected_reply)} bytes")


if __name__ == "__main__":
    main()
