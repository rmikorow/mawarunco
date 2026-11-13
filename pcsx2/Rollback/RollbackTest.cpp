// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Rollback/RollbackTest.h"

#include "Config.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "MemoryTypes.h"
#include "R3000A.h"
#include "R5900.h"
#include "Recording/PadData.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "SIO/Sio.h"
#include "SaveState.h"
#include "SPU2/spu2.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace RollbackTest
{
	enum class Phase
	{
		Idle,
		Recording,
		Replaying,
	};

	static constexpr u32 NUM_PORTS = 2;
	static constexpr const char* OSD_KEY = "RollbackTest";

	static Phase s_phase = Phase::Idle;
	static u32 s_requested_frames = 0;
	static u32 s_num_frames = 0;
	static u32 s_frame = 0;
	static std::unique_ptr<ArchiveEntryList> s_start_state;
	static std::unique_ptr<ArchiveEntryList> s_end_state;
	// Emulation state that is not part of the savestate format but must be carried by rollback
	// snapshots (see g_eeCycleFrac and SPU2::SaveVoiceBuffers).
	struct Extras
	{
		u32 ee_cycle_frac = 0;
		std::vector<u8> spu2_voice_buffers;

		bool operator==(const Extras& o) const
		{
			return ee_cycle_frac == o.ee_cycle_frac && spu2_voice_buffers == o.spu2_voice_buffers;
		}
	};
	static Extras s_start_extras;
	static Extras s_end_extras;

	static Extras CaptureExtras()
	{
		Extras e;
		e.ee_cycle_frac = g_eeCycleFrac;
		SPU2::SaveVoiceBuffers(e.spu2_voice_buffers);
		return e;
	}

	static void RestoreExtras(const Extras& e)
	{
		g_eeCycleFrac = e.ee_cycle_frac;
		SPU2::LoadVoiceBuffers(e.spu2_voice_buffers);
	}
	static std::vector<PadData> s_inputs; // (s_num_frames + 1) * NUM_PORTS entries

	static double s_save_ms = 0.0;
	static double s_load_ms = 0.0;

	static u32 s_test_index = 0;
	static bool s_exit_pending = false;
	static u32 s_input_changed_frames = 0; // frames where host input changed the pad after the poll
	static Variant s_variant = VARIANT_NONE; // diagnostic, see RollbackTest.h

	// Component mask bits for SaveState_LoadFromMemory (SavestateEntries order, bit 0 = internals).
	static constexpr u32 M_INTERNALS = 1u << 0;
	static constexpr u32 M_MEMORY = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) |
	                                (1u << 8) | (1u << 9); // EE/IOP RAM, HW regs, scratchpad, VU mem
	static constexpr u32 M_SPU2 = 1u << 10;
	static constexpr u32 M_USB = 1u << 11;
	static constexpr u32 M_PAD = 1u << 12;
	static constexpr u32 M_GS = 1u << 13;
	static constexpr u32 M_ALL = 0xFFFFFFFFu;

	static constexpr Variant s_battery_variants[] = {
		{"full restore (control)", true, true, M_ALL},
		{"caches + internals", true, true, M_INTERNALS},
		{"caches + SPU2", true, true, M_SPU2},
		{"caches + GS", true, true, M_GS},
		{"caches + PAD/USB", true, true, M_PAD | M_USB},
		{"caches + memory", true, true, M_MEMORY},
		{"caches + all but SPU2", true, true, M_ALL & ~M_SPU2},
		{"all, keep caches", true, false, M_ALL},
	};
	static u32 s_battery_next = 0;
	static u32 s_battery_frames = 0;
	static bool s_battery_active = false;

	// Per-frame fingerprint, taken right after the input poll. Used to find the first diverging frame.
	struct FrameTrace
	{
		u64 ee_cycle;
		u64 iop_cycle;
		u64 ee_hash;
		u64 iop_hash;

		bool operator==(const FrameTrace& o) const
		{
			return ee_cycle == o.ee_cycle && iop_cycle == o.iop_cycle && ee_hash == o.ee_hash && iop_hash == o.iop_hash;
		}
	};
	static std::vector<FrameTrace> s_trace;
	static u32 s_trace_mismatches = 0;

	static u64 HashMemory(const u8* data, size_t size);
	static FrameTrace TakeTrace();
	static void CheckTrace();

	static void Finish();

	static u64 PadSignature();
	static void ApplyFrameInput(u32 frame);
	static void DumpEntry(const char* tag, const std::string& name, const u8* data, u32 size);
	static std::unique_ptr<ArchiveEntryList> TakeSnapshot(double* ms);
	static bool CompareSnapshots(const ArchiveEntryList& a, const ArchiveEntryList& b);
} // namespace RollbackTest

