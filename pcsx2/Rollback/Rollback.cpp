// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Rollback/Rollback.h"

#include "R3000A.h"
#include "SPU2/spu2.h"

#include "common/Error.h"

#include <cstring>

bool Rollback::g_resimulating = false;

void Rollback::Snapshot::Release()
{
	state.Clear();
	ArchiveEntryList::VmStateBuffer().swap(state.GetBuffer());
	spu2_voice_buffers = {};
}

bool Rollback::Save(Snapshot& snapshot, Error* error)
{
	if (!SaveState_DownloadStateInto(snapshot.state, error))
		return false;

	snapshot.iop_event_action = iopEventAction;
	SPU2::SaveVoiceBuffers(snapshot.spu2_voice_buffers);
	return true;
}

bool Rollback::Load(const Snapshot& snapshot, Error* error)
{
	if (!SaveState_LoadFromMemory(snapshot.state, error, false))
		return false;

	LoadExtras(snapshot);
	return true;
}

void Rollback::LoadExtras(const Snapshot& snapshot)
{
	iopEventAction = snapshot.iop_event_action;
	SPU2::LoadVoiceBuffers(snapshot.spu2_voice_buffers);
}

const char* Rollback::FirstDifference(const Snapshot& a, const Snapshot& b)
{
	if (a.state.GetLength() != b.state.GetLength())
		return "entry count";

	for (size_t i = 0; i < a.state.GetLength(); i++)
	{
		const ArchiveEntry& ea = a.state[i];
		const ArchiveEntry& eb = b.state[i];
		if (ea.GetDataSize() != eb.GetDataSize() ||
			std::memcmp(a.state.GetPtr(ea.GetDataIndex()), b.state.GetPtr(eb.GetDataIndex()), ea.GetDataSize()) != 0)
		{
			return ea.GetFilename().c_str();
		}
	}

	return FirstExtrasDifference(a, b);
}

const char* Rollback::FirstExtrasDifference(const Snapshot& a, const Snapshot& b)
{
	if (a.iop_event_action != b.iop_event_action)
		return "iopEventAction";
	if (a.spu2_voice_buffers != b.spu2_voice_buffers)
		return "SPU2 voice buffers";
	return nullptr;
}
