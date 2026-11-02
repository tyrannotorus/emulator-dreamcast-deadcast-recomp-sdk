# Deadcast recomp SDK

The engine and port layer for Deadcast's recomp modules. A recomp module is a recompiled build of one Dreamcast
game for the TrimUI Brick: the game's SH-4 code translated to C++ at build time from the builder's own disc, and
played instead of Deadcast's emulation. A module is its own repository, with this one as a submodule (`sdk/`),
released as a tarball.

## What every module follows

1. **No in-game settings or menu changes.** The game's own menus stay as the original has them. Whatever a module
   lets the player choose is a setting declared in its manifest, shown in Recomp Options.
2. **Deadcast's menu, not one of its own.** MENU opens the shared menu (`ui/`) over the held game, through the port
   layer (`src/framework/deadcast.cpp`). Save and Load, the emulator's Controls and Local Multiplayer are disabled.
3. **No game data in git.** No disc images, nothing extracted from them and no translated code.
4. **The disc is checked.** The manifest lists every disc the module plays, by the SHA-1 its CHD's header records.
   Deadcast offers the module only for those discs, and the engine checks again before it runs.

## A module's repository

```
module.cfg          the manifest
store.json          what its page in the Recomp Store says
README.md           short: the game, Supported Formats, Features, Files, Building, Credits
game/<id>.toml      the game: its discs, its boot file, relocations, hooks, functions
game/CMakeLists.txt dreamcomp_add_port(<id> ...) with the module's own sources
src/                native code replacing the game's libraries, and the module's hooks
sdk/                this repository, a submodule
```

Ignore `build-brick/` and `game/extracted/`.

### module.cfg

```
id = rez                  unique; the folder it installs in
name = Rez
title = Rez Recompiled    what the menu calls it
version = 1.0.3
exec = rez                the executable, beside the manifest
disc = 9711a0d0...        each disc it plays: the SHA-1 its CHD's header records (one line each)
setting = scale | Resolution | 1=640x480, panel=1024x768 | 1
                          each setting: key | name | value=label, ... | default
```

The port layer applies `scale` itself, live. Any other setting a module reads from the store,
`deadui::recomp::get("<id>.<key>")`. **Show FPS** is always the first row of Recomp Options, undeclared.

### store.json

Every key is optional. The changelog is for the version in `module.cfg`, and `formats` lists the same discs:

```json
{
  "description": "The game in a line or two.",
  "formats": [{ "format": "CHD", "region": "Japan", "sha1": "9711a0d0..." }],
  "features": ["One line for each thing it adds."],
  "changelog": ["One line for each change in this version."],
  "repo": "https://github.com/..."
}
```

## How Deadcast runs a module

Installed modules are in `<DEADCAST_HOME>/recomp/<id>/`. When a game's core is a module, Deadcast's first act is to
`exec` it, `<exec> <disc>`, so the emulator never starts. A module keeps its files (settings, memory card, caches)
in `<DEADCAST_HOME>/recomp-data/<id>/`, and nothing outside those two folders: deleting it removes both. Exit code
86 starts the game again in the core now chosen.

## Building a module

From the module's repository, with your own disc:

```
git submodule update --init --recursive
sdk/ui/toolchain/toolchain.sh
sdk/ui/toolchain/devlibs.sh <adb serial>
sdk/tools/build-module.sh /path/to/your/disc.chd
sdk/tools/pack-module.sh
```

`build-module.sh` makes `build-brick/<id>/`, what Deadcast installs. `pack-module.sh` makes the tarball for its
release. The toolchain needs `docker pull ghcr.io/loveretro/tg5040-toolchain:latest` first.

## Licences

GPL-2.0 (`LICENSE`); `THIRD_PARTY_NOTICES.md` lists what it builds on.
