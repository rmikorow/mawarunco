# PCSX2 rollback fork (Fate/unlimited codes)

An unofficial fork of [PCSX2](https://github.com/PCSX2/pcsx2) v2.8.2 that adds
rollback netplay, built for **Fate/unlimited codes** (PS2, SLPM-55108). It is
not affiliated with or supported by the PCSX2 team. Please do not report
problems with this fork to the PCSX2 project.

All changes are on the `rollback` branch.

## What it adds

- **Rollback core** (`pcsx2/Rollback/Rollback.*`): in-memory snapshots (~1.5 ms)
  and restores (~2 ms) that are bit-identical. Several determinism problems
  in emulation and savestates had to be fixed for that. The commit messages
  describe each one.
- **Controller input injection**: 18 bytes of DualShock 2 input per port and
  frame. While a session runs, the game only sees the injected input.
- **Netplay** (`pcsx2/Rollback/Netplay.*`) on top of
  [GGRS](https://github.com/gschup/ggrs) (Rust, through the small C wrapper in
  `3rdparty/ggrs-capi`):
  - Direct IP: the host forwards one UDP port; the joining player needs
    nothing.
  - Selectable input delay and rollback depth.
  - Desync detection with checksums over the gameplay state.
  - Time synchronization between the peers.
- **Boot shortcut** (`pcsx2/Rollback/BootShortcut.*`): boots the game from
  power-on with a scripted input sequence, fast and without user input,
  straight to the "Vs. 2P" character select with all characters unlocked.
  Each player boots their own copy; no savestate is exchanged. Emulation is
  deterministic, so both arrive at the same state.
- **Run-ahead** (`pcsx2/Rollback/RunAhead.*`): one frame, offline and in
  netplay, hides the game's internal input lag.
- **Self-test and benchmark** (`pcsx2/Rollback/RollbackTest.*`) for
  determinism.
- The PCSX2 auto updater is disabled: it would replace this build with an
  official release.

## Playing

Both players need their own BIOS and their own copy of the game. Neither is
part of this repository. Both need the same BIOS version, the same game and
identical settings:

```
pcsx2-qtx64-clang.exe -batch -netplay "host=7000,delay=1,rollback=2,boot" -- game.iso
pcsx2-qtx64-clang.exe -batch -netplay "join=<host ip>:7000,delay=1,rollback=2,boot" -- game.iso
```

The session starts once both have finished the boot shortcut. If the two
states differ (different BIOS or game), desync detection reports it at the
first check.

Both controller ports must be set to DualShock 2. Memory cards are off during
netplay, and the clock and NVRAM use fixed defaults.
`tools/rollback/make-netplay-package.ps1` builds a ready-to-use package with
launcher scripts.

## Building (Windows)

Same as PCSX2, plus a Rust toolchain:

1. Dependencies: `.github/workflows/scripts/windows/build-dependencies.bat`
2. Rust: a normal [rustup](https://rustup.rs) install (`cargo` on PATH), or set
   `RollbackRustDir` in an untracked `3rdparty/ggrs-capi/ggrs-capi.local.props`.
3. `msbuild PCSX2_qt.sln "/p:Configuration=Release Clang" /p:Platform=x64`

## License

GPL-3.0-or-later, like PCSX2. The GGRS wrapper in `3rdparty/ggrs-capi` is
MIT OR Apache-2.0, like GGRS.
