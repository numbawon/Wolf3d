#!/usr/bin/env python3
"""tools/analyze_maps.py — empirical pinning of GAMEMAPS plane value semantics.

Phase 2 docs-of-record. Decodes every map of both games (independent
re-implementation of the Carmack/RLEW decoders, cross-checked against the C++
decoders by the golden tests) and reports:

  * plane0 / plane1 value histograms (across all 81 maps)
  * start cells (plane1 19..22), doors (plane0 90..101), elevator walls
    (plane0 == 21), pushwalls (plane1 == 98), markers plane1 256..259
  * walkability: BFS from the start cell over the cells the reference engine
    treats as open (plane0 > 63 and not a door, minus blocking props), so we
    can verify the reference's wall/door/open split against the real data
  * per-episode last-map check (where exits are expected to live)

Usage:  python tools/analyze_maps.py [game_dir]
        game_dir defaults to the Steam install root.
"""

import json
import struct
import sys
from collections import Counter, defaultdict

GAME_DIR = sys.argv[1] if len(sys.argv) > 1 else (
    r"C:\Program Files (x86)\Steam\steamapps\common\Wolfenstein 3D")

SIDE = 64
CELLS = SIDE * SIDE

BLOCKING_PROPS = {24, 25, 26, 28, 30, 31, 33, 34, 35, 36, 39, 40, 41, 45,
                  58, 59, 60, 62, 63, 68, 69}


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def carmack_decode(blob):
    """Carmack far/near-pointer stream -> decoded byte string.

    Mirrors src/w3d/carmack.cpp: decoding stops exactly at the declared
    total. Near pointer (x, 0xA7, z): x copies of the 2-byte word found
    2*z bytes back from the end of the output, source advancing one word
    per copy. Far pointer (x, 0xA8, w): x copies of the words at 2*w,
    2*w+2, ... .
    """
    (size,) = struct.unpack_from("<H", blob, 0)
    out = bytearray()
    i = 2
    n = len(blob)
    while i + 2 <= n and len(out) < size:
        x = blob[i]
        y = blob[i + 1]
        i += 2
        if y in (0xA7, 0xA8):
            if x == 0:
                out += bytes((blob[i], y))
                i += 1
            elif y == 0xA7:
                z = 2 * blob[i]
                i += 1
                src = len(out) - z
                for _ in range(x):
                    out += out[src:src + 2]
                    src += 2
            else:
                (w,) = struct.unpack_from("<H", blob, i)
                i += 2
                for _ in range(x):
                    out += out[2 * w:2 * w + 2]
                    w += 1
        else:
            out += bytes((x, y))
    if len(out) != size:
        raise ValueError(f"carmack: decoded {len(out)} != declared {size}")
    return bytes(out)


def rlew_decode(blob, tag=0xABCD):
    """RLEW word stream -> decoded byte string.

    [u16 total][words...] with word == tag: [u16 count][u16 value]
    (6 bytes total per run) -> count copies of value.
    """
    (size,) = struct.unpack_from("<H", blob, 0)
    out = bytearray()
    i = 2
    n = len(blob)
    while i < n:
        (w,) = struct.unpack_from("<H", blob, i)
        i += 2
        if w == tag:
            (count, v) = struct.unpack_from("<HH", blob, i)
            out += struct.pack("<H", v) * count
            i += 4
        else:
            out += blob[i - 2:i]
    if len(out) != size:
        raise ValueError(f"rlew: decoded {len(out)} != declared {size}")
    return out


def load_game(gamemaps_path, maphead_path):
    gm = read_file(gamemaps_path)
    mh = read_file(maphead_path)
    (magic,) = struct.unpack_from("<H", mh, 0)
    assert magic == 0xABCD, f"MAPHEAD magic {magic:#x}"
    (p0magic,) = struct.unpack_from("<8s", gm, 0)
    assert p0magic == b"TED5v1.0", f"GAMEMAPS magic {p0magic!r}"
    offsets = struct.unpack_from(f"<100I", mh, 2)
    maps = []
    for off in offsets:
        if off == 0:
            continue
        (p0o, p1o, p2o) = struct.unpack_from("<3I", gm, off)
        (p0s, p1s, p2s, w, h) = struct.unpack_from("<5H", gm, off + 12)
        assert (w, h) == (64, 64), (w, h)
        name = gm[off + 22: off + 38].split(b"\0")[0].decode("ascii")
        tag4 = gm[off + 38: off + 42]
        assert tag4 == b"!ID!", tag4
        pl0 = rlew_decode(carmack_decode(gm[p0o:p0o + p0s]))
        pl1 = rlew_decode(carmack_decode(gm[p1o:p1o + p1s]))
        blob = gm[p2o:p2o + p2s]
        assert blob == bytes.fromhex("08000020cdab00100000"), blob.hex()
        maps.append({
            "name": name,
            "plane0": list(struct.unpack(f"<{CELLS}H", pl0)),
            "plane1": list(struct.unpack(f"<{CELLS}H", pl1)),
        })
    return maps


