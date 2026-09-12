"""Run named STM32 I2C commands from this computer through the Jetson host."""
import argparse
from dataclasses import dataclass
import re
import shlex
import subprocess
from typing import Iterable


DEFAULT_HOST = "jetson"
DEFAULT_REMOTE_DIR = "/home/chen/stm32-spi-test"
DEFAULT_I2C_DEVICE = "/dev/i2c-7"
MAX_PAYLOAD = 248


@dataclass(frozen=True)
class CommandResponse:
    status: int
    data: bytes


class CommandError(RuntimeError):
    """The Jetson transport or the STM32 command did not complete successfully."""


def _validate_byte(value: int) -> int:
    if not isinstance(value, int) or not 0 <= value <= 0xFF:
        raise ValueError(f"payload values must be integers in 0..255, got {value!r}")
    return value


def _parse_response(output: str) -> CommandResponse:
    match = re.search(r"^status=(\d+) data\[(\d+)\]:(.*)$", output, re.MULTILINE)
    if not match:
        raise CommandError(f"Jetson returned an unrecognised response:\n{output.strip()}")
    status = int(match.group(1))
    expected_size = int(match.group(2))
    suffix = match.group(3).strip()
    try:
        data = bytes(int(value, 16) for value in suffix.split()) if suffix else b""
    except ValueError as error:
        raise CommandError(f"Jetson returned invalid response bytes: {suffix!r}") from error
    if len(data) != expected_size:
        raise CommandError(f"Jetson reported {expected_size} bytes but printed {len(data)}")
    return CommandResponse(status, data)


def sendCommand(cmdid: int, subid: int, payload: Iterable[int] = (), *,
                host: str = DEFAULT_HOST, remote_dir: str = DEFAULT_REMOTE_DIR,
                device: str = DEFAULT_I2C_DEVICE, timeout: float = 5.0) -> CommandResponse:
    """Send one protocol command via SSH and return its raw STM32 response."""
    cmdid = _validate_byte(cmdid)
    subid = _validate_byte(subid)
    data = [_validate_byte(value) for value in payload]
    if len(data) > MAX_PAYLOAD:
        raise ValueError(f"payload is limited to {MAX_PAYLOAD} bytes")
    remote_command = "cd {} && ./build/i2c_request {} 0x{:02X} 0x{:02X}".format(
        shlex.quote(remote_dir), shlex.quote(device), cmdid, subid)
    if data:
        remote_command += " " + " ".join(f"0x{value:02X}" for value in data)
    result = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", host, remote_command],
        text=True, capture_output=True, timeout=timeout, check=False)
    output = result.stdout.strip()
    if result.returncode not in (0, 3):
        detail = (output + "\n" + result.stderr.strip()).strip()
        raise CommandError(f"SSH/Jetson command failed with exit {result.returncode}: {detail}")
    response = _parse_response(output)
    if response.status != 0:
        raise CommandError(f"STM32 returned status=0x{response.status:02X}")
    return response


def getDeviceIdentity(**transport) -> str:
    """Return the fixed STM32 identity string from F0/00."""
    return sendCommand(0xF0, 0x00, **transport).data.decode("ascii")


def getDht11Temp(**transport) -> int:
    """Return DHT11 temperature in integer Celsius from F0/01."""
    data = sendCommand(0xF0, 0x01, **transport).data
    if len(data) != 1:
        raise CommandError(f"F0/01 expected 1 byte, got {len(data)}")
    return data[0]


def getArmCortexTemp(**transport) -> float:
    """Return STM32 internal sensor temperature in Celsius from F0/03 type 0."""
    data = sendCommand(0xF0, 0x03, [0], **transport).data
    if len(data) != 3 or data[0] != 0:
        raise CommandError(f"F0/03 type 0 expected [00 TEMP_LO TEMP_HI], got {data.hex(' ')}")
    return int.from_bytes(data[1:], byteorder="little", signed=True) / 100.0


def getLightLevel(**transport) -> int:
    """Return board LS1 relative light level (0..100) from F0/03 type 1."""
    data = sendCommand(0xF0, 0x03, [1], **transport).data
    if len(data) != 2 or data[0] != 1:
        raise CommandError(f"F0/03 type 1 expected [01 LEVEL], got {data.hex(' ')}")
    return data[1]


def echo(payload: Iterable[int], **transport) -> bytes:
    """Send arbitrary bytes to the F0-independent echo command (01/00)."""
    return sendCommand(0x01, 0x00, payload, **transport).data


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--remote-dir", default=DEFAULT_REMOTE_DIR)
    parser.add_argument("--device", default=DEFAULT_I2C_DEVICE)
    parser.add_argument("--timeout", type=float, default=5.0)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("identity")
    commands.add_parser("dht11-temp")
    commands.add_parser("arm-cortex-temp")
    commands.add_parser("light")
    echo_parser = commands.add_parser("echo")
    echo_parser.add_argument("bytes", nargs="+", type=lambda text: int(text, 0))
    raw_parser = commands.add_parser("raw")
    raw_parser.add_argument("cmdid", type=lambda text: int(text, 0))
    raw_parser.add_argument("subid", type=lambda text: int(text, 0))
    raw_parser.add_argument("bytes", nargs="*", type=lambda text: int(text, 0))
    args = parser.parse_args()
    transport = {
        "host": args.host,
        "remote_dir": args.remote_dir,
        "device": args.device,
        "timeout": args.timeout,
    }
    try:
        if args.command == "identity":
            print(getDeviceIdentity(**transport))
        elif args.command == "dht11-temp":
            print(f"{getDht11Temp(**transport)} C")
        elif args.command == "arm-cortex-temp":
            print(f"{getArmCortexTemp(**transport):.2f} C")
        elif args.command == "light":
            print(getLightLevel(**transport))
        elif args.command == "echo":
            print(echo(args.bytes, **transport).hex(" ").upper())
        else:
            response = sendCommand(args.cmdid, args.subid, args.bytes, **transport)
            print(response.data.hex(" ").upper())
    except (CommandError, OSError, subprocess.TimeoutExpired, ValueError) as error:
        raise SystemExit(f"error: {error}")


if __name__ == "__main__":
    main()
