// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Rollback/BootShortcut.h"
#include "Rollback/FucButtons.h"
#include "Rollback/Netplay.h"
#include "Rollback/Rollback.h"

#include "GS/GS.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "Memory.h"
#include "MTGS.h"
#include "VMManager.h"
#include "VUmicro.h"
#include "vtlb.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include "fmt/format.h"

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include "xxhash.h"

#include <optional>
#include <string>
#include <vector>

namespace BootShortcut
{
	enum class StepKind
	{
		Wait,
		Press,
		Poke,
		Shot,
		DumpRam,
	};

	struct Step
	{
		StepKind kind = StepKind::Wait;
		u32 frames = 0;      // Wait, Press
		u32 port = 0;        // Press
		u16 pressed = 0;     // Press: button bits (set = pressed)
		u32 addr = 0;        // Poke
		u32 value = 0;       // Poke
		u32 size = 0;        // Poke: 1, 2 or 4 bytes
	};

	struct Script
	{
		std::vector<Step> steps;
		u32 shot_interval = 0;
		bool slow = false;
	};

	// Fate/unlimited codes (SLPM-55108): power-on to a main menu entry, all characters unlocked.
	// Every wait has a wide margin: the game waits at each prompt, so pressing later is safe, and a
	// slightly different boot timing (e.g. another BIOS) must not make a press miss.
	// Main menu wheel, ARCADE selected first: ARCADE, VS-CPU, VS-2P, WATCH, TUTORIAL, PRACTICE, ???,
	// MISSION, EXTRA, OPTION.
	static std::string FucMenuScript(int downs)
	{
		// "No memory card. Start without saving?" (shown at about frame 540): cursor to "yes", confirm.
		std::string s = "w900 LEFT w20 CIRCLE w300";
		// Logos are not skippable; START skips the intro movie and the title screen. On the main menu START
		// does nothing, so keep pressing it until the menu is certainly reached (about 1300 frames).
		for (int i = 0; i < 45; i++)
			s += " START w40";
		// Unlock all characters: flags 0x200-0x212 of the game's progress bitfield, both in the active
		// copy (0x523850) and in the copy that would be saved (0x523a40); byte 0x43 keeps its other flags.
		s += " poke32:523890=ff07ffff poke32:523a80=ff07ffff";
		// Select the entry, then wait for its character select.
		s += " w120";
		for (int i = 0; i < downs; i++)
			s += " DOWN w20";
		s += " CIRCLE w500";
		return s;
	}

	/// Built-in script by name, empty if the name is not a built-in one.
	static std::string BuiltinScript(std::string_view name);

	static std::optional<Script> s_pending;
	static bool s_cards_off = false; // from the request until the VM shuts down
	static Script s_script;
	static bool s_running = false;
	static size_t s_step = 0;
	static u32 s_step_frame = 0; // frames spent in the current step
	static u32 s_frame = 0;      // frames since the shortcut started
	static Common::Timer s_timer;

	static bool EqualNoCase(std::string_view a, std::string_view b)
	{
		return a.size() == b.size() && StringUtil::Strncasecmp(a.data(), b.data(), a.size()) == 0;
	}

	static bool Parse(std::string_view text, Script* out, Error* error);
	static std::optional<u16> ParseButtons(std::string_view text);
	static void Begin();
	static void Finish();
	static void ApplyFlags(bool on);
	static void Screenshot();
	static std::string StateHash();
} // namespace BootShortcut

std::optional<u16> BootShortcut::ParseButtons(std::string_view text)
{
	// Bit = position in Pad::FrameInput::buttons (active low there).
	static constexpr struct
	{
		const char* name;
		u8 bit;
	} names[] = {{"L2", 0}, {"R2", 1}, {"L1", 2}, {"R1", 3}, {"TRIANGLE", 4}, {"CIRCLE", 5}, {"CROSS", 6},
		{"SQUARE", 7}, {"SELECT", 8}, {"L3", 9}, {"R3", 10}, {"START", 11}, {"UP", 12}, {"RIGHT", 13},
		{"DOWN", 14}, {"LEFT", 15}};

	u16 pressed = 0;
	for (const std::string_view name : StringUtil::SplitString(text, '+', true))
	{
		bool found = false;
		for (const auto& n : names)
		{
			if (EqualNoCase(name, n.name))
			{
				pressed |= static_cast<u16>(1u << n.bit);
				found = true;
				break;
			}
		}
		if (!found)
			return std::nullopt;
	}
	return pressed ? std::optional<u16>(pressed) : std::nullopt;
}

