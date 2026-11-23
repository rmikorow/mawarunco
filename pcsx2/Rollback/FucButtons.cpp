// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Rollback/FucButtons.h"

#include "Memory.h"

#include "common/StringUtil.h"

#include "fmt/format.h"

namespace FucButtons
{
	static constexpr u32 SETTINGS_P1 = 0x523918;
	static constexpr u32 PLAYER_STRIDE = 0x28;
	static constexpr u32 SLOTS_PER_PLAYER = 10;

	// Settings slot of each assignable button (slots 6 and 9 are not used by the game).
	static constexpr std::array<u32, NUM_BUTTONS> BUTTON_SLOT = {0, 1, 2, 3, 4, 5, 7, 8};
	static constexpr std::array<const char*, NUM_BUTTONS> BUTTON_NAMES = {
		"Square", "Triangle", "Circle", "Cross", "L1", "L2", "R1", "R2"};

	struct Action
	{
		const char* name;
		u32 mask;
		bool practice_only;
	};
	// Rows of the game's menu, in its order (masks measured in the game, 2026-11-23).
	static constexpr std::array<Action, NUM_ACTIONS> ACTIONS = {{
		{"None", 0, false},
		{"L Attack", 0x10, false},
		{"M Attack", 0x20, false},
		{"H Attack", 0x40, false},
		{"Reflect Guard", 0x80, false},
		{"L+M Attack", 0x30, false},
		{"M+H Attack", 0x60, false},
		{"L+M+H Attack", 0x70, false},
		{"SP Attack A", 0x400, false},
		{"SP Attack B", 0x800, false},
		{"SP Attack C", 0x1000, false},
		{"EX Attack", 0x2000, false},
		{"Key Recording", 0x4000, true},
		{"Key Replay (Player)", 0x8000, true},
		{"Key Replay (Enemy)", 0x10000, true},
		{"Key Replay (Repeat/Player)", 0x20000, true},
		{"Key Replay (Repeat/Enemy)", 0x40000, true},
		{"Control Change", 0x80000, true},
		{"Enemy Forward", 0x100000, true},
		{"Enemy Jump", 0x200000, true},
		{"Peculiar Parameter Up", 0x400000, true},
	}};

	static Layout s_local = Default();
} // namespace FucButtons

const char* FucButtons::ButtonName(u32 button)
{
	return (button < NUM_BUTTONS) ? BUTTON_NAMES[button] : "";
}

const char* FucButtons::ActionName(u32 action)
{
	return (action < NUM_ACTIONS) ? ACTIONS[action].name : "";
}

bool FucButtons::IsPracticeOnly(u32 action)
{
	return action < NUM_ACTIONS && ACTIONS[action].practice_only;
}

FucButtons::Layout FucButtons::Default()
{
	// Square L, triangle M, circle H, cross reflect guard, L1 L+M, L2 L+M+H, R1 M+H, R2 none.
	return {1, 2, 3, 4, 5, 7, 6, 0};
}

std::string FucButtons::ToString(const Layout& layout)
{
	std::string s;
	for (const u8 a : layout)
		s += fmt::format("{:02x}", a);
	return s;
}

std::optional<FucButtons::Layout> FucButtons::FromString(std::string_view text)
{
	if (text.size() != NUM_BUTTONS * 2)
		return std::nullopt;
	Layout layout;
	for (u32 i = 0; i < NUM_BUTTONS; i++)
	{
		const std::optional<u32> v = StringUtil::FromChars<u32>(text.substr(i * 2, 2), 16);
		if (!v.has_value() || *v >= NUM_ACTIONS)
			return std::nullopt;
		layout[i] = static_cast<u8>(*v);
	}
	return layout;
}

void FucButtons::SetLocal(const Layout& layout)
{
	s_local = layout;
}

const FucButtons::Layout& FucButtons::GetLocal()
{
	return s_local;
}

void FucButtons::Write(u32 player, const Layout& layout)
{
	std::array<u32, SLOTS_PER_PLAYER> words = {};
	for (u32 i = 0; i < NUM_BUTTONS; i++)
		words[BUTTON_SLOT[i]] = ACTIONS[std::min<u32>(layout[i], NUM_ACTIONS - 1)].mask;
	const u32 base = SETTINGS_P1 + player * PLAYER_STRIDE;
	for (u32 slot = 0; slot < SLOTS_PER_PLAYER; slot++)
		memWrite32(base + slot * 4, words[slot]);
}
