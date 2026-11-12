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
SETUP_PREFIX_EXTENDED = bytes.fromhex(
    "ff 43 02 d1 fa 67 04 a9 f8 5f 05 a9 f6 57 06 a9 "
    "f4 4f 07 a9 fd 7b 08 a9 fd 03 02 91 f4 03 04 aa "
    "f5 03 02 aa f6 03 01 aa f3 03 00 aa"
)
PRELOAD_PREFIX = bytes.fromhex(
    "ff c3 00 d1 f4 4f 01 a9 fd 7b 02 a9 fd 83 00 91 "
    "a0 04 00 b4 f3 03 01 aa f4 03 00 aa"
)
ACTOR_REFRESH_PREFIX = bytes.fromhex(
    "ff 43 01 d1 f8 5f 01 a9 f6 57 02 a9 f4 4f 03 a9 "
    "fd 7b 04 a9 fd 03 01 91 f4 03 02 aa f3 03 00 aa"
)
CHARACTER_UPDATE_PREFIX = bytes.fromhex(
    "ff 83 01 d1 f6 57 03 a9 f4 4f 04 a9 fd 7b 05 a9 "
    "fd 43 01 91 28 00 40 f9"
)
CHARACTER_PREPARE_PREFIX = bytes.fromhex(
    "f4 4f be a9 fd 7b 01 a9 fd 43 00 91 f3 03 01 aa "
    "f4 03 00 aa 08 00 40 f9"
)
# Named-animation helpers resolve the actor's animation player through a
# virtual getter, then pass the hashed clip name to the player:
# ldr x8, [x19]; ldr x8, [x8, #slot]; mov x0, x19; blr x8;
# ldrb w2, [sp, #0x14]; ldp w1, w3, [sp, #0xc]; ldr s0, [sp, #0x18]; bl play
ANIMATION_CALL = bytes.fromhex("e2 53 40 39 e1 8f 41 29 e0 1b 40 bd")
# mov x0, x19; mov w3, #1 immediately before the skin setup call.
SETUP_CALL_TAIL = bytes.fromhex("e0 03 13 aa 23 00 80 52")


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
    symbols = {}
    game_uuid = None
    pos = commands
    for _ in range(ncmds):
        command, size = struct.unpack_from("<II", data, pos)
        if size < 8 or pos + size > end:
            raise ValueError("invalid Mach-O load command")
        if command == 0x1B:
            game_uuid = str(uuid_module.UUID(bytes=data[pos + 8:pos + 24])).upper()
        if command == 0x2:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", data, pos + 8)
            for index in range(nsyms):
                strx, kind, _sect, _desc, value = struct.unpack_from(
                    "<IBBHQ", data, offset + symoff + index * 16)
                # Only defined section symbols; imports have no address here.
                if kind & 0x0E == 0x0E and strx < strsize:
                    name_at = offset + stroff + strx
                    name = data[name_at:data.index(b"\0", name_at)]
                    if name == b"__Znwm":
                        symbols["operator_new"] = value
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
    return game_uuid, sections[("__TEXT", "__text")], segments, symbols


def decode_bl(word, pc):
    if word & 0xFC000000 != 0x94000000:
        return None
    immediate = word & 0x03FFFFFF
    if immediate & (1 << 25):
        immediate -= 1 << 26
    return pc + immediate * 4


def decode_b(word, pc):
    if word & 0xFC000000 != 0x14000000:
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


