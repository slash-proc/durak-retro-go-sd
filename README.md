# Duren — Durak for Game & Watch Retro-Go SD

GWHB homebrew port of **Duren** (Durak card game) for
[Game & Watch Retro-Go SD](https://github.com/sylverb/game-and-watch-retro-go-sd).

Built on the freestanding Cortex-M7 core SDK (`gw_firmware_abi_t`). One tree =
one binary: `Duren.bin`, plus a combined resource pack `Duren.pak`.

## Credits

**Duren** is an original homebrew by **Seemerun** (itch.io: **abigas**). He
shared the source so it could be ported to a standalone GWHB binary for
Retro-Go SD.

- Game page: [Duren on itch.io](https://abigas.itch.io/duren-durak-card-game-for-game-and-watch-retro-go)
- Author: [abigas](https://abigas.itch.io) · Discord: **Seemerun**

Unofficial homebrew; not affiliated with or endorsed by Nintendo.

## About the game

A dark, sarcastic Durak experience: opponents do more than play — they mock
you, provoke you, and celebrate every mistake. Twenty different characters
wait at the table (knights, demons, professors, gamblers, strange creatures…).
Each has a unique portrait, personality, voice, dialogue, and play style.

Atmosphere: a dim table, weak light, dust in the air, and strange figures
watching every card you play.

### Features

- 20 unique characters with portraits, voices, personalities, and reactions
- Classic Durak (“fool”) rules — last player holding cards loses
- Campaign with gradually unlocked opponents (fog-of-war map)
- Tournament bracket
- “Online” random battles (local AI only — see below)
- Battle Royal for 3–4 players
- Multiple difficulty levels
- Animated dealing, attacks, defense, pickup, and discard
- Original music, voice lines, and sound effects
- Table lighting, shadows, particles, and visual effects
- Save / load system
- Designed for the 320×240 Game & Watch display
- Graphics, audio, and language packs on microSD (`Duren.pak`)
- 8 languages: English, Polish, Ukrainian, German, Spanish, French, Italian,
  Portuguese

### How a Durak hand works

- A **trump suit** is chosen (bottom card of the remaining deck, face up).
- Players take turns **attacking** by playing a card onto the table.
- The defender must **beat** each attack with a higher card of the same suit,
  or any trump.
- Extra attacks may be added only with ranks already on the table (and within
  the attack limit).
- If the defender beats every card, the table is cleared and they become the
  next attacker. If they cannot (or choose to) take, they pick up the table
  cards and stay the defender for the next attack.
- After each bout, players draw back up from the deck (attacker first).
- When the deck is empty, play continues until someone empties their hand.
  The last player still holding cards is the *durak*.

This homebrew follows classic “podkidnoy” (throw-in) style, with optional deck
styles and difficulty settings.

### Modes

| Mode | What it is |
|------|------------|
| **Career** | Travel a fog-of-war map and beat 20 opponents in sequence. Progress is saved. |
| **Online** | Menu label for a one-off 1v1 duel. No network: opponent is a **local AI** with a generated bot name (pseudo-online). |
| **Tournament** | 8-player bracket (quarters → semis → final). Win the cup or get eliminated. |
| **Battle Royal** | Up to 4 players at one table (you + AIs), money stakes, multi-round until a champion remains. |

On device, use the in-game options / pause menus; **PAUSE** opens the Retro-Go
system menu.

## Install (device)

Copy **both** files onto the SD card under `/homebrews/`:

| File | Role |
|------|------|
| `homebrews/Duren.bin` | GWHB binary |
| `homebrews/Duren.pak` | Graphics + audio + language data (~5.6 MiB) |

The game opens **only** `/homebrews/Duren.pak` (same folder as the `.bin`).
If the pack is missing, an on-screen error is shown and **PAUSE** still opens
the Retro-Go menu.

Requires firmware whose ABI matches `SDK_VERSION` in this repository.

Release zips already use that layout — unzip onto the SD card root.

## Requirements

**Device build**

- `arm-none-eabi-gcc` (v10+, hard-float `fpv5-d16`)
- GNU Make
- Python 3 + Pillow (`pip install -r requirements.txt`) for the cover JPEG

**Host SDL preview** (optional, Linux / macOS)

- Native C compiler, pkg-config, SDL2 (`brew install sdl2` / `libsdl2-dev`)

**Docker** (no host ARM toolchain)

- Docker + image [`sylverb/retro-go-sd-builder`](https://hub.docker.com/r/sylverb/retro-go-sd-builder)
  (default tag `v1.5`)

## Build

```bash
make                          # → Duren.bin + Duren.pak
make resources                # rebuild Duren.pak only
make docker                   # same build in the builder image
```

Packed header version comes from `git describe --tags --dirty` (`CORE_VERSION`).
No tags → `NOTAG` → header `0.0.0`.

Outputs at the repo root:

- `Duren.bin` — packed GWHB
- `Duren.pak` — combined pack (`gfx` / `audio` / `lang` members)

Inner sources for the pack live under `sd_content/roms/homebrew/duren_{gfx,audio,lang}.pak`
and are assembled by `scripts/pack_duren_pak.py`.

## Host preview (SDL)

Desktop build for faster iteration (does not replace the ARM pack):

```bash
make host                     # → ./Duren_host
make host_run                 # build and launch
```

Assets are loaded from `host_data/` (PNG atlases + a copy of `Duren.pak`).
`make resources` refreshes `host_data/Duren.pak`.

On macOS, if `pkg-config sdl2` fails:

```bash
export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
```

Controls: arrows = D-pad, `Z`/`X` = B/A, Enter = Start, Esc = quit.

## Layout

```
Makefile                 Homebrew build + pack + docker
src/main.c               GWHB entry (app_main)
src/hb_compat.c          Small freestanding stubs
src/duren/               Game, HAL (device + SDL), packs, modes, UI
src/assets/cover_src.png Cover art source
sd_content/roms/homebrew/  Inner gfx/audio/lang packs (inputs to Duren.pak)
host/                    SDL host glue
host_data/               PNGs + Duren.pak for desktop runs
scripts/                 pack_duren_pak.py, release staging, SDK sync
sdk/                     Vendored Retro-Go SD core SDK
```

## Releases (GitHub tags `v*`)

Pushing a tag `vX.Y.Z` (with a matching `## [vX.Y.Z]` section in
`CHANGELOG.md`) creates a GitHub Release with two zip assets:

| Asset | Contents |
|-------|----------|
| `Duren-vX.Y.Z.zip` | `homebrews/Duren.bin` + `homebrews/Duren.pak` |
| `Duren-vX.Y.Z-debug.zip` | ELF, map, short README for `addr2line` |

Unzip the install archive onto the SD card root. For a crash PC/LR:

```bash
unzip Duren-vX.Y.Z-debug.zip
arm-none-eabi-addr2line -e duren_core.elf -f -C -a 0x<PC> 0x<LR>
```

## ABI compatibility

The binary embeds `required_abi_version` / `required_abi_min_size` from the
SDK bridge. The firmware refuses to load a binary that asks for a newer/larger
ABI than it provides. See `SDK_VERSION` for the snapshot this tree was cut from.

## Refreshing the SDK from firmware

```bash
./scripts/sync_from_firmware.sh /path/to/game-and-watch-retro-go-sd
```

Review the diff before committing. Porting / memory notes for this SDK live in
`CLAUDE.md`.

## License

Build glue and project sources for this GWHB port are MIT (see `LICENSE`).
Vendored files under `sdk/include/` keep their upstream licenses (firmware /
HAL / FatFs / etc.). Game design, art, audio, and language content belong to
the original author (see [Credits](#credits)); follow the terms on the
[itch.io page](https://abigas.itch.io/duren-durak-card-game-for-game-and-watch-retro-go).
