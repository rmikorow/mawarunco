// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Rollback/RunAhead.h"
#include "Rollback/BootShortcut.h"
#include "Rollback/LatencyTest.h"
#include "Rollback/Netplay.h"
#include "Rollback/Rollback.h"
#include "Rollback/RollbackTest.h"

#include "Host.h"
#include "IconsFontAwesome.h"
#include "Input/InputManager.h"
#include "R5900.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <utility>

namespace RunAhead
{
	enum class State
	{
		Start, // waiting for the first frame boundary
		Real,  // the real frame is running
		Spec,  // speculative frames are running
	};

	static bool s_init = false;
	static u32 s_frames = 0;
	static State s_state = State::Start;
	static bool s_exit_pending = false;
	static u32 s_spec_index = 0;
	static Rollback::Snapshot s_snapshot;

	// Time budget: work per cycle (real frame + snapshot + speculative frames + restore).
	static Common::Timer s_cycle_timer;
	static u32 s_over_budget = 0;
	static u32 s_cycles = 0;
	static double s_work_sum_ms = 0.0;
	static double s_work_max_ms = 0.0;

	static void Init()
	{
		if (s_init)
			return;
		s_init = true;
		// On by default in this fork (one frame hides Fate's internal frame of lag, ~10 ms).
		s_frames = static_cast<u32>(std::clamp(Host::GetBaseIntSettingValue("EmuCore/Rollback", "RunAhead", 1), 0, 2));
		if (s_frames > 0)
			Console.WriteLn(fmt::format("Run-ahead: {} frame(s)", s_frames));
	}

	static void Disable(const char* reason)
	{
		Console.Warning(fmt::format("Run-ahead: switched off, {}", reason));
		Host::AddIconOSDMessage("RunAhead", ICON_FA_TRIANGLE_EXCLAMATION,
			fmt::format("Run-ahead switched off: {}", reason), Host::OSD_WARNING_DURATION);
		s_frames = 0;
	}
} // namespace RunAhead

u32 RunAhead::Frames()
{
	Init();
	return s_frames;
}

void RunAhead::BeginRealFrame()
{
	s_cycle_timer.Reset();
	Rollback::g_skip_present = true;
	Rollback::g_skip_audio = false;
	Rollback::g_skip_throttle = true;
}

void RunAhead::BeginSpeculativeFrame(u32 index)
{
	Rollback::g_skip_present = (index < s_frames);
	Rollback::g_skip_audio = true;
	Rollback::g_skip_throttle = true;
}

void RunAhead::ClearFlags()
{
	Rollback::g_skip_present = false;
	Rollback::g_skip_audio = false;
	Rollback::g_skip_throttle = false;
}

void RunAhead::EndCycle()
{
	ClearFlags();

	// Budget: the work must leave room in the frame, otherwise the game slows down.
	const double work_ms = s_cycle_timer.GetTimeMilliseconds();
	s_cycles++;
	s_work_sum_ms += work_ms;
	s_work_max_ms = std::max(s_work_max_ms, work_ms);
	s_over_budget = (work_ms > 15.5) ? (s_over_budget + 1) : 0;
	if ((s_cycles % 600) == 0)
	{
		Console.WriteLn(fmt::format("RUNAHEAD STATS: {} frame(s), work per frame avg {:.2f} ms, max {:.2f} ms",
			s_frames, s_work_sum_ms / 600.0, s_work_max_ms));
		s_work_sum_ms = 0.0;
		s_work_max_ms = 0.0;
	}
	if (s_over_budget >= 60)
		Disable("this PC needs more than a frame for it");

	// Sleep until the frame is due, then read the host input as late as possible.
	VMManager::Internal::Throttle();
	Host::PumpMessagesOnCPUThread();
	InputManager::PollSources();
	LatencyTest::OnRunAheadInputPolled();
}

bool RunAhead::IsActive()
{
	Init();
	// Netplay drives run-ahead itself; the self-test and the latency measurement need plain frames.
	return s_frames > 0 && !Netplay::IsActive() && !BootShortcut::IsActive() && !RollbackTest::IsBusy() &&
		   !LatencyTest::IsActive();
}

void RunAhead::OnInputPolled()
{
	if (!IsActive())
		return;
	s_exit_pending = true;
	Cpu->ExitExecution();
}

void RunAhead::OnExecutionExited()
{
	const bool pending = std::exchange(s_exit_pending, false);
	if (!IsActive())
	{
		// Switched off (budget) or standing down (self-test) while a cycle was running: finish it cleanly,
		// so whoever takes over sees the real state.
		if (s_state == State::Spec)
		{
			Error error;
			Rollback::Load(s_snapshot, &error);
		}
		if (s_state != State::Start)
		{
			ClearFlags();
			s_state = State::Start;
			s_snapshot.Release();
		}
		return;
	}
	if (!pending)
		return;

	Error error;
	switch (s_state)
	{
		case State::Start:
			BeginRealFrame();
			s_state = State::Real;
			return;

		case State::Real:
			// State after the real frame: keep it, then look ahead.
			if (!Rollback::Save(s_snapshot, &error))
			{
				Disable("snapshot failed");
				ClearFlags();
				s_state = State::Start;
				return;
			}
			s_spec_index = 1;
			BeginSpeculativeFrame(s_spec_index);
			s_state = State::Spec;
			return;

		case State::Spec:
			if (s_spec_index < s_frames)
			{
				BeginSpeculativeFrame(++s_spec_index);
				return;
			}
			if (!Rollback::Load(s_snapshot, &error))
			{
				Disable("restore failed");
				ClearFlags();
				s_state = State::Start;
				return;
			}
			EndCycle();
			BeginRealFrame();
			s_state = State::Real;
			return;
	}
}

void RunAhead::Abort()
{
	// State load, reset or shutdown: start over at the next frame boundary.
	ClearFlags();
	s_state = State::Start;
	s_exit_pending = false;
}
