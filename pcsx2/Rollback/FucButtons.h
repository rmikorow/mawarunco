// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

// Fate/unlimited codes (SLPM-55108): the game's own button config (OPTION > CONTROLLER SETTINGS), which
// assigns a game action to each PS2 button, separately for player 1 and 2.
//
// It lives in the settings block in EE RAM, 10 words per player (P1 at 0x523918, P2 0x28 bytes later):
// one word per PS2 button slot (square, triangle, circle, cross, L1, L2, -, R1, R2, -), each an action bit
// mask (0x10 L, 0x20 M, 0x40 H, 0x80 reflect guard, sums for combined attacks, 0x400 << n for the rows from
// SP attack A on). With memory cards off it is not saved, so the launcher keeps each player's layout and
// writes it into the game: offline for player 1, in netplay host -> P1 and joiner -> P2 (exchanged at the
// start of the session, see Netplay).
namespace FucButtons
{
	/// PS2 buttons the game lets you assign, in this order.
	static constexpr u32 NUM_BUTTONS = 8;
	/// Actions, index 0 = none.
	static constexpr u32 NUM_ACTIONS = 21;

	/// Action index per assignable button.
	using Layout = std::array<u8, NUM_BUTTONS>;

	/// Button name for the UI (English), e.g. "Square".
	const char* ButtonName(u32 button);
	/// Action name for the UI (English, as in the game menu), e.g. "L Attack".
	const char* ActionName(u32 action);
	/// True for the actions that only work in practice mode.
	bool IsPracticeOnly(u32 action);

	/// Action name as the game's menu writes it, e.g. "L - ATTACK".
	const char* ActionMenuName(u32 action);

	/// Assigns a button to an action the way the game's menu does: the button gets the action, and the
	/// button that had the action before gets the button's previous action (swap).
	void Assign(Layout& layout, u32 button, u32 action);

	/// Button that has the action, or NUM_BUTTONS if none.
	u32 ButtonForAction(const Layout& layout, u32 action);

	/// The game's default layout.
	Layout Default();

	/// Text form for settings and the command line: two hex digits per button, e.g. "0102030405070600".
	std::string ToString(const Layout& layout);
	std::optional<Layout> FromString(std::string_view text);

	/// Layout of the local player (set by the launcher before a boot; default if never set).
	void SetLocal(const Layout& layout);
	const Layout& GetLocal();

	/// Writes a layout into the game's settings block for player 0 or 1. CPU thread, between frames.
	void Write(u32 player, const Layout& layout);
} // namespace FucButtons
