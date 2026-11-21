// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SaveState.h"

#include <vector>

class Error;

// Rollback core: in-memory snapshots for rollback netcode.
//
// All functions run on the CPU thread, outside Cpu->Execute() (after the CPU loop has exited at the
// input poll), where the event test has completed and the state is consistent. Restores keep the
// recompiler caches and only drop compiled code in RAM pages that changed.
namespace Rollback
{
	/// Emulator state for one frame: the savestate data plus the state the savestate format leaves out.
	struct Snapshot
	{
		ArchiveEntryList state;

		// Set at the end of an EE event test and consumed by the next one (runs the IOP even when it
		// is ahead of the EE). Snapshots are taken in between.
		bool iop_event_action = false;

		// Each SPU2 voice's current decoded ADPCM block (the PCM cache is not part of the state).
		std::vector<u8> spu2_voice_buffers;

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

	/// True while frames are resimulated after a rollback. The frame limiter, GS presentation and audio
	/// output are skipped meanwhile; emulation itself is unchanged. CPU thread only.
	extern bool g_resimulating;
} // namespace Rollback
