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
// CPU thread at the input poll point of every vsync.
namespace RollbackTest
{
	/// Requests a test over the given number of frames. Starts at the next input poll.
	void Request(u32 frames);

	/// Called from VMManager::Internal::PollInputOnCPUThread after the pads have been polled.
	void OnInputPolled();

	/// Cancels a running test (state load, reset, shutdown).
	void Abort();
} // namespace RollbackTest