bool BootShortcut::Parse(std::string_view text, Script* out, Error* error)
{
	Script script;
	std::string normalized(text);
	for (char& c : normalized)
	{
		if (c == ';' || c == '\t' || c == '\n' || c == '\r')
			c = ' ';
	}

	for (const std::string_view token : StringUtil::SplitString(normalized, ' ', true))
	{
		auto fail = [&](std::string_view why) {
			Error::SetString(error, fmt::format("Boot shortcut: {} in '{}'", why, token));
			return false;
		};

		Step step;
		if (token == "dumpram")
		{
			step.kind = StepKind::DumpRam;
		}
		else if (token == "shot")
		{
			step.kind = StepKind::Shot;
		}
		else if (token == "slow")
		{
			script.slow = true;
			continue;
		}
		else if (token.starts_with("shots="))
		{
			const std::optional<u32> n = StringUtil::FromChars<u32>(token.substr(6));
			if (!n.has_value() || *n == 0)
				return fail("bad interval");
			script.shot_interval = *n;
			continue;
		}
		else if (token.starts_with("poke"))
		{
			const size_t colon = token.find(':');
			const size_t eq = token.find('=');
			if (colon == std::string_view::npos || eq == std::string_view::npos || eq < colon)
				return fail("expected pokeN:addr=value");
			const std::string_view width = token.substr(4, colon - 4);
			step.size = (width == "8") ? 1 : (width == "16") ? 2 : (width == "32") ? 4 : 0;
			const std::optional<u32> addr = StringUtil::FromChars<u32>(token.substr(colon + 1, eq - colon - 1), 16);
			const std::optional<u32> value = StringUtil::FromChars<u32>(token.substr(eq + 1), 16);
			if (step.size == 0 || !addr.has_value() || !value.has_value() || *addr >= Ps2MemSize::MainRam)
				return fail("bad poke");
			step.kind = StepKind::Poke;
			step.addr = *addr;
			step.value = *value;
		}
		else if (token.size() > 1 && token[0] == 'w' && token[1] >= '0' && token[1] <= '9')
		{
			const std::optional<u32> n = StringUtil::FromChars<u32>(token.substr(1));
			if (!n.has_value())
				return fail("bad wait");
			step.kind = StepKind::Wait;
			step.frames = *n;
		}
		else
		{
			std::string_view rest = token;
			if (rest.starts_with("2."))
			{
				step.port = 1;
				rest = rest.substr(2);
			}
			else if (rest.starts_with("1."))
			{
				rest = rest.substr(2);
			}
			step.frames = 3;
			if (const size_t colon = rest.find(':'); colon != std::string_view::npos)
			{
				const std::optional<u32> n = StringUtil::FromChars<u32>(rest.substr(colon + 1));
				if (!n.has_value() || *n == 0)
					return fail("bad press length");
				step.frames = *n;
				rest = rest.substr(0, colon);
			}
			const std::optional<u16> buttons = ParseButtons(rest);
			if (!buttons.has_value())
				return fail("unknown token or button");
			step.kind = StepKind::Press;
			step.pressed = *buttons;
		}
		script.steps.push_back(step);
	}

	*out = std::move(script);
	return true;
}

std::string BootShortcut::BuiltinScript(std::string_view name)
{
	if (EqualNoCase(name, "vs2p"))
		return FucMenuScript(2); // "Vs. 2P" character select
	if (EqualNoCase(name, "practice"))
		return FucMenuScript(5); // "Practice" character select
	return {};
}

bool BootShortcut::Request(std::string_view script, Error* error)
{
	Script parsed;
	const std::string builtin = BuiltinScript(script);
	if (!Parse(builtin.empty() ? script : std::string_view(builtin), &parsed, error))
		return false;
	s_pending = std::move(parsed);
	// The script expects the game's "no memory card" prompt, so the cards stay off for the whole session
	// (also offline, where the card contents would differ between machines).
	s_cards_off = true;
	return true;
}

bool BootShortcut::MemoryCardsOff()
{
	return s_cards_off;
}

void BootShortcut::OnShutdown()
{
	Abort();
	s_cards_off = false;
}

bool BootShortcut::IsActive()
{
	return s_running || s_pending.has_value();
}

bool BootShortcut::IsRunning()
{
	return IsActive();
}

void BootShortcut::ApplyFlags(bool on)
{
	const bool fast = on && !s_script.slow;
	Rollback::g_skip_throttle = fast;
	Rollback::g_skip_audio = fast;
}

void BootShortcut::Begin()
{
	s_script = std::move(*s_pending);
	s_pending.reset();

	// Port 2 may be empty offline; netplay checks both ports itself when its session starts.
	Error error;
	if (!Rollback::BeginInputInjection(&error, false))
	{
		Console.Error(fmt::format("Boot shortcut: {}", error.GetDescription()));
		Host::AddIconOSDMessage("BootShortcut", ICON_FA_TRIANGLE_EXCLAMATION,
			fmt::format("Boot shortcut: {}", error.GetDescription()), Host::OSD_ERROR_DURATION);
		return;
	}

	s_running = true;
	s_step = 0;
	s_step_frame = 0;
	s_frame = 0;
	s_timer.Reset();
	ApplyFlags(true);
	Console.WriteLn(fmt::format("Boot shortcut: started ({} steps)", s_script.steps.size()));
	Host::AddIconOSDMessage("BootShortcut", ICON_FA_FORWARD, "Starting the game ...", Host::OSD_INFO_DURATION);
}