void RollbackTest::Request(u32 frames, const Variant& variant)
{
	if (s_phase != Phase::Idle)
	{
		Host::AddIconOSDMessage(OSD_KEY, ICON_FA_TRIANGLE_EXCLAMATION, "Rollback test already running.",
			Host::OSD_QUICK_DURATION);
		return;
	}

	s_requested_frames = frames;
	s_variant = variant;
}

void RollbackTest::Abort()
{
	if (s_phase == Phase::Idle && s_requested_frames == 0)
		return;

	if (s_phase != Phase::Idle)
		Console.Warning("Rollback test: aborted.");

	s_battery_active = false;

	Finish();
}

void RollbackTest::Finish()
{
	s_phase = Phase::Idle;
	s_requested_frames = 0;
	s_exit_pending = false;
	s_start_state.reset();
	s_end_state.reset();
	s_inputs.clear();
	s_inputs.shrink_to_fit();
	s_trace.clear();
	s_trace.shrink_to_fit();
}

u64 RollbackTest::HashMemory(const u8* data, size_t size)
{
	// FNV-1a style over 64-bit words. Fast enough for a debug check, not cryptographic.
	u64 h = 0xcbf29ce484222325ull;
	const size_t words = size / sizeof(u64);
	for (size_t i = 0; i < words; i++)
	{
		u64 w;
		std::memcpy(&w, data + i * sizeof(u64), sizeof(w));
		h = (h ^ w) * 0x100000001b3ull;
	}
	return h;
}

RollbackTest::FrameTrace RollbackTest::TakeTrace()
{
	return FrameTrace{
		cpuRegs.cycle * 8 + g_eeCycleFrac,
		psxRegs.cycle,
		HashMemory(eeMem->Main, Ps2MemSize::ExposedRam),
		HashMemory(iopMem->Main, Ps2MemSize::ExposedIopRam),
	};
}

// Replay phase: compares the current frame with the recorded one and logs the first divergences.
void RollbackTest::CheckTrace()
{
	const FrameTrace now = TakeTrace();
	const FrameTrace& rec = s_trace[s_frame];
	if (now == rec)
		return;

	if (s_trace_mismatches++ < 8)
	{
		Console.Error(fmt::format("  frame {}: ee_cycle {} vs {} ({:+}), iop_cycle {} vs {} ({:+}), ee_ram {}, iop_ram {}",
			s_frame, rec.ee_cycle, now.ee_cycle, static_cast<s64>(now.ee_cycle - rec.ee_cycle),
			rec.iop_cycle, now.iop_cycle, static_cast<s64>(now.iop_cycle - rec.iop_cycle),
			rec.ee_hash == now.ee_hash ? "same" : "DIFF", rec.iop_hash == now.iop_hash ? "same" : "DIFF"));
	}
}

std::unique_ptr<ArchiveEntryList> RollbackTest::TakeSnapshot(double* ms)
{
	Common::Timer timer;
	Error error;
	std::unique_ptr<ArchiveEntryList> state = SaveState_DownloadState(&error);
	*ms = timer.GetTimeMilliseconds();
	if (!state)
		Console.Error(fmt::format("Rollback test: snapshot failed: {}", error.GetDescription()));
	return state;
}

