// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Rollback/Rollback.h"

#include "R3000A.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "SIO/Sio.h"
#include "SPU2/spu2.h"

#include "common/Assertions.h"
#include "common/Error.h"

#include "fmt/format.h"

#include <cstring>

bool Rollback::g_resimulating = false;

static bool s_input_injection = false;

static PadBase* GetPortPad(u32 port)
{
	return Pad::GetPad(sioConvertPortAndSlotToPad(port, 0));
}

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
	for (u32 port = 0; port < NUM_PORTS; port++)
	{
		const PadBase* pad = GetPortPad(port);
		snapshot.pad_eject_ticks[port] = pad ? pad->ejectTicks : 0;
	}
	snapshot.inputs_injected = s_input_injection;
	for (u32 port = 0; port < NUM_PORTS; port++)
		snapshot.inputs[port] = s_input_injection ? GetInput(port) : PadInput{};
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
	for (u32 port = 0; port < NUM_PORTS; port++)
	{
		if (PadBase* pad = GetPortPad(port))
			pad->ejectTicks = snapshot.pad_eject_ticks[port];
	}
	if (s_input_injection && snapshot.inputs_injected)
	{
		for (u32 port = 0; port < NUM_PORTS; port++)
			SetInput(port, snapshot.inputs[port]);
	}
}

bool Rollback::BeginInputInjection(Error* error, bool require_all_ports)
{
	for (u32 port = 0; port < NUM_PORTS; port++)
	{
		// An empty port stays empty (the game sees no controller there), it needs no injection.
		const PadBase* pad = GetPortPad(port);
		if (!pad || pad->GetType() == Pad::ControllerType::NotConnected)
		{
			if (require_all_ports)
			{
				Error::SetString(error, fmt::format("Controller port {} is empty. Set both controller ports to "
													"DualShock 2 (Settings, Controllers).", port + 1));
				return false;
			}
			continue;
		}
		if (!pad->SupportsInputInjection())
		{
			Error::SetString(error, fmt::format("Controller port {} does not support rollback input (use a DualShock 2).", port + 1));
			return false;
		}
	}

	s_input_injection = true;
	for (u32 port = 0; port < NUM_PORTS; port++)
		SetInput(port, PadInput{});
	return true;
}

void Rollback::EndInputInjection()
{
	if (!s_input_injection)
		return;

	s_input_injection = false;
	for (u32 port = 0; port < NUM_PORTS; port++)
	{
		if (PadBase* pad = GetPortPad(port))
			pad->SetInjectedInput(nullptr);
	}
}

bool Rollback::IsInputInjectionActive()
{
	return s_input_injection;
}

Rollback::PadInput Rollback::CaptureLocalInput(u32 port)
{
	const PadBase* pad = GetPortPad(port);
	return pad ? pad->CaptureLiveInput() : PadInput{};
}

void Rollback::SetInput(u32 port, const PadInput& input)
{
	pxAssert(s_input_injection);
	if (PadBase* pad = GetPortPad(port))
		pad->SetInjectedInput(&input);
}

Rollback::PadInput Rollback::GetInput(u32 port)
{
	const PadBase* pad = GetPortPad(port);
	const PadInput* input = pad ? pad->GetInjectedInput() : nullptr;
	return input ? *input : PadInput{};
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
	if (a.inputs_injected != b.inputs_injected || a.inputs != b.inputs)
		return "injected input";
	if (a.pad_eject_ticks != b.pad_eject_ticks)
		return "pad eject ticks";
	return nullptr;
}