def setup_call_fields(text, pos):
    """Return the actor fields used by a verified `bl setup` at text offset pos.

    Older builds pass the skin ID with one load. Newer builds keep the field
    encoded and decode it inline. Schema 10 also supports a rotate and add
    before the XOR/rotate pair; constants are recovered from the actual call.
    """
    if text[pos - 8:pos] != SETUP_CALL_TAIL:
        return None
    # New layout: mov champion; ldr w9, [actor, #skin]; ror w9;
    # movz/movk addend; add w9; movz/movk key; eor ... ror; ror w2;
    # adrp/add override; add x1, actor, x8; mov x0; mov w3; bl setup.
    at = pos - 60
    words = [u32(text, at + delta) for delta in range(0, 60, 4)] if at >= 0 else [0] * 15
    if (words[0] & 0xFFE0001F) == 0x52800008 and \
       (words[1] & 0xFFC003FF) == 0xB9400269 and \
       (words[2] & 0xFFFF83FF) == 0x13890129 and \
       (words[3] & 0xFFE0001F) == 0x5280000A and \
       (words[4] & 0xFFE0001F) == 0x72A0000A and words[5] == 0x0B0A0129 and \
       (words[6] & 0xFFE0001F) == 0x5280000A and \
       (words[7] & 0xFFE0001F) == 0x72A0000A and \
       (words[8] & 0xFFFF83FF) == 0x4AC90149 and \
       (words[9] & 0xFFFF83FF) == 0x13890122 and \
       (words[10] & 0x9F00001F) == 0x90000004 and \
       (words[11] & 0xFFC003FF) == 0x91000084 and words[12] == 0x8B080261:
        immediate = lambda low, high: ((low >> 5) & 0xFFFF) | (((high >> 5) & 0xFFFF) << 16)
        return ((words[0] >> 5) & 0xFFFF, ((words[1] >> 10) & 0xFFF) * 4,
                immediate(words[6], words[7]), (words[8] >> 10) & 31,
                (words[9] >> 10) & 31, (words[2] >> 10) & 31,
                immediate(words[3], words[4]))
    for load_at, encoded in ((pos - 20, False), (pos - 36, True)):
        mov, load = u32(text, load_at - 8), u32(text, load_at)
        if (mov & 0xFF80001F) != 0x52800008 or \
           text[load_at - 4:load_at] != bytes.fromhex("61 02 08 8b"):
            continue
        if not encoded:
            if (load & 0xFFC003FF) != 0xB9400262:
                continue
            key = field_ror = value_ror = 0
        else:
            low, high, eor, ror = (u32(text, load_at + delta) for delta in (4, 8, 12, 16))
            # ldr w8, [x19, #skin]; movz w9; movk w9, lsl #16;
            # eor w8, w9, w8, ror #a; ror w2, w8, #b
            if (load & 0xFFC003FF) != 0xB9400268 or \
               (low & 0xFFE0001F) != 0x52800009 or (high & 0xFFE0001F) != 0x72A00009 or \
               (eor & 0xFFFF03FF) != 0x4AC80128 or (ror & 0xFFFF03FF) != 0x13880102:
                continue
            key = ((low >> 5) & 0xFFFF) | (((high >> 5) & 0xFFFF) << 16)
            field_ror, value_ror = (eor >> 10) & 0x3F, (ror >> 10) & 0x3F
            if field_ror > 31 or value_ror > 31:
                continue
        champion = ((mov >> 5) & 0xFFFF) << (((mov >> 21) & 3) * 16)
        skin = ((load >> 10) & 0xFFF) * 4
        return champion, skin, key, field_ror, value_ror, 0, 0
    return None