def analyze(gamemaps_path, maphead_path, label):
    maps = load_game(gamemaps_path, maphead_path)
    h0 = Counter()
    h1 = Counter()
    rows = []
    for mi, m in enumerate(maps):
        p0, p1 = m["plane0"], m["plane1"]
        h0.update(p0)
        h1.update(p1)
        starts = [(x, y) for y in range(SIDE) for x in range(SIDE)
                  if 19 <= p1[y * SIDE + x] <= 22]
        doors = [(x, y) for y in range(SIDE) for x in range(SIDE)
                 if 90 <= p0[y * SIDE + x] <= 101]
        elev = [(x, y) for y in range(SIDE) for x in range(SIDE)
                if p0[y * SIDE + x] == 21]
        push = [(x, y) for y in range(SIDE) for x in range(SIDE)
                if p1[y * SIDE + x] == 98]
        marks = [(x, y, p1[y * SIDE + x]) for y in range(SIDE)
                 for x in range(SIDE) if 256 <= p1[y * SIDE + x] <= 259]
        enemies = sum(1 for v in p1 if v >= 108)

        # Reference-engine walkability: blocked = wall (m0<=63) or door
        # (90..101) or blocking prop.
        blocked = [[False] * SIDE for _ in range(SIDE)]
        for y in range(SIDE):
            for x in range(SIDE):
                v0 = p0[y * SIDE + x]
                v1 = p1[y * SIDE + x]
                if v0 <= 63 or 90 <= v0 <= 101 or v1 in BLOCKING_PROPS:
                    blocked[y][x] = True
        open_cells = sum(1 for y in range(SIDE) for x in range(SIDE)
                         if not blocked[y][x])
        reach = 0
        if starts:
            # Last start in scan order wins (reference iterates y then x and
            # overwrites).
            sx, sy = starts[-1]
            seen = {(sx, sy)}
            stack = [(sx, sy)]
            while stack:
                x, y = stack.pop()
                reach += 1
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if (0 <= nx < SIDE and 0 <= ny < SIDE
                            and (nx, ny) not in seen
                            and not blocked[ny][nx]):
                        seen.add((nx, ny))
                        stack.append((nx, ny))
        rows.append({
            "map": mi, "name": m["name"],
            "starts": starts, "start_ok": len(starts) == 1,
            "doors": len(doors), "door_cells": doors[:8],
            "elevator": elev, "pushwalls": len(push),
            "markers256": len(marks), "marker_cells": marks[:8],
            "enemies": enemies, "open_cells": open_cells,
            "reachable": reach,
        })

    print(f"=== {label}: {len(maps)} maps ===")
    print(f"plane0 histogram (value: count, maps):")
    in0 = Counter()
    for m in maps:
        in0.update(set(m["plane0"]))
    for v in sorted(h0):
        print(f"  {v:4d}: {h0[v]:6d}  ({in0[v]} maps)")
    print(f"plane1 histogram (value: count, maps):")
    in1 = Counter()
    for m in maps:
        in1.update(set(m["plane1"]))
    for v in sorted(h1):
        print(f"  {v:4d}: {h1[v]:6d}  ({in1[v]} maps)")

    print("\nper-map summary:")
    for r in rows:
        flag = ""
        if not r["start_ok"]:
            flag += " START!"
        if r["open_cells"] and r["reachable"] < r["open_cells"]:
            flag += f" REACH {r['reachable']}/{r['open_cells']}"
        print(f"  [{r['map']:2d}] {r['name']:<14s} doors={r['doors']:<3d} "
              f"elev={len(r['elevator'])} push={r['pushwalls']} "
              f"mark256={r['markers256']} enemies={r['enemies']:<3d} "
              f"open={r['open_cells']:<4d} reach={r['reachable']:<4d}"
              f"{flag}")
        if r["elevator"]:
            print(f"        elevator cells: {r['elevator']}")
        if r["marker_cells"]:
            print(f"        256..259 cells: {r['marker_cells']}")

    # Episode last-map check: episode = name prefix ("Wolf1", "Wolf2", ...).
    print("\nepisode last maps:")
    episodes = defaultdict(list)
    for r in rows:
        prefix = " ".join(r["name"].split(" ")[:2])
        episodes[prefix].append(r)
    for prefix, rs in episodes.items():
        last = rs[-1]
        print(f"  {prefix}: {len(rs)} maps, last = {last['name']} "
              f"(elevator={len(last['elevator'])}, "
              f"mark256={last['markers256']}, doors={last['doors']})")
    return maps, rows


def main():
    all_rows = []
    base, rows0 = analyze(f"{GAME_DIR}/base/GAMEMAPS.WL6",
                          f"{GAME_DIR}/base/MAPHEAD.WL6", "base")
    sod, rows1 = analyze(f"{GAME_DIR}/base/m1/GAMEMAPS.SOD",
                         f"{GAME_DIR}/base/m1/MAPHEAD.SOD", "m1 (SOD)")
    all_rows = rows0 + rows1
    out = {
        "base": rows0,
        "sod": rows1,
    }
    with open(r"C:\Users\NumbaWon\AppData\Local\hermes\cache\scratch"
              r"\wolf3d-ref\map_analysis.json", "w") as f:
        json.dump(out, f, indent=1)
    print("\nwrote map_analysis.json")


if __name__ == "__main__":
    main()
