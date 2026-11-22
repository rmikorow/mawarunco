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
- **Self-test and benchmark** (`pcsx2/Rollback/RollbackTest.*`) for
  determinism.

## Playing

Both players need their own BIOS and their own copy of the game. Neither is
part of this repository. Both start the same savestate with identical
settings:

```
pcsx2-qtx64-clang.exe -batch -netplay "host=7000,delay=1,rollback=2" -statefile start.p2s -- game.iso
pcsx2-qtx64-clang.exe -batch -netplay "join=<host ip>:7000,delay=1,rollback=2" -statefile start.p2s -- game.iso
```

Both controller ports must be set to DualShock 2. Memory cards are off during
netplay. `tools/rollback/make-netplay-package.ps1` builds a ready-to-use
package with launcher scripts.

## Building (Windows)

Same as PCSX2, plus a Rust toolchain:

1. Dependencies: `.github/workflows/scripts/windows/build-dependencies.bat`
2. Rust: a normal [rustup](https://rustup.rs) install (`cargo` on PATH), or set
   `RollbackRustDir` in an untracked `3rdparty/ggrs-capi/ggrs-capi.local.props`.
3. `msbuild PCSX2_qt.sln "/p:Configuration=Release Clang" /p:Platform=x64`

## License

GPL-3.0-or-later, like PCSX2. The GGRS wrapper in `3rdparty/ggrs-capi` is
MIT OR Apache-2.0, like GGRS.
