// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Input latency measurement in emulated frames (diagnostics).
//
// For each trial: snapshot, run N frames without input (pass A), restore, run the same N frames with a
// button pressed in frame 0 (pass B). Emulation is deterministic, so the first frame whose GS memory
// differs between the passes is the frame in which the reaction is drawn, and the first frame whose
// EE memory differs is the frame in which the game consumed the input. Host-side latency (queueing,
// rendering, presentation) is not part of this number.
//
// Enabled with [EmuCore/Rollback] LatencyTest = <trials per button>, runs once after the VM has
// settled, logs "LATENCY" lines and (with LatencyTestExit = true) shuts the VM down afterwards.
namespace LatencyTest
{
	/// True while a measurement is configured or running.
	bool IsActive();

	/// Called from the input poll (inside the vsync event).
	void OnInputPolled();

	/// Called after Cpu->Execute() returned.
	void OnExecutionExited();

	/// Cancels a running measurement (state load, reset, shutdown).
	void Abort();

	// ---- Host latency statistics ([EmuCore/Rollback] LatencyStats = true) and presentation order ----

	/// [EmuCore/Rollback] PresentBeforeThrottle: hand the frame to the GS before the frame limiter sleeps
	/// (instead of after), so a finished frame is shown right away. Host timing only. Default: on.
	bool PresentBeforeThrottle();

	/// CPU thread: a vsync packet was posted to the GS thread.
	void OnVsyncPosted();

	/// CPU thread: host input was polled for the next frame.
	void OnInputPolledHost();

	/// GS thread: a vsync packet was processed; skipped = not presented (run-ahead, resimulation).
	void OnVsyncPresented(bool skipped);

	/// CPU thread, run-ahead: host input was polled for the next cycle. With run-ahead the reaction to it
	/// is presented in the same cycle (the speculative frame), so statistics switch to that.
	void OnRunAheadInputPolled();
} // namespace LatencyTest
