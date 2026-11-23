// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string_view>

class Error;

// Boot shortcut: drives the game from power-on to a fixed point (Fate/unlimited codes: "Vs. 2P"
// character select) with a scripted input sequence, fast and without user input. Netplay peers each
// boot their own copy of the game this way instead of sharing a savestate. Emulation is deterministic,
// so both reach the same state; netplay starts its session once the shortcut has finished.
//
// While a shortcut is requested, the boot uses a fixed real-time clock (the game seeds its RNG from it)
// and fast boot. Netplay additionally runs without memory cards and with default NVRAM.
//
// Script: tokens separated by spaces or ';'
//   w<N>                 wait N frames, nothing pressed
//   <BTN>[+<BTN>][:<N>]  press buttons on port 1 for N frames (default 3), e.g. START, CIRCLE:5, UP+CIRCLE
//                        buttons: UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 L3 R3 START SELECT
//   2.<BTN>...           same on port 2
//   poke8|poke16|poke32:<addr>=<value>   write EE memory (hex), e.g. poke8:3a1b20=ff
//   shot                 screenshot to logs/bootshortcut (diagnostics)
//   dumpram              write EE RAM to logs/bootshortcut (diagnostics)
//   name=<NAME>          local player name; writes the HUD name patch for P1 when reached (tests)
//   shots=<N>            screenshot every N frames (diagnostics)
//   slow                 run at normal speed instead of fast (diagnostics)
// Built-in scripts for Fate/unlimited codes: "vs2p" ("Vs. 2P" character select) and "practice"
// ("Practice" character select), both with all characters unlocked.
//
// Memory cards stay off for the whole session started with a shortcut (the script expects the game's
// "no memory card" prompt, and card contents differ between machines).
namespace BootShortcut
{
	/// Requests a shortcut for the next boot. Returns false (with error) if the script is invalid.
	bool Request(std::string_view script, Error* error);

	/// True from the request until the shortcut has finished or was aborted.
	bool IsActive();

	/// True while the shortcut drives the game (called at the input poll, before netplay).
	bool IsRunning();

	/// Input poll of a frame while running.
	void OnInputPolled();

	/// Cancels a running or pending shortcut (state load, reset, shutdown).
	void Abort();

	/// True from the request until the VM shuts down: no memory cards in this session.
	bool MemoryCardsOff();

	/// VM shutdown: aborts and ends the session (memory cards available again).
	void OnShutdown();
} // namespace BootShortcut
