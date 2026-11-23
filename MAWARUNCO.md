# Mawarunco

Rollback netplay for **Fate/unlimited codes** (PS2, SLPM-55108), built as an
unofficial fork of [PCSX2](https://github.com/PCSX2/pcsx2) v2.8.2.

Mawarunco is **not affiliated with or supported by the PCSX2 team**. Please do
not report problems with it to the PCSX2 project. Fate/unlimited codes is a
trademark of its owners; this project is not affiliated with Capcom, TYPE-MOON
or cavia.

All changes are on the `rollback` branch. This repository contains **only
source code**: no BIOS, no disc image, no game executable and no game data.
Players need their own PS2 BIOS and their own copy of the game.

## What it adds to PCSX2

- **Rollback netplay** over direct IP, built on [GGRS](https://github.com/gschup/ggrs)
  (Rust, through a small C wrapper in `3rdparty/ggrs-capi`).
  - The host decides input delay (0–8) and rollback depth (1–8).
  - Both sides check that they run the same program version.
  - Checksums detect desyncs; on a desync, both sides log per-block memory
    hashes for diagnosis.
- **Rollback core** (`pcsx2/Rollback/Rollback.*`): in-memory snapshots, about
  1.5 ms to save and 2 ms to restore, bit-identical on replay. About a dozen
  determinism problems in emulation and savestates were fixed for that; the
  commit messages describe each one.
- **Run-ahead**: one frame by default, offline and in netplay. It hides the
  game's internal input lag. Together with presenting each frame before the
  frame limiter sleeps, input-to-screen latency went from 33.6 ms to 10.6 ms.
- **Boot shortcut**: each player boots their own copy straight to the
  character select of "Vs. 2P" or "Practice", with all characters unlocked.
  No savestate is exchanged.
- **Game-specific features**, written into game memory at runtime:
  - the game's own button config per player, set once in the start window;
  - player names next to the win counter in the versus HUD.
- **Start window** (`pcsx2-qt/RollbackLauncher.*`) instead of the PCSX2 main
  window in the player package: online (host/join), training, button config,
  display (4:3 resolutions, window/fullscreen).
- **Installer** (Inno Setup, `tools/rollback/installer`): per user, no admin
  rights; asks for the player's BIOS and disc image.
- The **PCSX2 auto updater is disabled**: it would replace this build with an
  official release.

## Security and transparency

The repository is public so that anyone can check what the program does.
In short:

- **Network:**
  - The only network code added is netplay, active only during an online
    session.
  - It opens one UDP port: 7000 on the host; any free port when joining.
  - Over it go a short handshake (program version, input delay, rollback
    depth) and then GGRS messages, which carry controller input (18 bytes per
    player and frame) and checksums.
  - Received input is only used as button and stick values. It never becomes
    a memory address, file name or command.
  - Code: `3rdparty/ggrs-capi/src/lib.rs` (socket, handshake),
    `pcsx2/Rollback/Netplay.cpp` (session, input handshake for button config
    and names).
- **Nothing is downloaded or started:** no other programs, no registry
  changes, no autostart. The PCSX2 auto updater is off
  (`pcsx2-qt/AutoUpdaterDialog.cpp`). PINE, PCSX2's local memory-access
  interface, is off in the player package.
- **Files:** besides PCSX2's usual folders (settings, logs, memory cards),
  the program writes only diagnostic dumps into its own `logs` folder.
  The start window copies the BIOS the player chooses into the `bios`
  folder.
- **Game memory changes:**
  - The boot shortcut and the game features write a few values into
    emulated PS2 memory: character unlock flags, the button config, and a
    small HUD routine for player names. That routine is MIPS code for the
    emulated PS2, not for the PC.
  - Code: `pcsx2/Rollback/BootShortcut.cpp`, `pcsx2/Rollback/FucButtons.cpp`,
    `pcsx2/Rollback/FucNames.cpp`.
- **Releases:** every release is built from a tagged commit of this
  repository, and its SHA-256 checksum is published with it.

## Playing

The easiest way is the installer from the releases. It asks for your BIOS
and your disc image. Then start **Mawarunco**:

- **Host:** forward UDP port 7000 in your router and click HOST.
- **Join:** enter the host's IP address and click JOIN.

The command line still works for testing:

```
pcsx2-qtx64-clang.exe -batch -netplay "host=7000,delay=1,rollback=2,boot" -- game.iso
pcsx2-qtx64-clang.exe -batch -netplay "join=<host ip>:7000,delay=1,rollback=2,boot" -- game.iso
```

## Building (Windows)

Same as PCSX2, plus a Rust toolchain:

1. Dependencies: `.github/workflows/scripts/windows/build-dependencies.bat`.
2. Rust: a normal [rustup](https://rustup.rs) install (`cargo` on PATH), or set
   `RollbackRustDir` in an untracked `3rdparty/ggrs-capi/ggrs-capi.local.props`.
3. `msbuild PCSX2_qt.sln "/p:Configuration=Release Clang" /p:Platform=x64`.
4. Package, patch and installer:
   `tools/rollback/make-netplay-package.ps1 -ReferenceBios <bios> -Update -Installer`.
   Inno Setup 6 is needed for the installer.

## How it was made

Mawarunco was developed with the help of an AI coding assistant (Claude, by
Anthropic). Every commit made this way is marked "(AI-assisted)".

## License

GPL-3.0-or-later, like PCSX2. The GGRS wrapper in `3rdparty/ggrs-capi` is
MIT OR Apache-2.0, like GGRS.
