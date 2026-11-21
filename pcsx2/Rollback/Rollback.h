// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SaveState.h"
#include "SIO/Pad/PadTypes.h"

#include <array>
#include <vector>

class Error;

// Rollback core: in-memory snapshots for rollback netcode.
//
// All functions run on the CPU thread, outside Cpu->Execute() (after the CPU loop has exited at the
// input poll), where the event test has completed and the state is consistent. Restores keep the
// recompiler caches and only drop compiled code in RAM pages that changed.
namespace Rollback
{
	/// Number of controller ports handled by rollback (no multitap).
	static constexpr u32 NUM_PORTS = 2;

	/// One frame of input for one controller port. 18 bytes, plain data (sent over the network as is).
	using PadInput = Pad::FrameInput;

	/// Emulator state for one frame: the savestate data plus the state the savestate format leaves out.
	struct Snapshot
	{
		ArchiveEntryList state;

		// Set at the end of an EE event test and consumed by the next one (runs the IOP even when it
		// is ahead of the EE). Snapshots are taken in between.
		bool iop_event_action = false;

		// Each SPU2 voice's current decoded ADPCM block (the PCM cache is not part of the state).
		std::vector<u8> spu2_voice_buffers;

		// The injected input of the current frame (controller input is not part of the savestate).
		bool inputs_injected = false;
		std::array<PadInput, NUM_PORTS> inputs = {};

		/// Releases the buffers (a later Save() allocates them again).
		void Release();
	};

	/// Takes a snapshot. Reuses the buffers of an earlier snapshot in the same object (about 1.5 ms).
	bool Save(Snapshot& snapshot, Error* error);

	/// Restores a snapshot (about 2 ms).
	bool Load(const Snapshot& snapshot, Error* error);

	/// Restores only the parts that are not in the savestate data (for callers that load the savestate
	/// part themselves, e.g. diagnostics with a component mask).
	void LoadExtras(const Snapshot& snapshot);

	/// Name of the first part that differs between two snapshots, nullptr if they are identical.
	const char* FirstDifference(const Snapshot& a, const Snapshot& b);

	/// Same, for the parts that are not in the savestate data only.
	const char* FirstExtrasDifference(const Snapshot& a, const Snapshot& b);

	/// Starts input injection: from now on the game only sees the inputs given to SetInput(), host input
	/// does not reach it any more. Both ports start with "nothing pressed". Empty ports stay empty. Fails
	/// if a port has a controller type that does not support injection (only the DualShock 2 does).
	bool BeginInputInjection(Error* error);

	/// Ends input injection, the game sees host input again.
	void EndInputInjection();

	bool IsInputInjectionActive();

	/// What the host controller of a port is pressing right now (the local player's input).
	PadInput CaptureLocalInput(u32 port);

	/// Sets the input the game sees on a port until the next SetInput(). Call it at the input poll point,
	/// before the frame runs. Requires active injection.
	void SetInput(u32 port, const PadInput& input);

	/// The input the game currently sees on a port (while injection is active).
	PadInput GetInput(u32 port);

	/// True while frames are resimulated after a rollback. The frame limiter, GS presentation and audio
	/// output are skipped meanwhile; emulation itself is unchanged. CPU thread only.
	extern bool g_resimulating;
} // namespace Rollback
