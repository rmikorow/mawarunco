// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Rollback/FucNames.h"

#include "Memory.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace FucNames
{
	// Calls of the text drawing function in the label loop of FUN_00333920 (two colour variants).
	static constexpr std::array<u32, 2> CALL_SITES = {0x333ff0, 0x334020};
	static constexpr u32 JAL_DRAW_TEXT = 0x0C0851AC; // jal 0x2146b0
	static constexpr u32 STUB = 0x000FF000;
	static constexpr u32 NAMES = 0x000FF100; // 2 x 32 bytes; 0xFF200: the stub's temp text
	static constexpr u32 NAME_STRIDE = 32;
	static constexpr u32 MATRICES = 0x000FF180; // 2 x 64 bytes: glyph transform per player
	static constexpr u32 MATRIX_STRIDE = 64;
	static constexpr u32 JAL_STUB = 0x0C000000 | (STUB >> 2);
	// HUD geometry (PS2 screen units): the label is centred on these x positions; glyphs are about 12.5
	// wide; about 240 fit between the screen edge and the timer.
	static constexpr std::array<float, 2> LABEL_CENTER_X = {140.0f, 500.0f};
	static constexpr float GLYPH_WIDTH = 12.5f;
	static constexpr float LABEL_MAX_WIDTH = 240.0f;

	// The stub (R5900), assembled by a script (comments show the source). v0, v1 and t4-t7 are free here
	// (not arguments of the call); a0 (the call's transform argument, 0 = none) gets the player's matrix.
	// It builds "<name> / <game text without leading spaces>" in the text buffer.
	static constexpr std::array<u32, 54> STUB_CODE = {
		0x3C020052, //  0 lui v0, 0x52  game mode (0x51d894)
		0x8C42D894, //  1 lw v0, -0x276c, v0
		0x2442FFFD, //  2 addiu v0, v0, -0x3
		0x2C420002, //  3 sltiu v0, v0, 0x2  versus modes 3 and 4
		0x1040002F, //  4 beq v0, zero, done
		0x00000000, //  5 nop
		0x3C030010, //  6 lui v1, 0x10
		0x2463F100, //  7 addiu v1, v1, -0xf00  names (0xff100)
		0x00141140, //  8 sll v0, s4, 0x5  player * 32
		0x00621821, //  9 addu v1, v1, v0
		0x90620000, // 10 lbu v0, 0x0, v1
		0x10400028, // 11 beq v0, zero, done  no name: keep the game's text
		0x00000000, // 12 nop
		0x3C0C0010, // 13 lui t4, 0x10
		0x258CF200, // 14 addiu t4, t4, -0xe00  temp (0xff200)
		0x01206821, // 15 addu t5, t1, zero
		0x91AE0000, // 16 skip: lbu t6, 0x0, t5
		0x25CFFFE0, // 17 addiu t7, t6, -0x20
		0x11E0FFFD, // 18 beq t7, zero, skip  leading space
		0x25AD0001, // 19 addiu t5, t5, 0x1
		0x25ADFFFF, // 20 addiu t5, t5, -0x1
		0x91AE0000, // 21 copy1: lbu t6, 0x0, t5
		0xA18E0000, // 22 sb t6, 0x0, t4
		0x25AD0001, // 23 addiu t5, t5, 0x1
		0x15C0FFFC, // 24 bne t6, zero, copy1
		0x258C0001, // 25 addiu t4, t4, 0x1
		0x01206021, // 26 addu t4, t1, zero
		0x906E0000, // 27 copy2: lbu t6, 0x0, v1
		0x11C00005, // 28 beq t6, zero, sep
		0x00000000, // 29 nop
		0xA18E0000, // 30 sb t6, 0x0, t4
		0x24630001, // 31 addiu v1, v1, 0x1
		0x1000FFFA, // 32 beq zero, zero, copy2
		0x258C0001, // 33 addiu t4, t4, 0x1
		0x240E0020, // 34 sep: addiu t6, zero, 0x20
		0xA18E0000, // 35 sb t6, 0x0, t4
		0x240E002F, // 36 addiu t6, zero, 0x2f  '/' (the font has no '|')
		0xA18E0001, // 37 sb t6, 0x1, t4
		0x240E0020, // 38 addiu t6, zero, 0x20
		0xA18E0002, // 39 sb t6, 0x2, t4
		0x258C0003, // 40 addiu t4, t4, 0x3
		0x3C0D0010, // 41 lui t5, 0x10
		0x25ADF200, // 42 addiu t5, t5, -0xe00
		0x91AE0000, // 43 copy3: lbu t6, 0x0, t5
		0xA18E0000, // 44 sb t6, 0x0, t4
		0x25AD0001, // 45 addiu t5, t5, 0x1
		0x15C0FFFC, // 46 bne t6, zero, copy3
		0x258C0001, // 47 addiu t4, t4, 0x1
		0x3C040010, // 48 lui a0, 0x10
		0x2484F180, // 49 addiu a0, a0, -0xe80  matrices (0xff180, 64 bytes per player)
		0x00141180, // 50 sll v0, s4, 0x6
		0x00822021, // 51 addu a0, a0, v0
		0x080851AC, // 52 done: j 0x2146b0  draw it
		0x00000000, // 53 nop
	};

	// Characters a name may have; index 0 ends the name. 6 bits per character: 20 characters in 15 bytes.
	static constexpr std::string_view ALPHABET = std::string_view("\0 ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.!?-", 42);
	static_assert(ALPHABET.size() <= 64);

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

FucNames::Packed FucNames::Pack(const Name& name)
{
	Packed out = {};
	for (size_t i = 0; i < MAX_LENGTH; i++)
	{
		const size_t pos = ALPHABET.find(name[i]);
		const u32 code = (name[i] == 0 || pos == std::string_view::npos) ? 0 : static_cast<u32>(pos);
		const size_t bit = i * 6;
		out[bit / 8] |= static_cast<u8>(code << (bit % 8));
		if (bit % 8 > 2)
			out[bit / 8 + 1] |= static_cast<u8>(code >> (8 - bit % 8));
		if (code == 0)
			break;
	}
	return out;
}

FucNames::Name FucNames::Unpack(const Packed& packed)
{
	std::string text;
	for (size_t i = 0; i < MAX_LENGTH; i++)
	{
		const size_t bit = i * 6;
		u32 code = packed[bit / 8] >> (bit % 8);
		if (bit % 8 > 2)
			code |= static_cast<u32>(packed[bit / 8 + 1]) << (8 - bit % 8);
		code &= 0x3F;
		if (code == 0 || code >= ALPHABET.size())
			break;
		text += ALPHABET[code];
	}
	return ToName(text);
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
	// Glyph transform: long texts are drawn narrower, scaled around the label centre, so they stay between
	// the screen edge and the timer. Row-vector layout as the PS2 SDK uses it (translation in the last row).
	for (u32 player = 0; player < 2; player++)
	{
		const size_t length = strnlen(names[player]->data(), MAX_LENGTH);
		const float text_width = static_cast<float>(length + 3 + 5) * GLYPH_WIDTH; // "<name> / 99WIN"
		const float sx = std::min(1.0f, LABEL_MAX_WIDTH / text_width);
		const std::array<float, 16> m = {sx, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
			LABEL_CENTER_X[player] * (1.0f - sx), 0.0f, 0.0f, 1.0f};
		for (u32 i = 0; i < m.size(); i++)
		{
			u32 bits;
			std::memcpy(&bits, &m[i], sizeof(bits));
			memWrite32(MATRICES + player * MATRIX_STRIDE + i * 4, bits);
		}
	}
	for (const u32 site : CALL_SITES)
		memWrite32(site, JAL_STUB);
	return true;
}
