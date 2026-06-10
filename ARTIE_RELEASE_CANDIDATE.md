# Artie Alive — Release Candidate

## Build

| Metric | Value |
|--------|-------|
| Flash used | 7831 / 8192 bytes (95.6%) |
| Flash free | 361 bytes |
| Warnings | 0 |
| Errors | 0 |
| Hex path | `.build/out/firmware.ino.hex` |
| Target | ATtiny814, megaTinyCore 2.6.11 |
| FQBN | `megaTinyCore:megaavr:atxy4:chip=814` |

## Active Modes

| Mode | Name | Description |
|------|------|-------------|
| 0 | Artie Alive | Animated character with idle emotes, touch reactions, demo |
| 1 | Breath Red | Slow red breathing effect |
| 2 | Looping Eyes Blue | Blue looping pattern |

Short press cycles: 0 → 1 → 2 → wrap to 0.

## Touch Mapping (Mode 0 Only)

| Pad | Mask | Reaction |
|-----|------|----------|
| Left Blue | 0x01 | Forced sleepy (blue lower arc pulse) |
| Right Blue | 0x20 | Forced wink (60-frame held cycle) |
| Left Red | 0x02 | Forced alert (full-eye red pulse) |
| Right Red | 0x10 | Forced suspicious (amber L/R fast) |
| Left Green | 0x04 | Forced excited (green top/full bounce) |
| Right Green | 0x08 | Forced happy (green top arc) |

Touch-only enters reaction. Physical button exits forced state.

## Hidden Demo

**Trigger:** Button + all three right-side pads simultaneously (mask `0x38`)

Plays 15 emotes sequentially (~47s total), then returns to normal idle.
Button press during demo cancels and returns to calm gap.

## Game Combos

| Combo | Game |
|-------|------|
| Button + Left Blue | Stop The Light |
| Button + Left Red | Find The Sequence |
| Button + Left Green | Follow The Sequence |

## Sleep

Long press (any state) → sleep. Wake via button press (full reboot).

## Conference Test Checklist

- [ ] Cold boot: wake-up animation (blue pulse → teal → look → settle)
- [ ] Idle 30s: varied emotes with teal gaps between
- [ ] Touch L-Blue: sleepy reaction, held while touching
- [ ] Touch R-Blue: wink reaction
- [ ] Touch L-Red: alert reaction
- [ ] Touch R-Red: suspicious reaction
- [ ] Touch L-Green: excited reaction
- [ ] Touch R-Green: happy reaction
- [ ] Short press while forced: exits to calm, no mode change
- [ ] Button + L-Blue: Stop The Light game
- [ ] Button + L-Red: Find The Sequence game
- [ ] Button + L-Green: Follow The Sequence game
- [ ] Button + all R pads: demo plays through all emotes
- [ ] Short press during demo: cancels demo
- [ ] Long press: sleep (LEDs off)
- [ ] Button press after sleep: full wake-up
- [ ] Short press (not forced): cycles mode 0→1→2→0
- [ ] Touch in mode 1/2: pad LEDs flash briefly, no reaction
- [ ] 5-minute stability: no lockups, no stuck states
