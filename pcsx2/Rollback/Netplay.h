// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string_view>

class Error;

// Rollback netplay on top of GGRS (3rdparty/ggrs-capi) and the rollback core (Rollback.h).
//
// The session runs on the CPU thread at frame boundaries: the CPU loop exits after every input poll,
// then the GGRS requests of the frame are answered in order (save, load, advance). An advance sets
// the inputs of both ports and lets the CPU emulate exactly one frame; further advances in the same
// list are resimulated frames (no frame limiter, presentation or audio). While the session has to
// wait (peers synchronizing, remote input too far behind, time synchronization), the CPU does not run.
//
// Player handle = controller port. The local player's input always comes from the host controller of
// port 1 and is injected on the port of the local player handle.
namespace Netplay
{
	/// Sets the session to start once the VM runs (command line "-netplay <spec>"). Specs:
	///   "host[=7000][,delay=1][,rollback=2]"         player 1, needs the UDP port forwarded
	///   "join=203.0.113.5[:7000][,delay=1][,rollback=2]"  player 2, no port forwarding needed
	///   "port=7000,remote=127.0.0.1:7001,player=0[,delay=1][,rollback=2][,desync=60][,sparse=1][,testinput=1]"
	///   "synctest[,distance=2][,delay=0]"  (local determinism test, no network)
	bool SetLaunchOptions(std::string_view spec, Error* error);

	/// True while a session runs or is about to start.
	bool IsActive();

	/// Called before Cpu->Execute(). Returns false while the CPU must not run (waiting for the peer).
	bool BeforeExecute();

	/// Called from the input poll (inside the vsync event): requests a CPU loop exit.
	void OnInputPolled();

	/// Called after Cpu->Execute() returned: answers the pending GGRS requests.
	void OnExecutionExited();

	/// Ends the session (VM shutdown, reset, state load). The game sees host input again.
	void Stop();
} // namespace Netplay
