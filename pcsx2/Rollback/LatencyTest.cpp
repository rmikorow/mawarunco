// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Rollback/LatencyTest.h"
#include "Rollback/Rollback.h"

#include "Host.h"
#include "R5900.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/HostSys.h"
#include "common/Error.h"

#include "fmt/format.h"

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include "xxhash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace LatencyTest
{
	enum class Phase
	{
		Off,
		Settle,   // let the game run normally for a while after boot / state load
		PassA,    // no input
		PassB,    // button pressed in frames [0, HOLD_FRAMES)
		Done,
	};

	struct Button
	{
		const char* name;
		u16 bit; // active-low bit in Pad::FrameInput::buttons
	};

	// Poll byte layout: bits 4-7 triangle, circle, cross, square; 12-15 up, right, down, left.
	static constexpr Button BUTTONS[] = {
		{"right", 1u << 13},
		{"square", 1u << 7},
		{"triangle", 1u << 4},
		{"circle", 1u << 5},
		{"cross", 1u << 6},
	};

	static constexpr u32 SETTLE_FRAMES = 120;
	static constexpr u32 PASS_FRAMES = 45;
	static constexpr u32 HOLD_FRAMES = 4;
	static constexpr u32 GAP_FRAMES = 13; // neutral frames between trials, shifts the frame phase

	struct FrameHashes
	{
		u64 ee = 0;
		u64 gs = 0;
	};

	struct Result
	{
		const char* button;
		int ee_frame; // first frame whose EE memory differs (-1 = none within PASS_FRAMES)
		int gs_frame; // first frame whose GS memory differs
	};

	static bool s_init = false;
	static bool s_exit_after = true;
	static u32 s_trials_per_button = 0;
	static Phase s_phase = Phase::Off;
	static bool s_exit_pending = false;
	static u32 s_counter = 0; // frames in the current phase
	static u32 s_trial = 0;   // index over BUTTONS x trials
	static Rollback::Snapshot s_start;
	static Rollback::Snapshot s_scratch;
	static std::vector<FrameHashes> s_pass_a;
	static std::vector<FrameHashes> s_pass_b;
	static std::vector<Result> s_results;

	static const Button& CurrentButton() { return BUTTONS[s_trial % std::size(BUTTONS)]; }
	static u32 TotalTrials() { return s_trials_per_button * static_cast<u32>(std::size(BUTTONS)); }
	static FrameHashes HashCurrentState();
	static void SetInput(bool pressed);
	static void StartTrial();
	static void FinishTrial();
	static void LogSummary();
	static void Finish();
} // namespace LatencyTest

namespace LatencyTest
{
	// Host latency statistics. The CPU thread counts posted vsync packets; the input poll after vsync
	// packet n belongs to frame n + 1, whose image goes out with packet n + 1. A reaction is drawn one
	// frame later (see the measurement above), so it reaches the screen with packet n + 2.
	static constexpr u32 RING = 64;
	static bool s_stats_init = false;
	static bool s_stats = false;
	static bool s_present_first = false;
	static std::atomic<u32> s_posted{0};
	static u32 s_presented = 0; // GS thread
	static std::array<std::atomic<u64>, RING> s_poll_time = {};
	static u64 s_last_present = 0;
	static std::atomic<u64> s_runahead_poll{0}; // last run-ahead input poll (0 = run-ahead not used)
	struct HostStats
	{
		u32 samples = 0;
		double sum_ms = 0.0;
		double max_ms = 0.0;
		double interval_dev_sum = 0.0; // |present interval - frame time|
		double interval_max = 0.0;
	};
	static HostStats s_host;

	static void InitStats()
	{
		if (s_stats_init)
			return;
		s_stats_init = true;
		s_stats = Host::GetBaseBoolSettingValue("EmuCore/Rollback", "LatencyStats", false);
		// On by default in this fork: about 13 ms less input latency (33.6 -> 20.8 ms from input poll to the
		// reaction frame), presentation deviates ~0.2 ms more from the frame time on average.
		s_present_first = Host::GetBaseBoolSettingValue("EmuCore/Rollback", "PresentBeforeThrottle", true);
		if (s_stats || s_present_first)
			Console.WriteLn(fmt::format("LATENCY STATS: {}, present before throttle: {}", s_stats ? "on" : "off",
				s_present_first ? "yes" : "no"));
	}
} // namespace LatencyTest

bool LatencyTest::PresentBeforeThrottle()
{
	InitStats();
	return s_present_first;
}

void LatencyTest::OnVsyncPosted()
{
	InitStats();
	if (s_stats)
		s_posted.fetch_add(1, std::memory_order_release);
}

