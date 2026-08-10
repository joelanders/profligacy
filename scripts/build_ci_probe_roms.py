#!/usr/bin/env python3
"""Build redistributable clean-room ROMs for packaged-product CI.

The H8 uploads tiny programs through the real board ports. DSP1 generates a
constant sentinel, DSP2 and DSP3 copy SI0 to SO0, and DSP3's SO0 reaches the
real DAC path. Consequently, nonzero final audio requires all three DSPs to
execute. The V55 writes an LCD sentinel. The H8 also transmits one byte on the
real inter-CPU serial link; H8 execution is independently required by the DSP
upload/audio chain so neither CPU's proof can mask the other.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def emit_v55() -> bytes:
    rom = bytearray([0xFF]) * 0x80000
    code = bytearray.fromhex("FA B8 00 F0 8E D8")  # cli; mov ax,F000; mov ds,ax

    def sfr_write(address: int, value: int) -> None:
        code.extend((0xC6, 0x06, address & 0xFF, address >> 8, value))

    def lcd_latch(value: int, rs: bool) -> None:
        sfr_write(0xFF07, value)
        sfr_write(0xFF05, 0x06 if rs else 0x04)
        sfr_write(0xFF05, 0x02 if rs else 0x00)

    lcd_latch(0x01, False)
    lcd_latch(0x80, False)
    for character in b"CI V55+H8 OK".ljust(40):
        lcd_latch(character, True)
    code += bytes.fromhex("EB FE")
    rom[:len(code)] = code
    rom[0x7FFF0:0x7FFF5] = bytes.fromhex("EA 00 00 00 80")
    return bytes(rom)


def emit_h8() -> bytes:
    rom = bytearray([0xFF]) * 0x20000
    rom[0:4] = bytes.fromhex("00 00 00 01")  # word-swapped reset vector -> 0x100
    code = bytearray()

    def mov_imm(value: int) -> None:
        code.extend((0xF8, value))

    def store(address: int) -> None:
        code.extend((0x6A, 0xA8))
        code.extend(address.to_bytes(4, "big"))

    def write(address: int, value: int) -> None:
        mov_imm(value)
        store(address)

    def word24(address: int, value: int) -> None:
        for shift in (16, 8, 0):
            write(address, (value >> shift) & 0xFF)

    def word32(address: int, value: int) -> None:
        for shift in (24, 16, 8, 0):
            write(address, (value >> shift) & 0xFF)

    # Real SCI0 setup and H8->V55 link exercise. H8 execution is independently
    # required by the DSP upload/audio chain, while V55 execution is required by
    # the LCD marker; neither proof is allowed to mask the other.
    for address, value in ((0x0FFFB0, 0x00), (0x0FFFB1, 0x0B),
                           (0x0FFFB2, 0x20), (0x0FFFB3, ord("H"))):
        write(address, value)
    code += bytes.fromhex("6A 28 00 0F FF B4 E8 7F")
    store(0x0FFFB4)

    write(0x0FFFD1, 0xFF)  # H8/3003 PADDR: Port A outputs
    ports = (0x0C0002, 0x0C0004, 0x0C0006)

    # Shared /PLOAD+/CLOAD. C0 = +0.25 in Q23, left-aligned for host upload.
    write(0x0FFFD3, 0xEB)
    for port in ports:
        word32(port, 0x20000000)
        for _ in range(255):
            word32(port, 0)

    # DSP1: constant C0 -> SO0. DSP2/3: SI0 -> SO0. All programs idle per frame.
    write(0x0FFFD3, 0xFB)
    generator = (0x480500, 0x001100, 0x00E100, 0x00E900, 0xFC4000)
    loopback = (0x008100, 0x00E100, 0x008900, 0x00E900, 0xFC4000)
    for port, st0, program in (
            (ports[0], 0x000680, generator),
            (ports[1], 0x000682, loopback),
            (ports[2], 0x000682, loopback)):
        word24(port, st0)
        word24(port, 0)
        for word in program:
            word24(port, word)
        for _ in range(256 - len(program)):
            word24(port, 0)

    write(0x0FFFD3, 0xFF)
    code += bytes.fromhex("40 FE")
    if len(code) & 1:
        code.append(0)
    file_code = bytearray()
    for index in range(0, len(code), 2):
        file_code.extend((code[index + 1], code[index]))
    rom[0x100:0x100 + len(file_code)] = file_code
    return bytes(rom)


def build(output: Path) -> dict[str, object]:
    rom_dir = output / "korgprop"
    rom_dir.mkdir(parents=True, exist_ok=True)
    files = {
        "korgprop/ic12_v17.bin": emit_v55(),
        "korgprop/ic22_v17.bin": emit_h8(),
    }
    records = []
    for relative, body in files.items():
        path = output / relative
        path.write_bytes(body)
        records.append({
            "path": relative,
            "bytes": len(body),
            "sha256": hashlib.sha256(body).hexdigest(),
        })
    return {
        "schema": "profligacy-clean-room-ci-rom-v1",
        "redistributable": True,
        "lcd_sentinel": "CI V55+H8 OK",
        "audio_path": "DSP1 C0 -> DSP2 SI0/SO0 -> DSP3 SI0/SO0 -> U2 DAC",
        "files": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args()
    receipt = build(args.output)
    body = json.dumps(receipt, indent=2, sort_keys=True) + "\n"
    if args.receipt:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(body, encoding="utf-8")
    print(body, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