void RollbackTest::OnInputPolled()
{
	if (s_phase == Phase::Idle)
	{
		if (s_requested_frames == 0)
			return;

		if (EmuConfig.Speedhacks.vuThread)
		{
			// MTVU runs VU1 on a host thread, so its timing is not deterministic.
			Host::AddIconOSDMessage("RollbackTestMTVU", ICON_FA_TRIANGLE_EXCLAMATION,
				"Rollback test: MTVU is enabled, results are not meaningful.", Host::OSD_WARNING_DURATION);
		}

		s_num_frames = s_requested_frames;
		s_requested_frames = 0;
		s_frame = 0;
		s_inputs.clear();
		s_inputs.reserve((s_num_frames + 1) * NUM_PORTS);
		s_trace.clear();
		s_trace.reserve(s_num_frames + 1);
		s_trace_mismatches = 0;
		s_input_changed_frames = 0;
		s_phase = Phase::Recording;
	}

	// Only the input is handled here. Snapshots and restores must not happen in the middle of the
	// vsync event: the counters are half-updated and callers hold state in locals. They are done in
	// OnExecutionExited(), after the event test has completed.
	if (s_phase == Phase::Recording)
	{
		// Input for the frame that starts now. Frame s_num_frames is recorded too, because the
		// pad state at the end point is part of the end snapshot.
		for (u32 port = 0; port < NUM_PORTS; port++)
			s_inputs.emplace_back(static_cast<int>(port), 0);
	}
	else
	{
		ApplyFrameInput(s_frame);
	}

	// Leave the CPU loop once the current event test has finished. This is requested on every
	// frame of both passes, so recording and replay take exactly the same path.
	s_exit_pending = true;
	Cpu->ExitExecution();
}

// Compact fingerprint of what the game can read from both pads (buttons, sticks, pressures).
u64 RollbackTest::PadSignature()
{
	u64 h = 0xcbf29ce484222325ull;
	auto mix = [&h](u64 v) { h = (h ^ v) * 0x100000001b3ull; };
	for (u32 port = 0; port < NUM_PORTS; port++)
	{
		const PadBase* pad = Pad::GetPad(sioConvertPortAndSlotToPad(static_cast<int>(port), 0));
		if (!pad)
			continue;
		mix(pad->GetButtons());
		const auto [lx, ly] = pad->GetRawLeftAnalog();
		const auto [rx, ry] = pad->GetRawRightAnalog();
		mix((static_cast<u64>(lx) << 24) | (static_cast<u64>(ly) << 16) | (static_cast<u64>(rx) << 8) | ry);
		for (u32 i = 0; i < 16; i++)
			mix(pad->GetRawInput(i));
	}
	return h;
}

// Sets the recorded pad state of the given frame.
void RollbackTest::ApplyFrameInput(u32 frame)
{
	for (u32 port = 0; port < NUM_PORTS; port++)
		s_inputs[frame * NUM_PORTS + port].OverrideActualController();
}

void RollbackTest::OnBeforeExecute()
{
	// OnExecutionExited() has already advanced s_frame, so the frame being emulated is s_frame - 1.
	if (s_phase == Phase::Idle || s_frame == 0 || s_inputs.size() < s_frame * NUM_PORTS)
		return;

	// Host input events are processed between Cpu->Execute() calls (Qt event loop). They must not
	// change the pad after the input poll, or the game reads something other than what was recorded.
	const u64 before = PadSignature();
	ApplyFrameInput(s_frame - 1);
	if (s_phase == Phase::Recording && PadSignature() != before)
		s_input_changed_frames++;
}

