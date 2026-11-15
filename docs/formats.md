# Wolfenstein 3D — 6-Episode Data Formats (Phase 1)

All formats derived empirically from the Steam/GOG install:

```
C:\Program Files (x86)\Steam\steamapps\common\Wolfenstein 3D\base\      (episodes 1-3, .WL6)
C:\Program Files (x86)\Steam\steamapps\common\Wolfenstein 3D\base\m1\   (Spear of Destiny, .SOD)
```

No original source code was used. Every layout below is verified by decoding the
real files and cross-checking against an independent reference render of
sprite 110 (table & chairs) from https://vpoupet.github.io/wolfenstein
(888/956 content pixels exact; the 67 residual pixels map 1:1 to two palette
indices whose values differ between the reference author's original files and
the GOG remaster — the geometry, offsets and pool are byte-identical).

All integers are little-endian. All offsets are byte offsets into the file
they appear in, unless stated otherwise.

## File set

| File        | Size (base/) | Purpose                                              |
|-------------|--------------|------------------------------------------------------|
| CONFIG.WL6  | 522          | config/version blob ("id software-'92" + zeros)      |
| VGADICT.WL6 | 1024         | 512 × u16 lookup table (see below)                   |
| VGAGRAPH.WL6| 275774       | HUD/menu graphics (structure not fully cracked yet)  |
| VGAHEAD.WL6 | 450          | index table describing VGAGRAPH                      |
| GAMEMAPS.WL6| 150652       | map geometry (60 maps)                               |
| MAPHEAD.WL6 | 402          | index of GAMEMAPS header records                     |
| VSWAP.WL6   | 1544376      | walls + sprites + sounds (663 chunks)                |
| AUDIOHED.WL6| 1156         | index of AUDIOT chunks (289 × u32)                   |
| AUDIOT.WL6  | 320209       | 86 PCM SFX + 87 FM effects + 27 OPL music tracks   |

The m1/ (Spear of Destiny) set mirrors this: GAMEMAPS.SOD (21 maps),
VSWAP.SOD, AUDIOHED.SOD, AUDIOT.SOD, VGAGRAPH.SOD (947979), VGAHEAD.SOD (510),
VGADICT.SOD (1024), MAPHEAD.SOD (402), CONFIG.SOD (522).

## GAMEMAPS

```
[0]   8B   magic "TED5v1.0"
[8]   3B   unknown: base = a0 ff cc, m1 = a2 ff cc  (possibly a checksum)
then, per map i (i = 0..n-1):
      [p0o]  Carmack→RLEW stream (p0s bytes) → 64×64 u16 plane 0  (walls/walkable)
      [p1o]  Carmack→RLEW stream (p1s bytes) → 64×64 u16 plane 1  (objects/actors)
      [p2o]  10-byte CONSTANT blob, identical in every map of both games:
             08 00 00 20 cd ab 00 10 00 00
             (Carmack-decodes to 4 u16: 8192, 0xABCD, 4096, 0 — reserved
              plane descriptor; treat as opaque, assert equality)
      [hdr]  42-byte header record
```

