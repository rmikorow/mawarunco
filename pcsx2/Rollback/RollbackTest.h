// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// Rollback self-test (M3 feasibility check for rollback netcode).
//
// 1. Take an in-memory snapshot S0 and record the pad input of both ports for N frames.
// 2. Take snapshot S_N, restore S0 from memory and replay the recorded input.
// 3. Take snapshot S_N' and compare it with S_N, component by component.
//
// If emulation is deterministic, S_N and S_N' are identical. All work happens on the
// CPU thread: input at the input poll point of every vsync, snapshots after the CPU loop exits.
namespace RollbackTest
{
	/// Diagnostic: what the recording pass does right after taking S0. The replay pass always does a
	/// full restore, so a variant that makes the test pass identifies the restore side effect.
	struct Variant
	{
		const char* name;
		bool restore;         ///< restore (parts of) S0
		bool clear_caches;    ///< clear the CPU/recompiler caches
		u32 component_mask;   ///< see SaveState_LoadFromMemory
	};

	/// The real test: the recording pass does nothing.
	inline constexpr Variant VARIANT_NONE = {"none", false, false, 0};
	/// Full restore in the recording pass too.
	inline constexpr Variant VARIANT_FULL = {"full restore", true, true, 0xFFFFFFFFu};

	/// Requests a test over the given number of frames. Starts at the next input poll.
	void Request(u32 frames, const Variant& variant = VARIANT_NONE);

	/// Runs a series of diagnostic variants back to back (one test each).
	void RequestBattery(u32 frames);

	/// Called from VMManager::Internal::PollInputOnCPUThread after the pads have been polled.
	/// Records or injects the pad input (Rollback input injection) and requests a CPU loop exit.
	void OnInputPolled();

	/// Called from VMManager::Execute before the CPU loop is entered (diagnostic timer trace).
	void OnBeforeExecute();

	/// Called from VMManager::Execute after the CPU loop has exited. Snapshots and restores happen
	/// here, where the event test has completed and the state is consistent.
	void OnExecutionExited();

	/// Cancels a running test (state load, reset, shutdown).
	void Abort();

	/// True while a test runs, is requested, or an unattended series still has runs left. Run-ahead stands
	/// down meanwhile.
	bool IsBusy();

	/// Diagnostics: timer reads are traced while this is set (first frames of both passes).
	extern bool g_trace_timers;
	/// unit 0 = EE, 1 = IOP. pc is the last written PC (usually the block start in the recompilers).
	void TraceTimerRead(u32 unit, u32 index, u32 pc, u64 cycle, u32 value);
} // namespace RollbackTest