void RollbackTest::OnExecutionExited()
{
	if (!std::exchange(s_exit_pending, false) || s_phase == Phase::Idle)
		return;

	if (s_phase == Phase::Recording)
	{
		if (s_frame == 0)
		{
			s_start_state = TakeSnapshot(&s_save_ms);
			s_start_extras = CaptureExtras();
			if (!s_start_state)
			{
				Host::AddIconOSDMessage(OSD_KEY, ICON_FA_TRIANGLE_EXCLAMATION, "Rollback test: snapshot failed.",
					Host::OSD_ERROR_DURATION);
				Finish();
				return;
			}

			Console.WriteLn(fmt::format("Rollback test: recording {} frames (snapshot {:.2f} ms, {} bytes){}",
				s_num_frames, s_save_ms, s_start_state->GetBuffer().size(),
				s_variant.restore ? fmt::format(" [record pass: {}]", s_variant.name) : std::string()));

			// Diagnostic: make the recording pass go through (part of) the same restore as the replay
			// pass. If that makes the test pass, restoring has a side effect on emulation.
			if (s_variant.restore)
			{
				Error error;
				if (!SaveState_LoadFromMemory(*s_start_state, &error, s_variant.clear_caches, s_variant.component_mask))
				{
					Console.Error(fmt::format("Rollback test: restore failed: {}", error.GetDescription()));
					Finish();
					return;
				}
				RestoreExtras(s_start_extras);
			}
			Host::AddIconOSDMessage(OSD_KEY, ICON_FA_CIRCLE_DOT,
				fmt::format("Rollback test: recording {} frames...", s_num_frames), Host::OSD_INFO_DURATION);
		}

		s_trace.push_back(TakeTrace());

		if (s_frame < s_num_frames)
		{
			s_frame++;
			return;
		}

		double end_ms;
		s_end_state = TakeSnapshot(&end_ms);
		s_end_extras = CaptureExtras();
		if (!s_end_state)
		{
			Finish();
			return;
		}

		Common::Timer timer;
		Error error;
		if (!SaveState_LoadFromMemory(*s_start_state, &error))
		{
			Console.Error(fmt::format("Rollback test: restore failed: {}", error.GetDescription()));
			Host::AddIconOSDMessage(OSD_KEY, ICON_FA_TRIANGLE_EXCLAMATION,
				fmt::format("Rollback test: restore failed: {}", error.GetDescription()), Host::OSD_ERROR_DURATION);
			Finish();
			return;
		}
		RestoreExtras(s_start_extras);
		s_load_ms = timer.GetTimeMilliseconds();
		Console.WriteLn(fmt::format("Rollback test: restored S0 in {:.2f} ms, replaying", s_load_ms));

		// Frame 0 of the replay is the restored S0 itself. The input for frame 0 is part of S0.
		s_phase = Phase::Replaying;
		s_frame = 0;
		CheckTrace();
		s_frame = 1;
		return;
	}

	// Replaying
	CheckTrace();
	if (s_frame < s_num_frames)
	{
		s_frame++;
		return;
	}

	double replay_ms;
	std::unique_ptr<ArchiveEntryList> replay_state = TakeSnapshot(&replay_ms);
	if (replay_state)
	{
		Console.WriteLn(fmt::format("Rollback test: {} of {} frames diverged during replay, host input changed the pad after the poll in {} frames",
			s_trace_mismatches, s_num_frames + 1, s_input_changed_frames));
		bool same = CompareSnapshots(*s_end_state, *replay_state);
		const Extras replay_extras = CaptureExtras();
		if (replay_extras.ee_cycle_frac != s_end_extras.ee_cycle_frac)
		{
			Console.Error(fmt::format("  g_eeCycleFrac: {} vs {}", s_end_extras.ee_cycle_frac, replay_extras.ee_cycle_frac));
			same = false;
		}
		if (replay_extras.spu2_voice_buffers != s_end_extras.spu2_voice_buffers)
		{
			Console.Error("  SPU2 voice buffers differ");
			same = false;
		}
		const std::string msg = fmt::format("Rollback test{} over {} frames: {} (save {:.1f} ms, load {:.1f} ms)",
			s_variant.restore ? fmt::format(" [record pass: {}]", s_variant.name) : std::string(), s_num_frames,
			same ? "IDENTICAL" : "MISMATCH", s_save_ms, s_load_ms);
		Console.WriteLn(same ? Color_Green : Color_Red, msg);
		Host::AddIconOSDMessage(OSD_KEY, same ? ICON_FA_CHECK : ICON_FA_TRIANGLE_EXCLAMATION, msg,
			Host::OSD_ERROR_DURATION);
		if (s_battery_active)
			Console.WriteLn(fmt::format("BATTERY RESULT: {:<28} {}", s_variant.name, same ? "IDENTICAL" : "MISMATCH"));
	}

	Finish();

	if (s_battery_active)
	{
		if (s_battery_next < std::size(s_battery_variants))
		{
			Request(s_battery_frames, s_battery_variants[s_battery_next++]);
		}
		else
		{
			s_battery_active = false;
			Host::AddIconOSDMessage(OSD_KEY, ICON_FA_CHECK, "Rollback test battery finished.", Host::OSD_ERROR_DURATION);
		}
	}
}