def find_animation_offsets(text_addr, text):
    """Find the animation-player getter and the clip-name table it consults."""
    calls = []
    start = 0
    while True:
        pos = text.find(ANIMATION_CALL, start)
        if pos < 0:
            break
        start = pos + 1
        if pos % 4 or pos < 16:
            continue
        slot = u32(text, pos - 12)
        if u32(text, pos - 16) != 0xF9400268 or (slot & 0xFFC003FF) != 0xF9400108 or \
           u32(text, pos - 8) != 0xAA1303E0 or u32(text, pos - 4) != 0xD63F0100:
            continue
        play = decode_bl(u32(text, pos + 12), text_addr + pos + 12)
        if play is not None:
            calls.append((((slot >> 10) & 0xFFF) * 8, play))
    # Several helpers share this sequence; they must all agree.
    if len(calls) < 3:
        raise ValueError("animation player getter: too few verified call sites")
    animation_slot, play = one(sorted(set(calls)), "animation player getter")
    # The player passes its override map and its graph to the clip resolver:
    # add x24, x0, #map; ldr x23, [x0, #graph]; ...; mov x2, x23; bl resolve
    at = play - text_addr
    graph_offsets, resolvers = [], []
    for delta in range(0, 0x60, 4):
        word = u32(text, at + delta)
        if (word & 0xFFC003FF) == 0xF9400017:
            graph_offsets.append(((word >> 10) & 0xFFF) * 8)
        if u32(text, at + delta) == 0xAA1703E2:
            target = decode_bl(u32(text, at + delta + 4), play + delta + 4)
            if target is not None:
                resolvers.append(target)
    graph_offset = one(graph_offsets, "animation graph field")
    resolver = one(resolvers, "animation clip resolver")
    at = resolver - text_addr
    sized_map = [u32(text, at + delta) for delta in (0, 8, 16)] == [
        0xF9400409, 0xF9400C08, 0x2A0103E8
    ] and all(u32(text, at + delta) & 0xFF00001F == 0xB4000000 | reg
              for delta, reg in ((4, 9), (12, 8)))
    if u32(text, at) != 0x92407C28 and not sized_map:
        raise ValueError("animation clip resolver changed")
    body = [u32(text, at + delta) for delta in range(0, 0x290, 4)]
    # The graph keeps a libc++ unordered_map<uint32 hash, int32 clip>: buckets,
    # bucket count, then nodes {next, hash, key, value}.
    table = {((word >> 10) & 0xFFF) * 8 for word in body if (word & 0xFFC003E0) == 0xF9400040}
    clip_map = one([offset for offset in table if offset + 8 in table], "animation clip table")
    expected_table = {clip_map, clip_map + 8}
    if sized_map:
        expected_table.add(clip_map + 0x18)
    if table != expected_table or not any((word & 0xFFFFFC1F) == 0xB9401400 for word in body) or \
       not any((word & 0xFFFFFC00) == 0xB9401000 for word in body):
        raise ValueError("animation clip table layout changed")
    return animation_slot, graph_offset, clip_map


def fnv1a(name):
    value = 0x811C9DC5
    for byte in name.lower().encode():
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def registered_field_offsets(text, name):
    """Offsets passed with a field's name hash to the bin class registration:
    mov w1, #lo; movk w1, #hi, lsl #16; mov w2, #offset."""
    value = fnv1a(name)
    needle = struct.pack("<II", 0x52800001 | (value & 0xFFFF) << 5,
                         0x72A00001 | (value >> 16) << 5)
    offsets, start = [], 0
    while True:
        pos = text.find(needle, start)
        if pos < 0:
            return offsets
        start = pos + 1
        word = u32(text, pos + 8)
        if pos % 4 == 0 and (word & 0xFFE0001F) == 0x52800002:
            offsets.append((word >> 5) & 0xFFFF)


