// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Rollback/RollbackTest.h"

#include "Host.h"
#include "IconsFontAwesome.h"
#include "Recording/PadData.h"
#include "SaveState.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <cstring>
#include <memory>
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
	static std::vector<PadData> s_inputs; // (s_num_frames + 1) * NUM_PORTS entries

	static double s_save_ms = 0.0;
	static double s_load_ms = 0.0;

	static void Finish();
	static std::unique_ptr<ArchiveEntryList> TakeSnapshot(double* ms);
	static bool CompareSnapshots(const ArchiveEntryList& a, const ArchiveEntryList& b);
} // namespace RollbackTest

void RollbackTest::Request(u32 frames)
{
	if (s_phase != Phase::Idle)
	{
		Host::AddIconOSDMessage(OSD_KEY, ICON_FA_TRIANGLE_EXCLAMATION, "Rollback test already running.",
			Host::OSD_QUICK_DURATION);
		return;
	}

	s_requested_frames = frames;
}

void RollbackTest::Abort()
{
	if (s_phase == Phase::Idle && s_requested_frames == 0)
		return;

	if (s_phase != Phase::Idle)
		Console.Warning("Rollback test: aborted.");

	Finish();
}

void RollbackTest::Finish()
{
	s_phase = Phase::Idle;
	s_requested_frames = 0;
	s_start_state.reset();
	s_end_state.reset();
	s_inputs.clear();
	s_inputs.shrink_to_fit();
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
	switch (s_phase)
	{
		case Phase::Idle:
		{
			if (s_requested_frames == 0)
				return;

			s_num_frames = s_requested_frames;
			s_requested_frames = 0;
			s_frame = 0;
			s_inputs.clear();
			s_inputs.reserve((s_num_frames + 1) * NUM_PORTS);

			s_start_state = TakeSnapshot(&s_save_ms);
			if (!s_start_state)
			{
				Host::AddIconOSDMessage(OSD_KEY, ICON_FA_TRIANGLE_EXCLAMATION, "Rollback test: snapshot failed.",
					Host::OSD_ERROR_DURATION);
				Finish();
				return;
			}

			Console.WriteLn(fmt::format("Rollback test: recording {} frames (snapshot {:.2f} ms, {} bytes)",
				s_num_frames, s_save_ms, s_start_state->GetBuffer().size()));
			Host::AddIconOSDMessage(OSD_KEY, ICON_FA_CIRCLE_DOT,
				fmt::format("Rollback test: recording {} frames...", s_num_frames), Host::OSD_INFO_DURATION);
			s_phase = Phase::Recording;
			[[fallthrough]];
		}

		case Phase::Recording:
		{
			// Input for the frame that starts now. Frame s_num_frames is recorded too, because the
			// pad state at the end point is part of the end snapshot.
			for (u32 port = 0; port < NUM_PORTS; port++)
				s_inputs.emplace_back(static_cast<int>(port), 0);

			if (s_frame < s_num_frames)
			{
				s_frame++;
				return;
			}

			double end_ms;
			s_end_state = TakeSnapshot(&end_ms);
			if (!s_end_state)
			{
				Finish();
				return;
			}

			// Restore S0. This happens at the same point of the vsync where S0 was taken, so the
			// rest of this vsync runs exactly as it did after the original snapshot.
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
			s_load_ms = timer.GetTimeMilliseconds();

			Console.WriteLn(fmt::format("Rollback test: restored S0 in {:.2f} ms, replaying", s_load_ms));
			s_phase = Phase::Replaying;
			s_frame = 0;
			[[fallthrough]];
		}

		case Phase::Replaying:
		{
			for (u32 port = 0; port < NUM_PORTS; port++)
				s_inputs[s_frame * NUM_PORTS + port].OverrideActualController();

			if (s_frame < s_num_frames)
			{
				s_frame++;
				return;
			}

			double replay_ms;
			std::unique_ptr<ArchiveEntryList> replay_state = TakeSnapshot(&replay_ms);
			if (replay_state)
			{
				const bool same = CompareSnapshots(*s_end_state, *replay_state);
				const std::string msg = fmt::format("Rollback test over {} frames: {} (save {:.1f} ms, load {:.1f} ms)",
					s_num_frames, same ? "IDENTICAL" : "MISMATCH", s_save_ms, s_load_ms);
				Console.WriteLn(same ? Color_Green : Color_Red, msg);
				Host::AddIconOSDMessage(OSD_KEY, same ? ICON_FA_CHECK : ICON_FA_TRIANGLE_EXCLAMATION, msg,
					Host::OSD_ERROR_DURATION);
			}

			Finish();
			return;
		}
	}
}

bool RollbackTest::CompareSnapshots(const ArchiveEntryList& a, const ArchiveEntryList& b)
{
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
