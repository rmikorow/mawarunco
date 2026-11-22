// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// Run-ahead: hides game-internal input lag. Each real frame runs without presentation (audio on); then
// a snapshot is taken, N more frames are emulated with the latest input and the last one is shown
// (audio off), and the snapshot is restored. Afterwards the frame limiter sleeps and the host input is
// polled again, as late as possible before the next real frame reads it.
//
// [EmuCore/Rollback] RunAhead = 0 (off), 1 (default) or 2 frames. Offline this module drives itself; during
// netplay the netplay session uses the helpers below (and skips run-ahead in frames with a rollback).
// If the work no longer fits into a frame for a while, run-ahead switches itself off.
namespace RunAhead
{
	/// Configured number of frames (0 = off), after the automatic switch-off.
	u32 Frames();

	/// Offline driver: true while run-ahead runs without netplay.
	bool IsActive();
	void OnInputPolled();
	void OnExecutionExited();
	void Abort();

	// ---- Helpers shared with netplay -----------------------------------------------------------------

	/// The next frame is the real one: no presentation, no limiter sleep, audio on.
	void BeginRealFrame();
	/// Frame index (1-based) of N speculative frames: audio off, no limiter sleep, presented if it is
	/// the last one.
	void BeginSpeculativeFrame(u32 index);
	/// After the snapshot was restored: clear the flags, sleep in the frame limiter, poll host input.
	/// Also checks the time budget.
	void EndCycle();
	/// Clears all flags (e.g. when run-ahead is skipped for a frame).
	void ClearFlags();
} // namespace RunAhead
