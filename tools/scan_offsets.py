#!/usr/bin/env python3
"""Locate verified ARM64 League addresses without opening Ghidra.

The signatures describe instruction relationships in the studied build. A
changed compiler layout must fail closed; a unique byte match alone does not
prove that a new game's function still has the same semantics.
"""

import argparse
import json
import os
from pathlib import Path
import struct
import sys
import uuid as uuid_module

DEFAULT_GAME = Path(
    "/Applications/League of Legends.app/Contents/LoL/Game/"
    "LeagueofLegends.app/Contents/MacOS/LeagueofLegends"
)
FRAME_PREFIX = bytes.fromhex(
    "ff c3 05 d1 fc 6f 12 a9 f8 5f 13 a9 f6 57 14 a9 "
    "f4 4f 15 a9 fd 7b 16 a9 fd 83 05 91 f3 03 00 aa"
)
FRAME_MIDDLE = bytes.fromhex("08 01 40 f9 a8 83 1b f8")
FRAME_END = bytes.fromhex("08 11 40 b9")
SETUP_PREFIX = bytes.fromhex(
    "ff 03 02 d1 f8 5f 04 a9 f6 57 05 a9 f4 4f 06 a9 "
    "fd 7b 07 a9 fd c3 01 91 f4 03 04 aa f5 03 02 aa "
    "f6 03 01 aa f3 03 00 aa"
)


def u32(data, pos):
    return struct.unpack_from("<I", data, pos)[0]


def one(items, label):
    if len(items) != 1:
        raise ValueError(f"{label}: expected one verified match, found {len(items)}")
    return items[0]


def parse_macho(path):
    data = path.read_bytes()
    offset = 0
    if data[:4] == b"\xca\xfe\xba\xbe":
        count = struct.unpack_from(">I", data, 4)[0]
        arm_slices = []
        for i in range(count):
            cpu, _subtype, start, size, _align = struct.unpack_from(">IIIII", data, 8 + i * 20)
            if cpu == 0x0100000C:
                arm_slices.append((start, size))
        offset, _size = one(arm_slices, "ARM64 slice")
    if u32(data, offset) != 0xFEEDFACF:
        raise ValueError("not a 64-bit Mach-O file")
    cpu, _subtype, _filetype, ncmds, sizeofcmds = struct.unpack_from("<IIIII", data, offset + 4)
    if cpu != 0x0100000C or ncmds > 1000 or sizeofcmds > 1_000_000:
        raise ValueError("unsupported Mach-O header")
    commands = offset + 32
    end = commands + sizeofcmds
    sections = {}
    segments = []
    game_uuid = None
    pos = commands
    for _ in range(ncmds):
        command, size = struct.unpack_from("<II", data, pos)
        if size < 8 or pos + size > end:
            raise ValueError("invalid Mach-O load command")
        if command == 0x1B:
            game_uuid = str(uuid_module.UUID(bytes=data[pos + 8:pos + 24])).upper()
        if command == 0x19:
            segname, vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<16sQQQQ", data, pos + 8)
            name = segname.rstrip(b"\0").decode("ascii")
            segments.append((name, vmaddr, vmaddr + vmsize))
            nsects = u32(data, pos + 64)
            for index in range(nsects):
                at = pos + 72 + index * 80
                sect, segment, address, length, file_offset = struct.unpack_from("<16s16sQQI", data, at)
                sect_name = sect.rstrip(b"\0").decode("ascii")
                segment_name = segment.rstrip(b"\0").decode("ascii")
                if file_offset + length <= filesize + fileoff:
                    sections[(segment_name, sect_name)] = (
                        address, data[offset + file_offset:offset + file_offset + length]
                    )
        pos += size
    if game_uuid is None or ("__TEXT", "__text") not in sections:
        raise ValueError("missing ARM64 UUID or text section")
    return game_uuid, sections[("__TEXT", "__text")], segments


def decode_bl(word, pc):
    if word & 0xFC000000 != 0x94000000:
        return None
    immediate = word & 0x03FFFFFF
    if immediate & (1 << 25):
        immediate -= 1 << 26
    return pc + immediate * 4