void LatencyTest::OnInputPolledHost()
{
	if (!s_stats)
		return;
	const u32 n = s_posted.load(std::memory_order_acquire);
	s_poll_time[n % RING].store(GetCPUTicks(), std::memory_order_release);
}

void LatencyTest::OnRunAheadInputPolled()
{
	if (s_stats)
		s_runahead_poll.store(GetCPUTicks(), std::memory_order_release);
}

void LatencyTest::OnVsyncPresented(bool skipped)
{
	if (!s_stats)
		return;
	const u64 now = GetCPUTicks();
	const double tick_ms = 1000.0 / static_cast<double>(GetTickFrequency());
	const u32 j = ++s_presented; // packets are processed in posting order, 1-based like s_posted

	// Run-ahead: the presented (speculative) frame already shows the reaction to the last input poll.
	const u64 ra_poll = s_runahead_poll.load(std::memory_order_acquire);
	if (ra_poll != 0)
	{
		if (skipped)
			return;
		if (s_last_present != 0)
		{
			const double interval = static_cast<double>(now - s_last_present) * tick_ms;
			const double dev = std::abs(interval - 1000.0 / 59.94);
			s_host.interval_dev_sum += dev;
			s_host.interval_max = std::max(s_host.interval_max, dev);
		}
		s_last_present = now;
		if (now > ra_poll)
		{
			const double ms = static_cast<double>(now - ra_poll) * tick_ms;
			s_host.samples++;
			s_host.sum_ms += ms;
			s_host.max_ms = std::max(s_host.max_ms, ms);
		}
		if (s_host.samples >= 600)
		{
			Console.WriteLn(fmt::format("LATENCY STATS: input poll -> reaction frame handed to the GPU: avg {:.1f} ms, max "
										"{:.1f} ms; frame pacing: avg deviation {:.2f} ms, max {:.2f} ms (run-ahead)",
				s_host.sum_ms / s_host.samples, s_host.max_ms, s_host.interval_dev_sum / s_host.samples,
				s_host.interval_max));
			s_host = {};
		}
		return;
	}
	if (s_last_present != 0)
	{
		const double interval = static_cast<double>(now - s_last_present) * tick_ms;
		const double dev = std::abs(interval - 1000.0 / 59.94);
		s_host.interval_dev_sum += dev;
		s_host.interval_max = std::max(s_host.interval_max, dev);
	}
	s_last_present = now;
	if (j >= 2)
	{
		const u64 polled = s_poll_time[(j - 2) % RING].load(std::memory_order_acquire);
		if (polled != 0 && now > polled)
		{
			const double ms = static_cast<double>(now - polled) * tick_ms;
			s_host.samples++;
			s_host.sum_ms += ms;
			s_host.max_ms = std::max(s_host.max_ms, ms);
		}
	}
	if (s_host.samples >= 600)
	{
		Console.WriteLn(fmt::format("LATENCY STATS: input poll -> reaction frame handed to the GPU: avg {:.1f} ms, max "
									"{:.1f} ms; frame pacing: avg deviation {:.2f} ms, max {:.2f} ms",
			s_host.sum_ms / s_host.samples, s_host.max_ms, s_host.interval_dev_sum / s_host.samples, s_host.interval_max));
		s_host = {};
	}
}

bool LatencyTest::IsActive()
{
	if (!s_init)
	{
		s_init = true;
		s_trials_per_button = static_cast<u32>(Host::GetBaseIntSettingValue("EmuCore/Rollback", "LatencyTest", 0));
		s_exit_after = Host::GetBaseBoolSettingValue("EmuCore/Rollback", "LatencyTestExit", true);
		if (s_trials_per_button > 0)
		{
			s_phase = Phase::Settle;
			Console.WriteLn(fmt::format("LATENCY: {} trials per button, {} buttons", s_trials_per_button, std::size(BUTTONS)));
		}
	}
	return s_phase != Phase::Off && s_phase != Phase::Done;
}

void LatencyTest::OnInputPolled()
{
	if (!IsActive())
		return;
	s_exit_pending = true;
	Cpu->ExitExecution();
}

LatencyTest::FrameHashes LatencyTest::HashCurrentState()
{
	FrameHashes h;
	Error error;
	if (!Rollback::Save(s_scratch, &error))
	{
		Console.Error(fmt::format("LATENCY: snapshot failed: {}", error.GetDescription()));
		return h;
	}
	for (size_t i = 0; i < s_scratch.state.GetLength(); i++)
	{
		const ArchiveEntry& e = s_scratch.state[i];
		const u64 v = XXH3_64bits(s_scratch.state.GetPtr(e.GetDataIndex()), e.GetDataSize());
		if (e.GetFilename() == "eeMemory.bin")
			h.ee = v;
		else if (e.GetFilename() == "GS.bin")
			h.gs = v;
	}
	return h;
}

