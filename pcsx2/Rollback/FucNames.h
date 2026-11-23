// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <string>
#include <string_view>

// Fate/unlimited codes (SLPM-55108): player names in the fight HUD. In the versus modes the game draws
// each player's win count ("  0WIN") above the health bar, where practice shows "PRACTICE MODE". A small
// patch in the label routine (FUN_00333920) shows "<name> / <win count>" when the player has a name.
//
// The patch: the two calls of the text drawing function (0x2146b0) in the label loop are redirected to a
// stub in EE RAM that is unused while the game runs (0x9xxxx-0xFxxxx stays zero). In versus modes (game
// mode 3 or 4) the stub puts the name of the loop's player (register s4) in front of the game's text in
// the buffer (t1), separated by " / " (the font has no '|'), drawn narrower for long names, then jumps on to the original function. Netplay writes patch and names identically on both peers.
namespace FucNames
{
	/// Longest name.
	static constexpr size_t MAX_LENGTH = 20;
	using Name = std::array<char, MAX_LENGTH>; // NUL padded
	/// A name in 15 bytes (6 bits per character), for the netplay handshake.
	using Packed = std::array<u8, (MAX_LENGTH * 6 + 7) / 8>;
	Packed Pack(const Name& name);
	Name Unpack(const Packed& packed);

	/// Keeps only characters the game's font has (upper-case letters, digits, a few symbols), upper-cases
	/// letters and cuts to MAX_LENGTH.
	std::string Sanitize(std::string_view text);
	Name ToName(std::string_view text);

	/// Name of the local player (set by the launcher before a boot).
	void SetLocal(std::string_view name);
	const Name& GetLocal();

	/// Installs the HUD patch and writes both names (empty = the game's own text). CPU thread, between
	/// frames. Returns false if the game code is not the expected one (then nothing is written).
	bool Apply(const Name& p1, const Name& p2);
} // namespace FucNames
