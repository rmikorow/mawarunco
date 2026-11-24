// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Rollback fork: performance report per session, for players to send when the game stutters or gets
// slower. Written to logs\perf-<date>-<time>.txt (the last 10 are kept): the PC and settings at the
// start, one line every 10 seconds (frame work, stutter, memory, snapshot save/restore times, netplay
// rollbacks and ping) and a summary with trends and plain warnings at the end.
//
// It never contains IP addresses, file paths (they often contain the Windows user name) or player
// names, so it can be shared as it is. [EmuCore/Rollback] PerfReport = false turns it off.
namespace PerfReport
{
	/// Called by the frame limiter once per real frame (not for speculative or resimulated frames):
	/// before it sleeps (the frame's work is done) and after it (the frame ends).
	void OnFrameWorkDone();
	void OnFrameEnd();

	/// Called by Rollback::Save/Load with the time they took.
	void OnSnapshot(bool load, double ms);

	/// Called when the virtual machine shuts down: writes the summary and closes the file.
	void OnShutdown();
} // namespace PerfReport