void LatencyTest::SetInput(bool pressed)
{
	Rollback::PadInput input;
	if (pressed)
		input.buttons = static_cast<u16>(~CurrentButton().bit);
	Rollback::SetInput(0, input);
}

void LatencyTest::StartTrial()
{
	Error error;
	if (!Rollback::Save(s_start, &error))
	{
		Console.Error(fmt::format("LATENCY: snapshot failed: {}", error.GetDescription()));
		Finish();
		return;
	}
	s_pass_a.clear();
	s_pass_b.clear();
	s_counter = 0;
	s_phase = Phase::PassA;
	SetInput(false); // input of frame 0
}

void LatencyTest::FinishTrial()
{
	Result r = {CurrentButton().name, -1, -1};
	for (u32 f = 0; f < PASS_FRAMES; f++)
	{
		if (r.ee_frame < 0 && s_pass_a[f].ee != s_pass_b[f].ee)
			r.ee_frame = static_cast<int>(f);
		if (r.gs_frame < 0 && s_pass_a[f].gs != s_pass_b[f].gs)
			r.gs_frame = static_cast<int>(f);
	}
	s_results.push_back(r);
	Console.WriteLn(fmt::format("LATENCY trial {}/{}: {:<8} pressed in frame 0 -> game memory reacts in frame {}, "
								"graphics in frame {}",
		s_trial + 1, TotalTrials(), r.button, r.ee_frame, r.gs_frame));
}

void LatencyTest::OnExecutionExited()
{
	if (!std::exchange(s_exit_pending, false) || !IsActive())
		return;

	switch (s_phase)
	{
		case Phase::Settle:
			if (++s_counter >= SETTLE_FRAMES)
			{
				Error error;
				if (!Rollback::BeginInputInjection(&error))
				{
					Console.Error(fmt::format("LATENCY: {}", error.GetDescription()));
					Finish();
					return;
				}
				StartTrial();
			}
			return;

		case Phase::PassA:
			// The frame that just ran is s_counter.
			s_pass_a.push_back(HashCurrentState());
			if (++s_counter < PASS_FRAMES)
			{
				SetInput(false);
				return;
			}
			{
				Error error;
				if (!Rollback::Load(s_start, &error))
				{
					Console.Error(fmt::format("LATENCY: restore failed: {}", error.GetDescription()));
					Finish();
					return;
				}
			}
			s_counter = 0;
			s_phase = Phase::PassB;
			SetInput(true); // frame 0 of pass B
			return;

		case Phase::PassB:
			s_pass_b.push_back(HashCurrentState());
			if (++s_counter < PASS_FRAMES)
			{
				SetInput(s_counter < HOLD_FRAMES);
				return;
			}
			FinishTrial();
			s_trial++;
			if (s_trial >= TotalTrials())
			{
				LogSummary();
				Finish();
				return;
			}
			// A few neutral frames before the next trial (also changes the frame phase).
			s_counter = 0;
			s_phase = Phase::Settle;
			// Re-use the settle phase with a short count.
			s_counter = SETTLE_FRAMES - GAP_FRAMES;
			SetInput(false);
			return;

		default:
			return;
	}
}

void LatencyTest::LogSummary()
{
	Console.WriteLn("LATENCY SUMMARY (frames after the frame in which the button was first read, 0 = same frame):");
	for (const Button& b : BUTTONS)
	{
		std::string ee, gs;
		for (const Result& r : s_results)
		{
			if (r.button != b.name)
				continue;
			ee += fmt::format(" {}", r.ee_frame);
			gs += fmt::format(" {}", r.gs_frame);
		}
		Console.WriteLn(fmt::format("  {:<8} game memory:{}   graphics:{}", b.name, ee, gs));
	}
}

void LatencyTest::Finish()
{
	s_phase = Phase::Done;
	Rollback::EndInputInjection();
	s_start.Release();
	s_scratch.Release();
	if (s_exit_after)
		Host::RequestVMShutdown(false, false, false);
}

void LatencyTest::Abort()
{
	// The boot state load also calls this; only a running measurement is affected.
	if (s_phase == Phase::PassA || s_phase == Phase::PassB)
	{
		Console.Warning("LATENCY: aborted.");
		Finish();
	}
}
