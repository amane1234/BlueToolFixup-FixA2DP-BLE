"""Check the six Tahoe advsets signatures against an x86_64 bluetoothd slice."""
import argparse
from pathlib import Path
import re
import struct
import uuid


def take(fmt, data, offset, end):
    if offset < 0 or offset + struct.calcsize(fmt) > end:
        raise ValueError("truncated Mach-O structure")
    return struct.unpack_from(fmt, data, offset)


def inspect(data):
    magic, cpu, _, kind, count, size, _, _ = take("<8I", data, 0, len(data))
    if (magic, cpu, kind) != (0xfeedfacf, 0x01000007, 2):
        raise ValueError("expected a thin x86_64 executable (extract the slice with lipo)")
    end = 32 + size
    if end > len(data):
        raise ValueError("truncated load commands")
    pos = 32
    identity = None
    code = None
    for _ in range(count):
        cmd, length = take("<II", data, pos, end)
        limit = pos + length
        if length < 8 or length % 8 or limit > end:
            raise ValueError("invalid load command size")
        if cmd == 0x1b:
            if length != 24 or identity:
                raise ValueError("invalid LC_UUID")
            identity = uuid.UUID(bytes=data[pos+8:limit])
        if cmd == 0x19:
            sections, = take("<I", data, pos + 64, limit)
            if length != 72 + sections * 80:
                raise ValueError("invalid segment size")
            for i in range(sections):
                section = pos + 72 + i * 80
                name, segment, _, nbytes, offset = take("<16s16sQQI", data, section, limit)
                if (name.rstrip(b"\0"), segment.rstrip(b"\0")) == (b"__text", b"__TEXT"):
                    if code or offset + nbytes > len(data):
                        raise ValueError("invalid text section")
                    code = (offset, nbytes)
        pos = limit
    if pos != end or not identity or not code:
        raise ValueError("missing or invalid UUID/text section")
    return identity, code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--expected-uuid", type=uuid.UUID, required=True)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    identity, (start, size) = inspect(data)
    if identity != args.expected_uuid:
        raise ValueError(f"UUID mismatch: {identity}")
    source = (Path(__file__).resolve().parents[1] / "BrcmPatchRAM/BlueToolFixup.cpp").read_text()
    for name in ("SpecAdvSetsCap", "ExtScanCap", "LeEventMask", "AdvSetAddr", "ExtCreateConn", "AdvSetAddrResend"):
        array = re.search(r"k" + name + r"Original26\[\]\s*=\s*\{([^}]*)\}", source, re.S)
        if not array:
            raise ValueError(f"missing pattern: {name}")
        body = re.sub(r"//[^\n]*|/\*.*?\*/", "", array[1], flags=re.S)
        pattern = bytes(int(part.strip(), 0) for part in body.split(",") if part.strip())
        offset = data.find(pattern, start, start + size)
        if not pattern or offset < 0 or data.find(pattern, offset + 1, start + size) >= 0:
            raise ValueError(f"{name}: missing or ambiguous")
        if offset // 4096 != (offset + len(pattern) - 1) // 4096:
            raise ValueError(f"{name}: spans two validation pages")
        print(f"{name}: 0x{offset:x}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(str(error))
