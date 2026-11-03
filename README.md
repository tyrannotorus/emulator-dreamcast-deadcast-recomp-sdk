# Deadcast recomp SDK

The engine and port layer for Deadcast's recomp modules. A recomp module is a recompiled build of one Dreamcast
game for the TrimUI Brick: the game's SH-4 code translated to C++ at build time from the builder's own disc, and
played under Deadcast in place of its emulation. A module is its own repository, with this one as a submodule (`sdk/`),
released as a tarball.

## What every module follows

1. **No in-game settings or menu changes.** The game's own menus stay as the original has them. Whatever a module
   lets the player choose is a setting declared in its manifest, shown in Options.
2. **A module, not a replacement.** Deadcast stays running and hosts the module as a child process. MENU, the menu, Recomp
   Options and the Recomp Store are Deadcast's. The module is the game: it draws, plays sound, and pauses, resumes and quits
   when told to, through the port layer (`src/framework/host_link.cpp`).
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
id = mygame               unique; the folder it installs in
name = My Game
title = My Game           what the menu calls it
version = 1.0.0
exec = mygame             the executable, beside the manifest
host = 1                  the format Deadcast hosts it in; without this it is not run
needs = 1.2.0             the oldest Deadcast it runs under
disc = 0123abcd...        each disc it plays: the SHA-1 its CHD's header records (one line each)
setting = scale | Resolution | 1=640x480, panel=1024x768 | panel
                          each setting: key | name | value=label, ... | default
```

Deadcast shows them in Options, with Core on the module, and keeps the values, and hands every value to the module as it starts, as
`--set <key>=<value>`, a port layer setting (`dreamcomp::settings()`). The port layer applies `scale` and `rumble` (0-100)
itself, live. **Show FPS** is always the first row there, undeclared.

### Buttons

The port layer plays the game on this layout, with nothing declared:

| Control | Key | Default |
|---|---|---|
| D-pad | `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right` | `UP`, `DOWN`, `LEFT`, `RIGHT` |
| A, B, X, Y | `a`, `b`, `x`, `y` | `A`, `B`, `X`, `Y` |
| Start | `start` | `START` |
| L, R triggers | `ltrigger`, `rtrigger` | `L1,L2` and `R1,R2` |
| Stick | `stick_up`, `stick_down`, `stick_left`, `stick_right` | none |

To let the player remap them, declare each control they may change in `module.cfg`, with its default:

```
control = a | A Button | A      key | name | the pad's inputs, comma separated, or NONE
```

Options then has a **Controls** page for the module, drawn by Deadcast, as the emulator's is: A waits for a press, X
clears, and a press taken leaves any other declared control that had it. The inputs are `A B X Y L1 R1 L2 R2 SELECT START
FN1 FN2 UP DOWN LEFT RIGHT`, by the labels on the device. MENU is Deadcast's and cannot be bound, and Deadcast's menu keeps
its own buttons whatever the game's are. A control not declared stays on its default, and a module that declares none has
no page. The values reach the module like its settings, as `control.<key>`, and apply at once. They are the module's alone:
nothing comes from the emulator's Controls, and its defaults differ (there, the Dreamcast's A is the button labelled B).

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
sdk/tools/toolchain/toolchain.sh
sdk/tools/toolchain/devlibs.sh <adb serial>
sdk/tools/build-module.sh /path/to/your/disc.chd
sdk/tools/pack-module.sh
```

`build-module.sh` makes `build-brick/<id>/`, what Deadcast installs. `pack-module.sh` makes the tarball for its
release. The toolchain needs `docker pull ghcr.io/loveretro/tg5040-toolchain:latest` first.

## Getting listed

1. Open a pull request adding your repository to Deadcast's `store/registry.txt`.
2. Publish a GitHub release with the tarball attached. Tag it with the version in `module.cfg`, and raise that
   version for every release.

The store's list picks up your latest release the next time it is rebuilt: on a schedule of an hour, which GitHub does not
guarantee, so it can take longer.

## Licences

GPL-2.0 (`LICENSE`); `THIRD_PARTY_NOTICES.md` lists what it builds on.
