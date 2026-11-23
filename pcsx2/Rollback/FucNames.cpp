// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Rollback/FucNames.h"

#include "Memory.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <cctype>

namespace FucNames
{
	// Calls of the text drawing function in the label loop of FUN_00333920 (two colour variants).
	static constexpr std::array<u32, 2> CALL_SITES = {0x333ff0, 0x334020};
	static constexpr u32 JAL_DRAW_TEXT = 0x0C0851AC; // jal 0x2146b0
	static constexpr u32 STUB = 0x000FF000;
	static constexpr u32 NAMES = 0x000FF100;   // 2 x 16 bytes
	static constexpr u32 NAME_STRIDE = 16;
	static constexpr u32 JAL_STUB = 0x0C000000 | (STUB >> 2);

	// The stub (R5900). v0, v1 and t4-t6 are free here: they are not arguments of the call.
	static constexpr std::array<u32, 22> STUB_CODE = {
		0x3C020052, //  0 lui   v0, 0x52
		0x8C42D894, //  1 lw    v0, -0x276c(v0)       game mode (0x51d894)
		0x2442FFFD, //  2 addiu v0, v0, -3
		0x2C420002, //  3 sltiu v0, v0, 2             versus modes 3 and 4
		0x1040000F, //  4 beq   v0, zero, done
		0x00000000, //  5 nop
		0x3C030010, //  6 lui   v1, 0x10
		0x2463F100, //  7 addiu v1, v1, -0xf00        names (0xff100)
		0x00141100, //  8 sll   v0, s4, 4             player * 16
		0x00621821, //  9 addu  v1, v1, v0
		0x90620000, // 10 lbu   v0, 0(v1)
		0x10400008, // 11 beq   v0, zero, done        no name: keep the game's text
		0x00000000, // 12 nop
		0x01206021, // 13 addu  t4, t1, zero          text buffer
		0x00606821, // 14 addu  t5, v1, zero
		0x91AE0000, // 15 loop: lbu t6, 0(t5)
		0xA18E0000, // 16 sb    t6, 0(t4)
		0x25AD0001, // 17 addiu t5, t5, 1
		0x15C0FFFC, // 18 bne   t6, zero, loop
		0x258C0001, // 19 addiu t4, t4, 1
		0x080851AC, // 20 done: j 0x2146b0           draw it
		0x00000000, // 21 nop
	};

	static Name s_local = {};
} // namespace FucNames

std::string FucNames::Sanitize(std::string_view text)
{
	std::string out;
	for (const char c : text)
	{
		const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == ' ' || u == '-' || u == '.' || u == '!' || u == '?')
			out += u;
		if (out.size() == MAX_LENGTH)
			break;
	}
	// No leading or trailing spaces.
	const size_t first = out.find_first_not_of(' ');
	if (first == std::string::npos)
		return {};
	return out.substr(first, out.find_last_not_of(' ') - first + 1);
}

FucNames::Name FucNames::ToName(std::string_view text)
{
	Name name = {};
	const std::string clean = Sanitize(text);
	std::copy(clean.begin(), clean.end(), name.begin());
	return name;
}

void FucNames::SetLocal(std::string_view name)
{
	s_local = ToName(name);
}

const FucNames::Name& FucNames::GetLocal()
{
	return s_local;
}

bool FucNames::Apply(const Name& p1, const Name& p2)
{
	for (const u32 site : CALL_SITES)
	{
		const u32 insn = memRead32(site);
		if (insn != JAL_DRAW_TEXT && insn != JAL_STUB)
		{
			Console.Error(fmt::format("FucNames: unexpected code at {:08x} ({:08x}), names not shown", site, insn));
			return false;
		}
	}

	for (size_t i = 0; i < STUB_CODE.size(); i++)
		memWrite32(STUB + static_cast<u32>(i) * 4, STUB_CODE[i]);
	const std::array<const Name*, 2> names = {&p1, &p2};
	for (u32 player = 0; player < 2; player++)
	{
		for (u32 i = 0; i < NAME_STRIDE; i++)
			memWrite8(NAMES + player * NAME_STRIDE + i, (i < MAX_LENGTH) ? static_cast<u8>((*names[player])[i]) : 0);
	}
	for (const u32 site : CALL_SITES)
		memWrite32(site, JAL_STUB);
	return true;
}