void BootShortcut::Finish()
{
	// Offline: the local player's in-game button layout for player 1 (player 2 keeps the default). In
	// netplay both peers write both layouts at the start of the session instead (Netplay handshake).
	if (!Netplay::IsActive())
	{
		FucButtons::Write(0, FucButtons::GetLocal());
		Console.WriteLn(fmt::format("Boot shortcut: P1 button layout {}", FucButtons::ToString(FucButtons::GetLocal())));
	}

	ApplyFlags(false);
	Rollback::EndInputInjection();
	s_running = false;
	VMManager::Internal::ResetFrameLimiter();
	Console.WriteLn(fmt::format("BOOT SHORTCUT done: frame {}, {:.2f} s, state {}", s_frame,
		s_timer.GetTimeSeconds(), StateHash()));
	Host::RemoveKeyedOSDMessage("BootShortcut");
}

void BootShortcut::Abort()
{
	s_pending.reset();
	if (!s_running)
		return;
	ApplyFlags(false);
	Rollback::EndInputInjection();
	s_running = false;
	Console.WriteLn(fmt::format("Boot shortcut: aborted at frame {}", s_frame));
}

void BootShortcut::Screenshot()
{
	const std::string dir = Path::Combine(EmuFolders::Logs, "bootshortcut");
	FileSystem::EnsureDirectoryExists(dir.c_str(), true);
	std::string path = Path::Combine(dir, fmt::format("f{:05}.png", s_frame));
	MTGS::RunOnGSThread([path = std::move(path)]() { GSQueueSnapshot(path, 0); });
}

std::string BootShortcut::StateHash()
{
	// Gameplay-relevant memory, as compared by netplay's desync detection.
	XXH3_state_t* state = XXH3_createState();
	XXH3_128bits_reset(state);
	XXH3_128bits_update(state, eeMem->Main, Ps2MemSize::MainRam);
	XXH3_128bits_update(state, eeMem->Scratch, sizeof(eeMem->Scratch));
	XXH3_128bits_update(state, iopMem->Main, Ps2MemSize::IopRam);
	XXH3_128bits_update(state, vuRegs[0].Mem, VU0_MEMSIZE);
	XXH3_128bits_update(state, vuRegs[1].Mem, VU1_MEMSIZE);
	const XXH128_hash_t h = XXH3_128bits_digest(state);
	XXH3_freeState(state);
	// EE RAM alone as well: it tells whether a difference is in the game's memory or elsewhere.
	const XXH64_hash_t ee = XXH3_64bits(eeMem->Main, Ps2MemSize::MainRam);
	return fmt::format("{:016x}{:016x} (EE RAM {:016x})", h.high64, h.low64, ee);
}

void BootShortcut::OnInputPolled()
{
	if (s_pending.has_value())
		Begin();
	if (!s_running)
		return;

	if (s_script.shot_interval && (s_frame % s_script.shot_interval) == 0)
		Screenshot();

	// Run the steps that take no frame (pokes, screenshots), then set this frame's input.
	Rollback::PadInput input[Rollback::NUM_PORTS] = {};
	for (;;)
	{
		if (s_step >= s_script.steps.size())
		{
			Finish();
			return;
		}

		const Step& step = s_script.steps[s_step];
		if (step.kind == StepKind::Poke)
		{
			if (step.size == 1)
				memWrite8(step.addr, static_cast<u8>(step.value));
			else if (step.size == 2)
				memWrite16(step.addr, static_cast<u16>(step.value));
			else
				memWrite32(step.addr, step.value);
			s_step++;
			continue;
		}
		if (step.kind == StepKind::DumpRam)
		{
			const std::string dir = Path::Combine(EmuFolders::Logs, "bootshortcut");
			FileSystem::EnsureDirectoryExists(dir.c_str(), true);
			FileSystem::WriteBinaryFile(Path::Combine(dir, fmt::format("ee_f{:05}.bin", s_frame)).c_str(), eeMem->Main,
				Ps2MemSize::MainRam);
			s_step++;
			continue;
		}
		if (step.kind == StepKind::Shot)
		{
			Screenshot();
			s_step++;
			continue;
		}
		if (s_step_frame >= step.frames)
		{
			s_step++;
			s_step_frame = 0;
			continue;
		}

		if (step.kind == StepKind::Press)
			input[step.port].buttons = static_cast<u16>(~step.pressed);
		s_step_frame++;
		break;
	}

	for (u32 port = 0; port < Rollback::NUM_PORTS; port++)
		Rollback::SetInput(port, input[port]);
	s_frame++;
}
