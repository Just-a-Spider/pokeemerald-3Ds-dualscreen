# Changelog

## Unreleased — first public version

- Native ARM11 port of pokeemerald for Nintendo 3DS: GPU compositor at
  native 400x240, NDSP audio, touch and Circle Pad input, SD saves.
- Bottom-screen interface replacing the START menu (map, party, bag, trainer
  card, Pokédex, PokéNav, save, options) and touch battle menus.
- Optional voxel overworld with buildings, trees, signposts and terrain relief
  modelled from each map's own art, fixed-sun lighting and cast shadows.
- Game data outside the executable: embedded (development), loose files or a
  single `emerald3ds.pak` with ABI and integrity checks.
- Pokémon Emerald 3Ds Dual Screen Builder: generates the data pack from the player's own ROM and
  installs the game on an SD card, with no toolchain or Python required on
  Windows.
