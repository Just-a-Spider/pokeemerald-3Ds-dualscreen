# Pokémon Emerald 3Ds Dual Screen

A native Nintendo 3DS port of Pokémon Emerald that uses both screens: the
game on the top screen with an optional 3D (voxel) overworld, and a touch
interface on the bottom screen that replaces the START menu.

**This repository contains no game content.** It holds the port's own code,
the tools that build it and the builder that turns *your own* cartridge dump
into the game's data pack. Nothing derived from the ROM is distributed here or
in the releases.

- Engine: [pret/pokeemerald](https://github.com/pret/pokeemerald) decompilation,
  pinned in [`upstream.lock`](upstream.lock), plus the port's changes in
  [`patches/pokeemerald/`](patches/pokeemerald).
- Port: [`3ds_port/`](3ds_port) — ARM11 backend (libctru, Citro2D/Citro3D),
  GPU compositor, NDSP audio, bottom-screen UI, voxel overworld.
- Data: generated on the player's computer by the
  [Pokémon Emerald 3Ds Dual Screen Builder](builder/) from a Pokémon Emerald (USA, Europe) ROM.

## Playing

You need a 3DS/2DS family console with custom firmware (Luma3DS) and the
Homebrew Launcher, and a clean dump of your own Pokémon Emerald (USA, Europe)
cartridge (SHA-1 `f3ae088181bf583e55daf962a92bb46f4f1d07b7`).

1. Download `Emerald3DS-vX.Y.Z-Windows.zip` from the releases and extract it.
2. Run `Emerald3DS-Builder.exe`, choose your ROM and your SD card, press
   **Install**.
3. Start **Pokémon Emerald 3Ds Dual Screen** from the Homebrew Launcher.

The builder writes `/3ds/emerald3ds/Emerald3DS.3dsx`, `Emerald3DS.smdh` and
`emerald3ds.pak`. The ROM is only read: it is not copied, uploaded or modified,
and no Internet connection is involved. Each release comes with its own
builder; after updating, run it again (the game tells you when the data pack
does not match the release).

On Linux and macOS the builder runs from source:
`python -m emerald3ds_builder install --rom <ROM> --sd <card>` (see
[builder/](builder/)).

## Building from source

```
python tools/bootstrap.py        # pinned upstream + patches + port -> build/upstream
python tools/bootstrap.py --make # also builds the 3DSX there
```

Requirements, the development loop (loose data, data packs, host tests) and
the release process are in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) and
[docs/RELEASING.md](docs/RELEASING.md). How the pieces fit together is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/ASSET_PIPELINE.md](docs/ASSET_PIPELINE.md).

## Provenance and licences

Only original Pokémon Emerald 3Ds Dual Screen code, tools and documentation are licensed by this
project ([LICENSE-PORT.md](LICENSE-PORT.md)). The decompilation, the game and
third-party components keep their own terms; see [NOTICE.md](NOTICE.md) and
[docs/PROVENANCE.md](docs/PROVENANCE.md). AI-assisted tooling was used during
development: [AI_DISCLOSURE.md](AI_DISCLOSURE.md).

Pokémon Emerald 3Ds Dual Screen is an unofficial fan project, not affiliated with or endorsed by
Nintendo, Game Freak, Creatures or The Pokémon Company. Pokémon and Pokémon
Emerald are trademarks of their respective owners.