def find_gear_list_offsets(text_addr, text, actor_refresh):
    """The path from the actor to its skin's gear list, as the gear component
    setup reads it: [[actor+resource]+properties]+list, count at list+8."""
    # The gear component helpers just before the actor refresh create a
    # component and set it up: bl create; mov x21, x0; mov x1, x20; mov x2, #0; bl setup
    at = actor_refresh - text_addr
    setups = set()
    for pos in range(max(0, at - 0x200), at - 16, 4):
        if decode_bl(u32(text, pos), text_addr + pos) is None or \
           [u32(text, pos + delta) for delta in (4, 8, 12)] != [0xAA0003F5, 0xAA1403E1, 0xD2800002]:
            continue
        target = decode_bl(u32(text, pos + 16), text_addr + pos + 16)
        if target is not None:
            setups.add(target)
    setup = one(sorted(setups), "gear component setup")
    body = [u32(text, setup - text_addr + delta) for delta in range(0, 0x200, 4)]

    def loads(value, scale, label):
        return one(sorted({((word >> 10) & 0xFFF) * scale for word in body
                           if (word & 0xFFC003FF) == value}), label)
    if any(word & 0xFFC003FF == 0xF9400276 for word in body):
        resource = loads(0xF9400276, 8, "skin resource field")      # ldr x22, [x19, #resource]
        properties = loads(0xF9400137, 8, "skin properties field")  # ldr x23, [x9, #properties]
        count = loads(0xB94002E9, 4, "gear count field")
        gear_list = loads(0xF94002E9, 8, "gear list field")
    else:
        # This build checks x8 == actor (x19), then loads its resource.
        resource = one([((body[i + 2] >> 10) & 0xFFF) * 8
                        for i in range(len(body) - 2)
                        if body[i] == 0xEB13011F and body[i + 1] & 0xFF00001F == 0x54000001
                        and body[i + 2] & 0xFFC003FF == 0xF9400116], "skin resource field")
        properties = one([((body[i + 1] >> 10) & 0xFFF) * 8
                          for i in range(len(body) - 1)
                          if body[i] == 0xF94007E8 and
                          body[i + 1] & 0xFFC003FF == 0xF9400116], "skin properties field")
        count = loads(0xB94002C9, 4, "gear count field")
        gear_list = loads(0xF94002C9, 8, "gear list field")
    # The list is SkinCharacterDataProperties.skinUpgradeData.mGearSkinUpgrades.
    if gear_list + 8 != count or gear_list not in registered_field_offsets(text, "skinUpgradeData") or \
       0 not in registered_field_offsets(text, "mGearSkinUpgrades"):
        raise ValueError("gear list layout changed")
    return resource, properties, gear_list