def decode_adrp(word, pc):
    if word & 0x9F00001F != 0x90000008:
        return None
    immediate = ((word >> 5) & 0x7FFFF) << 2 | ((word >> 29) & 3)
    if immediate & (1 << 20):
        immediate -= 1 << 21
    return (pc & ~0xFFF) + immediate * 4096


def find_offsets(text_addr, text, segments):
    frames = []
    setups = []
    for pos in range(0, len(text) - 64, 4):
        if text.startswith(FRAME_PREFIX, pos) and \
           text.startswith(FRAME_MIDDLE, pos + 40) and \
           u32(text, pos + 56) & 0xFF00001F == 0xB4000008 and \
           text.startswith(FRAME_END, pos + 60):
            frames.append(text_addr + pos)
        if text.startswith(SETUP_PREFIX, pos):
            setups.append(text_addr + pos)
    frame = one(frames, "frame hook")
    setup = one(setups, "skin setup")
    setup_pos = setup - text_addr

    callers = []
    for pos in range(max(28, setup_pos - 0x3000), setup_pos, 4):
        if decode_bl(u32(text, pos), text_addr + pos) != setup:
            continue
        mov = u32(text, pos - 28)
        ldr = u32(text, pos - 20)
        if (mov & 0xFF80001F) != 0x52800008 or \
           text[pos - 24:pos - 20] != bytes.fromhex("61 02 08 8b") or \
           (ldr & 0xFFC003FF) != 0xB9400262 or \
           text[pos - 8:pos - 4] != bytes.fromhex("e0 03 13 aa") or \
           text[pos - 4:pos] != bytes.fromhex("23 00 80 52"):
            continue
        champion = ((mov >> 5) & 0xFFFF) << (((mov >> 21) & 3) * 16)
        skin = ((ldr >> 10) & 0xFFF) * 4
        if 0x1000 <= champion <= 0xFFFF and 0x100 <= skin < 0x4000:
            callers.append((text_addr + pos, champion, skin))
    _caller, champion_offset, skin_offset = one(callers, "setup call and actor fields")

    globals_found = []
    for pos in range(setup_pos, min(len(text) - 24, setup_pos + 0x20000), 4):
        page = decode_adrp(u32(text, pos), text_addr + pos)
        if page is None or u32(text, pos + 4) != 0xD503201F:
            continue
        load = u32(text, pos + 8)
        branch = u32(text, pos + 12)
        if load & 0xFFC003FF != 0xF9400100 or \
           branch & 0xFF00001F != 0xB4000000 or \
           u32(text, pos + 16) != 0xF9400008 or \
           u32(text, pos + 20) != 0xF9413108:
            continue
        target = page + (((load >> 10) & 0xFFF) * 8)
        if target % 8 == 0 and any(name in ("__DATA", "__DATA_CONST") and start <= target < stop
                                     for name, start, stop in segments):
            globals_found.append(target)
    actor_global = one(globals_found, "local actor global")
    return frame, setup, actor_global, champion_offset, skin_offset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=DEFAULT_GAME)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "offsets.json")
    parser.add_argument("--check", action="store_true", help="verify without writing")
    args = parser.parse_args()
    game_uuid, (text_addr, text), segments = parse_macho(args.game)
    frame, setup, actor, champion, skin = find_offsets(text_addr, text, segments)
    profile = {
        "schema": 1,
        "uuid": game_uuid,
        "frame": hex(frame),
        "setup": hex(setup),
        "actor_global": hex(actor),
        "champion_name_offset": hex(champion),
        "skin_id_offset": hex(skin),
    }
    if not args.check:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        temp = args.output.with_suffix(args.output.suffix + ".tmp")
        temp.write_text(json.dumps(profile, indent=2) + "\n")
        os.replace(temp, args.output)
    print(json.dumps(profile, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, struct.error) as error:
        print(f"offset scan refused: {error}", file=sys.stderr)
        sys.exit(1)