void RollbackTest::RequestBattery(u32 frames)
{
	if (s_phase != Phase::Idle || s_battery_active)
		return;

	s_battery_active = true;
	s_battery_next = 0;
	s_battery_frames = frames;
	Console.WriteLn(fmt::format("Rollback test battery: {} variants", std::size(s_battery_variants)));
	Request(frames, s_battery_variants[s_battery_next++]);
}

// Writes a differing component to <logs>/rollback/test<N>_<tag>_<name> for offline diffing.
void RollbackTest::DumpEntry(const char* tag, const std::string& name, const u8* data, u32 size)
{
	const std::string dir = Path::Combine(EmuFolders::Logs, "rollback");
	if (!FileSystem::EnsureDirectoryExists(dir.c_str(), true))
		return;

	const std::string path = Path::Combine(dir, fmt::format("test{}_{}_{}", s_test_index, tag, name));
	if (!FileSystem::WriteBinaryFile(path.c_str(), data, size))
		Console.Error(fmt::format("Rollback test: failed to write {}", path));
}

bool RollbackTest::CompareSnapshots(const ArchiveEntryList& a, const ArchiveEntryList& b)
{
	s_test_index++;

	if (a.GetLength() != b.GetLength())
	{
		Console.Error("Rollback test: snapshots have a different number of entries.");
		return false;
	}

	bool all_same = true;
	for (u32 i = 0; i < a.GetLength(); i++)
	{
		const ArchiveEntry& ea = a[i];
		const ArchiveEntry& eb = b[i];
		const u8* pa = a.GetBuffer().data() + ea.GetDataIndex();
		const u8* pb = b.GetBuffer().data() + eb.GetDataIndex();

		if (ea.GetDataSize() != eb.GetDataSize())
		{
			Console.Error(fmt::format("  {}: size differs ({} vs {})", ea.GetFilename(), ea.GetDataSize(), eb.GetDataSize()));
			DumpEntry("record", ea.GetFilename(), pa, ea.GetDataSize());
			DumpEntry("replay", eb.GetFilename(), pb, eb.GetDataSize());
			all_same = false;
			continue;
		}

		const u32 size = ea.GetDataSize();
		if (std::memcmp(pa, pb, size) == 0)
		{
			Console.WriteLn(fmt::format("  {}: identical ({} bytes)", ea.GetFilename(), size));
			continue;
		}

		all_same = false;
		u32 diff_bytes = 0;
		u32 first = 0xFFFFFFFFu;
		u32 last = 0;
		for (u32 j = 0; j < size; j++)
		{
			if (pa[j] != pb[j])
			{
				diff_bytes++;
				first = std::min(first, j);
				last = j;
			}
		}
		Console.Error(fmt::format("  {}: {} of {} bytes differ, first at 0x{:x}, last at 0x{:x}",
			ea.GetFilename(), diff_bytes, size, first, last));
		DumpEntry("record", ea.GetFilename(), pa, size);
		DumpEntry("replay", eb.GetFilename(), pb, size);

		// Detail for EE RAM: list the first differing 16-byte lines (offset = PS2 physical address).
		if (ea.GetFilename() == "eeMemory.bin")
		{
			u32 shown = 0;
			for (u32 line = first & ~15u; line <= last && shown < 32; line += 16)
			{
				const u32 n = std::min<u32>(16, size - line);
				if (std::memcmp(pa + line, pb + line, n) != 0)
				{
					Console.Error(fmt::format("    {:08x}", line));
					shown++;
				}
			}
		}
	}

	return all_same;
}