def find_offsets(text_addr, text, segments, symbols):
    frames = []
    setups = []
    preloads = []
    character_updates = []
    character_prepares = []
    for pos in range(0, len(text) - 64, 4):
        if text.startswith(FRAME_PREFIX, pos) and \
           text.startswith(FRAME_MIDDLE, pos + 40) and \
           u32(text, pos + 56) & 0xFF00001F == 0xB4000008 and \
           text.startswith(FRAME_END, pos + 60):
            frames.append(text_addr + pos)
        if text.startswith(SETUP_PREFIX, pos) or text.startswith(SETUP_PREFIX_EXTENDED, pos):
            setups.append(text_addr + pos)
        if text.startswith(PRELOAD_PREFIX, pos):
            preloads.append(text_addr + pos)
        if text.startswith(CHARACTER_UPDATE_PREFIX, pos) and \
           u32(text, pos + 24) & 0xFFC003FF == 0xF9400009 and \
           u32(text, pos + 28) == 0xEB09011F:
            character_updates.append(text_addr + pos)
        if text.startswith(CHARACTER_PREPARE_PREFIX, pos) and \
           u32(text, pos + 24) & 0xFFC003FF == 0xF9400108 and \
           u32(text, pos + 28) == 0xD63F0100:
            character_prepares.append(text_addr + pos)
    frame = one(frames, "frame hook")
    setup = one(setups, "skin setup")
    setup_pos = setup - text_addr
    actor_refreshes = []
    for pos in range(max(0, setup_pos - 0x400), setup_pos, 4):
        candidate = text_addr + pos
        # The call moves as fields are added before it (+0xc8 in 16.20, +0x124 in 16.21).
        if text.startswith(ACTOR_REFRESH_PREFIX, pos) and \
           any(decode_bl(u32(text, pos + delta), candidate + delta) == setup
               for delta in range(0x40, 0x200, 4)):
            actor_refreshes.append(candidate)
    actor_refresh = one(actor_refreshes, "actor appearance refresh")
    preload = one(preloads, "character and skin preload")
    # These helpers sit next to skin setup. Virtual-table slots and actor
    # field positions move between patches, while the calling shape persists.
    character_update = one([address for address in character_updates
                            if setup < address < setup + 0x10000],
                           "character resource update")
    character_prepare = one([address for address in character_prepares
                             if setup < address < setup + 0x10000],
                            "character animation prepare")
    if decode_bl(u32(text, character_update - text_addr + 0xc4),
                 character_update + 0xc4) is None:
        raise ValueError("character resource update call changed")
    preload_pos = preload - text_addr
    cache_lookup = decode_bl(u32(text, preload_pos + 0x38), preload + 0x38)
    cache_create = decode_bl(u32(text, preload_pos + 0x68), preload + 0x68)
    if cache_lookup is None or cache_create is None or cache_lookup == cache_create or \
       u32(text, preload_pos + 0x3c) != 0xF94007E0 or \
       u32(text, preload_pos + 0x84) != 0x910003E0:
        raise ValueError("character preload call chain changed")

    callers = []
    for pos in range(max(60, setup_pos - 0x3000), setup_pos, 4):
        if decode_bl(u32(text, pos), text_addr + pos) != setup:
            continue
        fields = setup_call_fields(text, pos)
        if fields and 0x1000 <= fields[0] <= 0xFFFF and 0x100 <= fields[1] < 0x4000:
            callers.append(fields)
    champion_offset, skin_offset, skin_key, skin_field_ror, skin_value_ror, skin_pre_ror, skin_add = \
        one(callers, "setup call and actor fields")

    # The setup resolver stores the selected CharacterRecord* in an actor
    # field. Validate both stores and both reads before handing the address to
    # the game-thread character binding call.
    at = setup - text_addr
    record_offsets = []
    for delta in range(0x180, 0x500, 4):
        if at + delta + 0x2c >= len(text): break
        loads = [u32(text, at + delta + offset) for offset in (0, 0x18)]
        stores = [u32(text, at + delta + offset) for offset in (0x8, 0x2c)]
        if any(word & 0xFFC003FF != 0xF9400268 for word in loads) or \
           any(word & 0xFFC003FF != 0xF9000260 for word in stores):
            continue
        offsets = {((word >> 10) & 0xFFF) * 8 for word in loads + stores}
        if len(offsets) == 1:
            record_offsets.extend(offsets)
    character_record_offset = one(record_offsets, "character record field")

    globals_found = []
    for pos in range(setup_pos, min(len(text) - 24, setup_pos + 0x20000), 4):
        page = decode_adrp(u32(text, pos), text_addr + pos)
        if page is None:
            continue
        page_adjust = u32(text, pos + 4)
        if page_adjust == 0xD503201F:
            pass
        elif page_adjust & 0xFFC003FF == 0x91000108:  # add x8, x8, #pageoff
            page += (page_adjust >> 10) & 0xFFF
        else:
            continue
        load = u32(text, pos + 8)
        branch = u32(text, pos + 12)
        if load & 0xFFC003FF != 0xF9400100 or \
           branch & 0xFF00001F != 0xB4000000 or \
           u32(text, pos + 16) != 0xF9400008 or \
           u32(text, pos + 20) & 0xFFC003FF != 0xF9400108:
            continue
        target = page + (((load >> 10) & 0xFFF) * 8)
        if target % 8 == 0 and any(name in ("__DATA", "__DATA_CONST") and start <= target < stop
                                     for name, start, stop in segments):
            globals_found.append(target)
    actor_global = one(globals_found, "local actor global")
    # The skin setup copies a character-stack record and tail-calls its refresh
    # function. The form selector is the byte at record+0x84. Verify the complete
    # call chain before publishing these values; a game patch must fail closed.
    at = setup - text_addr
    stack_mov = u32(text, at + 0x68)
    if stack_mov & 0xFF80001F != 0x52800008 or \
       [u32(text, at + delta) for delta in (0x6c, 0x70, 0x74)] not in (
           [0x8B080260, 0x29077FF5, 0x910063E1],
           [0x29077FF5, 0x8B080260, 0x910063E1],
       ):
        raise ValueError("character stack setup signature changed")
    stack_offset = ((stack_mov >> 5) & 0xFFFF)
    if not 0x2000 <= stack_offset <= 0x5000 or stack_offset % 8:
        raise ValueError("character stack field outside actor")
    copy_record = decode_bl(u32(text, at + 0x78), setup + 0x78)
    if copy_record is None:
        raise ValueError("character stack copy call changed")
    at = copy_record - text_addr
    if [u32(text, at + delta) for delta in (0, 0x28, 0x2C, 0x38, 0x3C)] != [
        0xA9BE4FF4, 0xF9401268, 0xF9001E88, 0xA9417BFD, 0xA8C24FF4
    ]:
        raise ValueError("character stack copy signature changed")
    refresh = decode_b(u32(text, at + 0x40), copy_record + 0x40)
    if refresh is None or u32(text, refresh - text_addr) != 0xD10203FF:
        raise ValueError("character stack refresh changed")
    helper = decode_bl(u32(text, refresh - text_addr + 0x4c), refresh + 0x4c)
    if helper is None:
        raise ValueError("gear reader call changed")
    at = helper - text_addr
    gear_layouts = (
        (0xD10283FF, 0xF0, (0x204, 0x2E8, 0x374)),
        (0xD102C3FF, 0xF4, (0x204, 0x2F0, 0x384)),
    )
    if not any(u32(text, at) == prefix and u32(text, at + base) == 0x91006118 and
               all(u32(text, at + delta) == 0x39421388 for delta in loads)
               for prefix, base, loads in gear_layouts):
        raise ValueError("gear field signature changed")
    gear_offset = stack_offset + 0x18 + 0x84
    animation_slot, animation_graph_offset, clip_map_offset = \
        find_animation_offsets(text_addr, text)
    skin_resource_offset, skin_properties_offset, gear_list_offset = \
        find_gear_list_offsets(text_addr, text, actor_refresh)
    # Clip aliases are inserted into a game-owned hash table, so their nodes
    # must come from the game's own allocator.
    operator_new = symbols.get("operator_new")
    if operator_new is None or not text_addr <= operator_new < text_addr + len(text):
        raise ValueError("game operator new not found")
    return {
        "frame": frame, "setup": setup, "preload": preload,
        "actor_refresh": actor_refresh, "character_update": character_update,
        "character_prepare": character_prepare, "actor_global": actor_global,
        "champion_name_offset": champion_offset, "skin_id_offset": skin_offset,
        "skin_id_key": skin_key, "skin_id_field_ror": skin_field_ror,
        "skin_id_value_ror": skin_value_ror, "refresh": refresh,
        "skin_id_pre_ror": skin_pre_ror, "skin_id_add": skin_add,
        "stack_offset": stack_offset, "gear_offset": gear_offset,
        "character_record_offset": character_record_offset,
        "animation_slot": animation_slot, "animation_graph_offset": animation_graph_offset,
        "clip_map_offset": clip_map_offset, "operator_new": operator_new,
        "skin_resource_offset": skin_resource_offset,
        "skin_properties_offset": skin_properties_offset, "gear_list_offset": gear_list_offset,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=DEFAULT_GAME)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "offsets.json")
    parser.add_argument("--check", action="store_true", help="verify without writing")
    args = parser.parse_args()
    game_uuid, (text_addr, text), segments, symbols = parse_macho(args.game)
    found = find_offsets(text_addr, text, segments, symbols)
    profile = {"schema": 10, "uuid": game_uuid}
    profile.update({key: hex(value) for key, value in found.items()})
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