Header record (42 bytes, located at MAPHEAD's offset for that map):

```
off  sz  field
0    4   p0o  absolute file offset of plane 0 stream
4    4   p1o  absolute file offset of plane 1 stream
8    4   p2o  absolute file offset of the 10-byte blob
12   2   p0s  compressed size of plane 0
14   2   p1s  compressed size of plane 1
16   2   p2s  = 10
18   2   w    = 64
20   2   h    = 64
22   16  name fixed 16-byte field, NUL-padded ("Wolf1 Map1"…"Wolf1 Map40"/
                   "Wolf2 …"/"Wolf3 …", m1: "Tunnels 1"…"Tunnels 21")
38   4   tag  "!ID!" (21 49 44 21) in every map
```

Record layout is `[pools][header]`: header_i sits immediately before
pools_{i+1}, so `p0o_{i+1} == MAPHEAD[i] + 42` for consecutive maps.
MAPHEAD[0] is the first header in the file; the pools it points at are the
bytes between the 11-byte file preamble (magic + 3 unknown + …) and MAPHEAD[0].

Plane values: verified range 0..143 (plane 0) and 0..259 (plane 1) across all
81 maps. 256..259 occur in plane 1 and are special markers (doors/exits).

## MAPHEAD

```
[0] 2   magic 0xABCD
[2] 400 100 × u32 offsets of the 42-byte header records (0 = no map)
```

60 non-zero entries for base/, 21 for m1/.

## VSWAP

```
[0] 2   n        = 663 (base) / 663 (m1)
[2] 2   first_sprite   = 106
[4] 2   first_sound    = 542
[6] 4n  u32 chunk offsets (absolute)
[6+4n] 2n u16 chunk lengths
```

- Chunks 0..105: wall textures. 4096 bytes = 64×64 8-bit palette indices,
  **column-major** (`idx = col*64 + row`), palette = gamePalette.
- Chunks 106..541: sprites (436).
- Chunks 542..662: sounds (121) — PCM audio data.

### Sprite chunk

```
[0] 2  first_col
[2] 2  last_col
[4] 2*(last_col-first_col+1)  u16 coloffs[col] (absolute offsets within chunk)
...
[4+2*ncols]  pixel pool: N bytes, 8-bit palette indices.
             N == pixel count, or pixel count + 1 when the pixel count is
             odd: the pool is padded with one 0x00 byte to a 2-byte
             boundary. The engine never reads the pad byte.
...
[coloffs[0]]  posts (run-length column data), ending exactly at chunk end
```

Posts: per column c from first_col..last_col, read one entry (6 bytes):

```
[u16 e1]
if e1 == 0:  column empty (no more entries for this column)
else: [u16 m1][u16 s1]      (m1 at +2: purpose unknown, unused for counting;
                             s1 at +4)
       rows s1/2 .. e1/2-1 (floor division, row 0 = top) get the next
       (e1/2 - s1/2) pool bytes, in row order.
```

The 6-byte stride and s1-at-+4 layout match the reference engine's decoder
byte-for-byte. Verified across the full corpus: all 436 sprites of VSWAP.WL6
— every post stream ends exactly at the chunk end, and the pool is consumed
exactly (or with exactly one trailing 0x00 pad byte when the pixel count is
odd; 221 of 436 sprites carry the pad).

Verified on chunk 110 (the spec's worked example, byte-for-byte match):
first post `78 00 3b 00 6e 00 00 00`, pool = 956 bytes, all 956 pool bytes
consumed, post stream ends exactly at the chunk boundary.

## gamePalette (VSWAP texture palette)

256 × u32, embedded in `src/w3d/gamepalette.inc` (generated from the
reference engine's `gamePalette`). Storage layout per entry:

```
u32 = 0xAABBGGRR   →  R = byte 0 (low), G = byte 1, B = byte 2, A = byte 3
```

i.e. **red is the low byte** (the JS array is 0xAABBGGRR when read as a
little-endian word). Entry 0 = 0xff000000 = black; entries 1..15 are the
classic VGA dark colors. Verified pixel-exact against the reference render.

## AUDIOHED / AUDIOT (Phase 4 — fully cracked)

```
AUDIOHED: 289 × u32 offsets into AUDIOT (base) / 268 × u32 (SOD).
          offsets[0] = 0; strictly non-decreasing; the LAST entry is the
          total size of AUDIOT (an EOF sentinel, not a chunk). The 288 (base)
          / 267 (SOD) real chunks are laid out in four regions.
```

**Chunk regions** (base shown; SOD in parens):

| Region    | base entries | SOD entries | Content                                  |
|-----------|--------------|-------------|------------------------------------------|
| PCM SFX   | 0..85 (86)   | 0..79 (80)  | digitized 8-bit mono sound effects       |
| unknown   | 86 (1)       | 80 (1)      | a value table (see below); skipped       |
| FM effects| 87..173 (87) | 81..161 (81)| OPL2 FM sound effects (pickup/door/etc.) |
| empty     | 174..260     | 162..257    | length 0; offset → the `!ID!` marker     |
| music     | 261..287 (27)| 258..266+   | OPL2 music tracks (27 base / 24 SOD)     |
| EOF       | 288          | 267         | = file size                              |

The regions are identified **by chunk shape**, not by trusting the table
indices: the GOG files re-arranged the original DOS layout (the original had
PC-speaker / FM / digitised tables of 87 each), so the empty region and the
table do not reliably bound the music. The robust way to reach the music is
to walk it in the raw file (below).

### PCM SFX chunk

```
[u32 nsamples][u16 volume 0..100][nsamples × u8 8-bit PCM][u8 flag = 0]
```

Chunk length == 6 + nsamples + 1 for every PCM entry. PCM is unsigned 8-bit
mono. `volume` is a discrete 0..100 ladder (observed set: 1,10,20,30,40,45,
49,50,60,70,80,85,87,90,99) — it is the *base gain* of the effect, not a
master rate. 86 in base, 80 in SOD.

Sample rate is **assumed 11025 Hz** (the value the VSWAP/PLAYPAL-era
digitised SFX are documented at; no clean-room reference decodes the PCM —
uWolf is OPL-only — so this is not independently confirmed). These SFX are
short by design: the longest is 306 samples (~28 ms at 11025 Hz), the median
69. The *long* Wolf3D sound effects (gunshots, doors, monster cries) are the
FM effects below, not the digitised ones.

### Unknown table chunk (one per file)

Base [86] / SOD [80]: `[u32 table_len][u16 0x32][table_len × u8]["00 !ID!"]`.
The bytes are a table of ramping values (e.g. SOD: `81 80 80 7f 7f 7e …`
then `6e 6f 70 71 …`); both end with `00 21 49 44 21` (`\0!ID!`). Purpose
unknown; it is neither a PCM SFX (length ≠ 6+ns+1) nor an FM effect (the
note data would overrun the chunk). The parser skips it.

### FM effect chunk

```
[u32 note_len][u16 priority 0..100][16 × u8 instrument][u8 block]
[note_len × u8 notes][name trailer: NUL-terminated ascii + padding]
```

- `instrument` (bytes 6..21), the first 10 used:
  `mChar cChar mScale cScale mAttack cAttack mSus cSus mWave cWave`
  (byte 15..21 = nConn + reserved). `m*` = modulator operator, `c*` =
  carrier operator.
- `block` (byte 22) = the note octave (0..7).
- `notes`: one byte per 140 Hz tick; the byte is the FNUM-low (0..127);
  a `0` byte means **key-off** for that tick.
- `name`: original SFX instrument tag (`HUMAN1`, `TUBA1`, `BASS`, `SHOT2`,
  `BANJO1`, `WOOD1`, `STRING2`, `BELLS2`, `MEOW1A`, `WHISTLE`, `CLICK`,
  `BAGPIPE2`, `ELGUIT5`, `POPBASS1`, `SDRSYN02`, `DRMLOG01`, `WUZZLE3B`,
  `ECHOBASS`, `WEIRD1/1A/1B`, `PIG1`, `QBERT1B`, `BASSDRN2`, `HUMAN3`,
  `METAL1`, `BASSGTR`, `CRUNCHY1`, `GTRELC01`, `PINNOR07`, …).

Playback (matches the original): program the instrument onto **channel 0**
(modulator = operator offset 0, carrier = operator offset 3), set feedback/
connect register `0xC0 = 0`, then step the note stream one byte per 140 Hz
tick — a non-zero note writes `0xA0 = note` (FNUM-low) and `0xB0 =
((block & 7) << 2) | 0x20` (key-on + octave); a zero note writes `0xB0 = 0`
(key-off). The effect borrows voice 0 of the music chip exactly as the
original did.

### Music track chunk (IMF / WLF)

The 27 base / 24 SOD tracks are a **contiguous region** in AUDIOT, located
by the first `!ID!!ID!` (8 bytes) marker (base: offset 22951; SOD: 17592).
Right after the marker, each track is:

```
[u16 dlen][dlen × (u8 reg, u8 val, u16 delay) — 4-byte IMF units]
[88-byte tag]
```

- `dlen` is the byte length of the IMF data (NOT including the 2 length
  bytes). The 88-byte tag (a Muse artefact the engine never reads) begins
  `[u8 1][u8 0][16 title][64 remarks][6 prog]`; the 16-byte title carries the
  song name (`CORNER`, `DUNGEON`, `WARMARCH`, `GETTHEM`, `HEADACHE`,
  `HITLWLTZ`, `INTROCW3`, `NAZI_NOR`, `NAZI_OMI`, `POW`, `SALUTE`,
  `SEARCHN`, `SUSPENSE`, `VICTORS`, `WONDERIN`, `FUNKYOU`, `ENDLEVEL`,
  `GOINGAFT`, `PREGNANT`, `ULTIMATE`, `NAZI_RAP`, `ZEROHOUR`, `TWELFTH`,
  `ROSTER`, `URAHERO`, `VICMARCH`, `PACMAN`; SOD adds the `X`-prefixed
  tracks `XFUNKIE`, `XDEATH`, `XTIPTOE`, `XTHEEND`, `XEVIL`, `XJAZNAZI`,
  `XAWARD`, `XPUTIT`, `XGETYOU`, `XTOWER2`, …).
- Walking `[2 + dlen + 88]` from the marker reaches EOF **exactly** in both
  files (base gap 0, SOD gap 0) — the definitive structural proof.
- IMF timing: the `delay` field is in ticks of a **700 Hz** clock
  (Wolf3D's AdLib clock). A track loops (the original never stops a floor's
  music).

### OPL2 (YM3812) register map — Wolf3D's packed encoding

The register byte in each IMF unit is NOT the stock Yamaha address. Wolf3D
packs both operators of each channel into the low nibble via:

```
i     = reg - page          (page ∈ {0x20,0x30,0x40,0x50,0x60,0x70,
                               0x80,0x90,0xE0,0xF0})
ch    = (i / 8) * 3 + (i % 8) % 3        // 0..8
op    = (i % 8) / 3                       // 0 = modulator, 1 = carrier
```

(offsets 0,1,2 = ch0-2; 8,9,10 = ch3-5; 16,17,18 = ch6-8; the `op` bit
selects modulator vs carrier within each channel group).

Register pages (val semantics, YM3812):

| Address   | Meaning                                                    |
|-----------|------------------------------------------------------------|
| 0x01      | global: bit 5 = wave-shaping (non-sine) enable             |
| 0xBD      | global: bit 7 = tremolo depth, bit 6 = vibrato depth       |
| 0x20/0x30 | per-op: bit 7 AM, bit 6 vibrato, bit 5 keyscale-enable,   |
|           |         bit 4 KSR, bits 3-0 = multiplier (1,2,4,6,8,10,12, |
|           |         14,16,18,20,20,24,24,30,30)                        |
| 0x40/0x50 | per-op: bits 6-5 = key-scale level (KSL), bits 5-0 = total |
|           |         level (TL)                                         |
| 0x60/0x70 | per-op: bits 7-4 = attack rate (AR), bits 3-0 = decay (DR)|
| 0x80/0x90 | per-op: bits 7-4 = sustain level (SL), bits 3-0 = release |
| 0xE0/0xF0 | per-op: bits 1-0 = waveform select (0 sine,1,2,3)         |
| 0xA0..0xA8| per-ch: FNUM low 8 bits                                    |
| 0xB0..0xB8| per-ch: bits 1-0 FNUM high, bits 4-2 block (octave), bit 5 |
|           |         key-on                                             |
| 0xC0..0xC8| per-ch: bits 3-1 = feedback, bit 0 = connect (0=FM,1=add) |

`0x00`/`0x08` (and the other per-channel-mode addresses) are written by the
tracks' init sequences but carry no state the chip model needs; writes to
unmapped addresses are ignored.

This map was recovered two independent ways and they agree: the register
decoding of a public IMF→MIDI converter, and the raw register histogram of
every track (119 distinct addresses, all resolved by the table above).
The synthesiser is a from-scratch YM3812 model (log-domain envelope
generator, quarter-sine + exp2 tables, 49716 Hz native rate).

**Cross-validation:** the C++ `Opl2` reproduces the clean-room reference
(uWolf's `opl2.js`, same log-domain model) **sample-for-sample** (max
diff 0.0 over 20,000 native-rate samples of the `CORNER` track, identical
peak/RMS). The IMF sequencer (700 Hz tick, non-zero delay consumes the tick,
loop on end) and the FM effect note clock (140 Hz, zero = key-off) follow the
same reference.

## VGADICT

1024 bytes = 512 × u16. Values observed 0..~259 (high bits used are 9).
Values 256..259 also appear as map-plane-1 markers. Semantics: a 16-bit
lookup table (likely 4-bit-tile → 16-bit-color expansion pairs); not needed
for VSWAP rendering. Deferred.

## VGAGRAPH / VGAHEAD — partially decoded (Phase 5, HUD/menus)

- VGAGRAPH: `[u32 528][12 zero bytes][275770 high-entropy payload]`
  (m1: 588 / 947975). Payload = 528×512 + 5434. Not Carmack/RLEW, no fixed
  tile period, contains 0xFFFF. Raw interpretation is noise.
- VGAHEAD: 450 bytes = 75 × (u16, u16, u16) (m1: 510 = 85 entries).
  Third field is strictly monotonically increasing (1..1077 / 1..3703);
  first two fields carry coordinate-like sub-bytes. Almost certainly the
  index/descriptor table for the VGAGRAPH payload (analogous to
  MAPHEAD/AUDIOHED).
- Plan to finish: correlate VGAHEAD entries with payload regions, then
  render candidate HUD tiles against screenshots of the original HUD.

## CONFIG

522 bytes: `"id software-'92"` + NUL padding + a few small values (0x70, 1…).
Version/config marker only. Deferred until options/menus are needed.

## Map plane value semantics (pinned in Phase 2)

Derived from the reference engine (vpoupet/wolfenstein `game.js`, a public
clean-room reimplementation) and cross-checked against the public map-format
tables (reference repo `docs/files.md`, 1999 XWolf "Map Data Values") and the
shipped data via `tools/analyze_maps.py`.

Plane 0 (wall/walkable):
- `0..63`   wall (blocking). Texture = value (walls live in VSWAP 0..105;
  values 64..101 that appear in plane 0 use the wall index directly, e.g.
  value 21 renders wall texture 40 — the elevator-shaft shaft).
- `90..101` doors (blocking while closed, 64-tick open/close cycle).
  90/91 plain, 92/93 gold-locked, 94/95 silver-locked, 96..101 plain.
- `>63` and not a door (102..143 observed) = open floor.
- `21` is the elevator-shaft wall: activating (pressing "use") with the
  shaft cell in front and the x-axis dominant exits the level.

Plane 1 (objects/actors):
- `19..22`  player spawn, facing N/E/S/W.
- `23..70`  props, sprite = value − 21; most walk-through, 21 of them
  blocking (see `src/game/actors.cpp` `kBlockingProps`); collectibles are
  29 (ammo), 43/44 (gold/silver key), 47..51 (health/ammo), 52..56 (treasures,
  counted in the score).
- `98`      pushwall (secret): 2 × 64-tick cell move; counts in totalSecrets.
- `108..141, 144..177, 216..223, 234..241` regular enemies, 8 values per
  band (0-3 standing, 4-7 patrolling — behaviorally identical in the
  reference: enemies are static). Direction = (value − band base) % 4,
  except the 216-223 zombie band, which the reference computes as
  (value − 116) % 4 (mirrored in `actor_direction`).
- `124`     dead guard (decorative sprite 95).
- `160, 178, 179, 196, 197, 214, 215` bosses (always face the player).
- `224..227` pac-man ghosts, 2-frame loop, not killable.
- `256..259` special markers (see the Phase 1 note); inert in the simulation.
- `71..74` (SoD static objects), `106, 107, 125, 142, 143, 161` (SoD
  enemies with no reference class), `180..213` (hard-difficulty regulars)
  and `252..259` (hard-difficulty mutants) are present in the shipped data
  but unhandled by the reference → inert (`kUnknown`).

UNRESOLVED: `90..97, 99` in plane 1. Present in both games (1621 base /
548 SOD cells) in small clusters of 8 subtypes on open cells, often near
enemies; not documented in any public spec found and not handled by the
reference. Treated as inert; re-check in the Phase 3 screenshot comparison.

## Simulation simplifications (Phase 2)

The headless sim (`src/game/sim.*`) mirrors the reference update loop with
these deliberate differences, each pinned by a golden test in
`tests/test_sim.cpp`:

- The reference fires activate/shoot from key events; the sim processes the
  per-tick input flags at a fixed point (after movement), so a held
  "activate" re-triggers once per tick (an open door re-closes if the press
  is still held on the tick it opens — key-event input never does that).
- Enemies have no AI (true of the reference): they stand facing their spawn
  direction and die in one shot via the center ray (|perpendicular offset|
  ≤ 0.3, no spread, no hitpoints).
- The LOS ray is a grid DDA over blocking cells (walls, closed doors,
  blocking props, map bounds) — the boundary convention is equivalent to the
  reference's door-center-line geometry for all killable cases.
- m1 90..99 cells are inert (see above).
- Level exit freezes the player (reference: loads the next level).

## Open items (do NOT block the renderer)

1. VGAGRAPH/VGAHEAD exact layout (Phase 5 — HUD & menus).
2. Meaning of VGADICT table (Phase 5, if 4-bit tiles ever needed).
3. The 3 mystery bytes after the GAMEMAPS magic.
4. Plane 1 values 90..97/99 (inert in the sim; see "Map plane value
   semantics" — check rendering in Phase 3).
5. PCM SFX sample rate (assumed 11025 Hz; unconfirmed — no clean-room
   reference decodes the digitised SFX).
