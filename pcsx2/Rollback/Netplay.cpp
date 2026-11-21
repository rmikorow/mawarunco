// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Rollback/Netplay.h"
#include "Rollback/Rollback.h"

#include "Config.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "R5900.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Threading.h"
#include "common/Timer.h"

#include "fmt/format.h"
#include "ggrs_capi.h"

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include "xxhash.h"

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

static_assert(sizeof(Rollback::PadInput) == GGRS_INPUT_SIZE);

namespace Netplay
{
	struct Options
	{
		bool synctest = false;
		u32 check_distance = 2;
		u16 local_port = 7000;
		std::string remote;
		u32 local_player = 0;
		u32 input_delay = 1;
		u32 max_prediction = 2;
		u32 desync_interval = 60;
		bool sparse_saving = false;
		bool test_input = false; // testing: generated local input instead of the host controller
		bool dump = false;       // diagnostics: write the snapshots of frames 0 and 60 to the log folder
	};

	static constexpr u32 NUM_PLAYERS = Rollback::NUM_PORTS;
	static constexpr u16 DEFAULT_PORT = 7000;
	static constexpr u32 LOCAL_CONTROLLER_PORT = 0; // host controller bound to port 1
	static constexpr const char* OSD_KEY = "Netplay";
	static constexpr double FRAME_MS = 1000.0 / 59.94;

	static std::optional<Options> s_pending; // set by SetLaunchOptions, started at the first exit
	static Options s_options;
	static GgrsSession* s_session = nullptr;
	static std::vector<std::unique_ptr<Rollback::Snapshot>> s_ring; // not movable, so held by pointer

	static bool s_exit_pending = false;
	static bool s_need_advance = false; // all requests answered, the next frame needs ggrs_advance_frame()
	static int s_req_count = 0;
	static int s_req_index = 0;
	static bool s_was_running = false;
	static Common::Timer::Value s_wait_until = 0;
	static int s_timesync_frame = -1; // frame for which the time sync wait was applied

	struct Stats
	{
		u64 frames = 0;      // all emulated frames (real and resimulated)
		u64 real_frames = 0; // frames shown to the player
		u64 rollbacks = 0;
		u64 resimulated = 0;
		u64 waits = 0;
		u32 max_rollback = 0;
		double timesync_ms = 0.0;
	};
	static Stats s_stats;
	static u32 s_rollback_depth = 0; // advances since the last load in the current request list

	static bool Start(const Options& opts, Error* error);
	static void LogStats();
	static void ProcessRequests();
	static void HandleEvents();
	static bool AdvanceSession();
	static std::pair<u64, u64> Checksum(const Rollback::Snapshot& snapshot);
	static std::string LastGgrsError();
	static Rollback::PadInput LocalInput(u32 port);
	static void DumpSnapshot(const Rollback::Snapshot& snapshot, int frame);
	static void Fail(std::string_view what);
} // namespace Netplay

std::string Netplay::LastGgrsError()
{
	char buf[512];
	ggrs_last_error(buf, sizeof(buf));
	return buf;
}

bool Netplay::SetLaunchOptions(std::string_view spec, Error* error)
{
	Options opts;
	bool have_remote = false;
	for (const std::string_view part : StringUtil::SplitString(spec, ',', true))
	{
		const std::string_view key_value = StringUtil::StripWhitespace(part);
		const size_t eq = key_value.find('=');
		const std::string_view key = (eq == std::string_view::npos) ? key_value : key_value.substr(0, eq);
		const std::string_view value = (eq == std::string_view::npos) ? std::string_view() : key_value.substr(eq + 1);
		auto number = [&](u32 max) -> std::optional<u32> {
			const std::optional<u32> v = StringUtil::FromChars<u32>(value);
			if (!v.has_value() || v.value() > max)
			{
				Error::SetString(error, fmt::format("Netplay: invalid value '{}' for '{}'", value, key));
				return std::nullopt;
			}
			return v;
		};

		if (key == "synctest")
			opts.synctest = true;
		else if (key == "host")
		{
			// Host: player 1, waits on the given port (default 7000) and learns the peer's address from
			// its first packet. Only the host needs a forwarded UDP port.
			opts.local_player = 0;
			opts.remote = "any";
			opts.local_port = DEFAULT_PORT;
			have_remote = true;
			if (!value.empty())
			{
				const std::optional<u32> v = number(65535);
				if (!v)
					return false;
				opts.local_port = static_cast<u16>(*v);
			}
		}
		else if (key == "join")
		{
			// Join: player 2, connects to the host's "ip[:port]" from any free local port.
			if (value.empty())
			{
				Error::SetString(error, "Netplay: 'join' needs the host address, e.g. join=203.0.113.5:7000");
				return false;
			}
			opts.local_player = 1;
			opts.local_port = 0;
			opts.remote = (value.find(':') == std::string_view::npos) ? fmt::format("{}:{}", value, DEFAULT_PORT) :
																		std::string(value);
			have_remote = true;
		}
		else if (key == "remote")
		{
			opts.remote = std::string(value);
			have_remote = true;
		}
		else if (key == "port")
		{
			const std::optional<u32> v = number(65535);
			if (!v)
				return false;
			opts.local_port = static_cast<u16>(*v);
		}
		else if (key == "player")
		{
			const std::optional<u32> v = number(NUM_PLAYERS - 1);
			if (!v)
				return false;
			opts.local_player = *v;
		}
		else if (key == "delay")
		{
			const std::optional<u32> v = number(10);
			if (!v)
				return false;
			opts.input_delay = *v;
		}
		else if (key == "rollback")
		{
			const std::optional<u32> v = number(16);
			if (!v)
				return false;
			opts.max_prediction = *v;
		}
		else if (key == "desync")
		{
			const std::optional<u32> v = number(100000);
			if (!v)
				return false;
			opts.desync_interval = *v;
		}
		else if (key == "sparse")
		{
			const std::optional<u32> v = number(1);
			if (!v)
				return false;
			opts.sparse_saving = (*v != 0);
		}
		else if (key == "testinput")
		{
			const std::optional<u32> v = number(1);
			if (!v)
				return false;
			opts.test_input = (*v != 0);
		}
		else if (key == "dump")
		{
			const std::optional<u32> v = number(1);
			if (!v)
				return false;
			opts.dump = (*v != 0);
		}
		else if (key == "distance")
		{
			const std::optional<u32> v = number(16);
			if (!v)
				return false;
			opts.check_distance = *v;
		}
		else
		{
			Error::SetString(error, fmt::format("Netplay: unknown option '{}'", key));
			return false;
		}
	}

	if (!opts.synctest && !have_remote)
	{
		Error::SetString(error, "Netplay: use 'host', 'join=ip[:port]' or 'synctest'.");
		return false;
	}

	s_pending = std::move(opts);
	return true;
}

bool Netplay::IsActive()
{
	return s_session || s_pending.has_value();
}

bool Netplay::Start(const Options& opts, Error* error)
{
	// A real match needs a DualShock 2 on both ports on both machines; the sync test also runs with an
	// empty port 2.
	if (!Rollback::BeginInputInjection(error, !opts.synctest))
		return false;

	if (opts.synctest)
	{
		s_session = ggrs_synctest_start(NUM_PLAYERS, opts.check_distance, opts.input_delay,
			std::max(opts.max_prediction, opts.check_distance + 1));
	}
	else
	{
		GgrsP2PConfig cfg = {};
		cfg.num_players = NUM_PLAYERS;
		cfg.local_player = opts.local_player;
		cfg.local_port = opts.local_port;
		cfg.remote_addr = opts.remote.c_str();
		cfg.input_delay = opts.input_delay;
		cfg.max_prediction = opts.max_prediction;
		cfg.desync_interval = opts.desync_interval;
		cfg.sparse_saving = opts.sparse_saving;
		cfg.fps = 60;
		s_session = ggrs_p2p_start(&cfg);
	}

	if (!s_session)
	{
		Rollback::EndInputInjection();
		Error::SetString(error, fmt::format("GGRS: {}", LastGgrsError()));
		return false;
	}

	s_options = opts;
	// One slot per frame that can be rolled back to, plus the current frame and a margin.
	const u32 depth = opts.synctest ? std::max(opts.max_prediction, opts.check_distance + 1) : opts.max_prediction;
	s_ring.clear();
	for (u32 i = 0; i < depth + 3; i++)
		s_ring.push_back(std::make_unique<Rollback::Snapshot>());
	s_need_advance = true;
	s_req_count = s_req_index = 0;
	s_was_running = false;
	s_wait_until = 0;
	s_timesync_frame = -1;
	s_stats = {};

	if (opts.synctest)
	{
		Console.WriteLn(fmt::format("Netplay: sync test started (check distance {}, delay {})", opts.check_distance,
			opts.input_delay));
	}
	else
	{
		Console.WriteLn(fmt::format("Netplay: session started, local player {} on UDP port {}, remote {}, delay {}, "
									"rollback {}, desync check every {} frames{}",
			opts.local_player + 1, opts.local_port, opts.remote, opts.input_delay, opts.max_prediction,
			opts.desync_interval, opts.sparse_saving ? ", sparse saving" : ""));
		Host::AddIconOSDMessage(OSD_KEY, ICON_FA_NETWORK_WIRED,
			(opts.remote == "any") ? fmt::format("Netplay: hosting on UDP port {}, waiting for the other player ...", opts.local_port) :
									 fmt::format("Netplay: connecting to {} ...", opts.remote),
			Host::OSD_INFO_DURATION);
	}
	return true;
}

void Netplay::Stop()
{
	// A session that has not started yet stays pending (the boot state load also calls this).
	if (!s_session)
		return;

	Console.WriteLn(fmt::format("Netplay: session ended after {} frames, {} rollbacks ({} frames resimulated, max {}), "
								"{} waits",
		s_stats.frames, s_stats.rollbacks, s_stats.resimulated, s_stats.max_rollback, s_stats.waits));
	ggrs_free(s_session);
	s_session = nullptr;
	Rollback::g_resimulating = false;
	Rollback::EndInputInjection();
	s_ring.clear();
	s_exit_pending = false;
}

void Netplay::LogStats()
{
	std::string net;
	if (!s_options.synctest)
	{
		const u32 remote = (s_options.local_player == 0) ? 1 : 0;
		GgrsNetworkStatsC ns = {};
		if (ggrs_network_stats(s_session, remote, &ns) == 0)
			net = fmt::format(", ping {} ms, frames ahead {}, remote behind {}", ns.ping_ms, ggrs_frames_ahead(s_session),
				ns.remote_frames_behind);
	}
	Console.WriteLn(fmt::format("NETPLAY STATS: frame {}, {} rollbacks, {} frames resimulated (max depth {}), {} waits, "
								"time sync {:.0f} ms{}",
		s_stats.real_frames, s_stats.rollbacks, s_stats.resimulated, s_stats.max_rollback, s_stats.waits,
		s_stats.timesync_ms, net));
}

void Netplay::Fail(std::string_view what)
{
	Console.Error(fmt::format("Netplay: {}", what));
	Host::AddIconOSDMessage(OSD_KEY, ICON_FA_TRIANGLE_EXCLAMATION, fmt::format("Netplay: {}", what),
		Host::OSD_ERROR_DURATION);
	Stop();
}

void Netplay::OnInputPolled()
{
	if (!IsActive())
		return;

	// Leave the CPU loop once the current event test has finished; requests are answered there.
	s_exit_pending = true;
	Cpu->ExitExecution();
}

void Netplay::OnExecutionExited()
{
	if (!std::exchange(s_exit_pending, false))
		return;

	if (s_pending.has_value())
	{
		const Options opts = std::move(*s_pending);
		s_pending.reset();
		Error error;
		if (!Start(opts, &error))
		{
			Fail(error.GetDescription());
			return;
		}
	}

	if (!s_session)
		return;

	// A frame just finished. Answer the rest of the current request list (saves, maybe the next advance).
	s_stats.frames++;
	if (!Rollback::g_resimulating)
	{
		s_stats.real_frames++;
		if ((s_stats.real_frames % 600) == 0)
			LogStats();
	}
	ProcessRequests();
}

bool Netplay::BeforeExecute()
{
	if (!s_session || !s_need_advance)
		return true;

	if (!AdvanceSession())
	{
		// Waiting: keep the network going, do not run the CPU.
		Threading::Sleep(1);
		return false;
	}

	return true;
}

bool Netplay::AdvanceSession()
{
	ggrs_poll(s_session);
	HandleEvents();
	if (!s_session)
		return true; // stopped by an event, run on without netplay

	if (s_wait_until != 0)
	{
		if (Common::Timer::GetCurrentValue() < s_wait_until)
			return false;
		s_wait_until = 0;
	}

	if (!ggrs_is_running(s_session))
		return false;

	if (!s_was_running)
	{
		s_was_running = true;
		if (!s_options.synctest)
		{
			char peer[64] = {};
			ggrs_peer_address(s_session, peer, sizeof(peer));
			Console.WriteLn(fmt::format("Netplay: connected to {}, running", peer));
			Host::AddIconOSDMessage(OSD_KEY, ICON_FA_NETWORK_WIRED, fmt::format("Netplay: connected to {}.", peer),
				Host::OSD_INFO_DURATION);
		}
	}

	// Time synchronization: the peer that runs ahead runs each frame slightly longer (1 ms per frame
	// ahead, at most 4 ms) until both are on the same timeline. Otherwise the leading peer keeps hitting
	// the prediction window and rolls back far more often than the connection requires (GGRS only
	// recommends a wait from 3 frames on). This moves the frame limiter's schedule: plain waits do not
	// work, because the limiter keeps an absolute schedule and catches up afterwards.
	if (!s_options.synctest)
	{
		const int frame = ggrs_current_frame(s_session);
		const int ahead = ggrs_frames_ahead(s_session);
		if (ahead > 0 && frame != s_timesync_frame)
		{
			s_timesync_frame = frame;
			const double ms = static_cast<double>(std::min(ahead, 4));
			VMManager::Internal::DelayFrameLimiter(ms);
			s_stats.timesync_ms += ms;
		}
	}

	// Local input of this frame. In a sync test both players are local (host ports 1 and 2).
	if (s_options.synctest)
	{
		for (u32 player = 0; player < NUM_PLAYERS; player++)
		{
			const Rollback::PadInput input = LocalInput(player);
			ggrs_add_local_input(s_session, player, reinterpret_cast<const u8*>(&input));
		}
	}
	else
	{
		const Rollback::PadInput input = LocalInput(LOCAL_CONTROLLER_PORT);
		if (ggrs_add_local_input(s_session, s_options.local_player, reinterpret_cast<const u8*>(&input)) < 0)
		{
			Fail(fmt::format("add_local_input failed: {}", LastGgrsError()));
			return true;
		}
	}

	const int n = ggrs_advance_frame(s_session);
	if (n == GGRS_ERR_PREDICTION_THRESHOLD || n == GGRS_ERR_NOT_SYNCHRONIZED)
	{
		s_stats.waits++;
		return false;
	}
	if (n == GGRS_ERR_MISMATCHED_CHECKSUM)
	{
		// Sync test: a resimulated frame produced a different checksum.
		Fail(fmt::format("SYNC TEST FAILED: {}", LastGgrsError()));
		return true;
	}
	if (n < 0)
	{
		Fail(fmt::format("advance_frame failed: {}", LastGgrsError()));
		return true;
	}

	s_req_count = n;
	s_req_index = 0;
	s_need_advance = false;
	s_rollback_depth = 0;

	// GGRS 0.13 does not report the prediction threshold as an error: it returns the requests without an
	// advance (for example only a save). Answer them, but do not run a frame.
	bool has_advance = false;
	for (int i = 0; i < n && !has_advance; i++)
		has_advance = (ggrs_request_kind(s_session, static_cast<u32>(i)) == GGRS_REQUEST_ADVANCE);

	ProcessRequests();
	if (!has_advance)
	{
		s_stats.waits++;
		return false;
	}
	return true;
}

void Netplay::ProcessRequests()
{
	while (s_req_index < s_req_count)
	{
		const u32 index = static_cast<u32>(s_req_index);
		const int kind = ggrs_request_kind(s_session, index);
		Error error;

		if (kind == GGRS_REQUEST_SAVE)
		{
			const int frame = ggrs_request_frame(s_session, index);
			const u32 slot = static_cast<u32>(frame) % static_cast<u32>(s_ring.size());
			if (!Rollback::Save(*s_ring[slot], &error))
			{
				Fail(fmt::format("snapshot failed: {}", error.GetDescription()));
				return;
			}
			// Checksums cost about 2 ms, so only compute them where GGRS can use them.
			const bool want_checksum = s_options.synctest || s_options.sparse_saving ||
									   (s_options.desync_interval > 0 && (frame % s_options.desync_interval) == 0);
			const auto [lo, hi] = want_checksum ? Checksum(*s_ring[slot]) : std::pair<u64, u64>(0, 0);
			ggrs_request_save(s_session, index, slot, want_checksum, lo, hi);
			if (s_options.dump && (frame <= 12 || frame == 60))
				DumpSnapshot(*s_ring[slot], frame);
		}
		else if (kind == GGRS_REQUEST_LOAD)
		{
			const s64 slot = ggrs_request_load(s_session, index);
			if (slot < 0 || static_cast<size_t>(slot) >= s_ring.size() ||
				!Rollback::Load(*s_ring[static_cast<size_t>(slot)], &error))
			{
				Fail(fmt::format("restore failed (slot {}): {}", slot, error.GetDescription()));
				return;
			}
			s_stats.rollbacks++;
			s_rollback_depth = 0;
		}
		else if (kind == GGRS_REQUEST_ADVANCE)
		{
			std::array<Rollback::PadInput, NUM_PLAYERS> inputs;
			ggrs_request_inputs(s_session, index, reinterpret_cast<u8*>(inputs.data()), nullptr);
			for (u32 port = 0; port < NUM_PLAYERS; port++)
				Rollback::SetInput(port, inputs[port]);

			// Further advances in this list follow: this frame is resimulated.
			bool more = false;
			for (int i = s_req_index + 1; i < s_req_count && !more; i++)
				more = (ggrs_request_kind(s_session, static_cast<u32>(i)) == GGRS_REQUEST_ADVANCE);
			Rollback::g_resimulating = more;
			if (more)
			{
				s_stats.resimulated++;
				s_rollback_depth++;
				s_stats.max_rollback = std::max(s_stats.max_rollback, s_rollback_depth);
			}

			s_req_index++;
			return; // let the CPU emulate this frame
		}
		else
		{
			Fail(fmt::format("unexpected request {} at {}", kind, index));
			return;
		}

		s_req_index++;
	}

	Rollback::g_resimulating = false;
	s_need_advance = true;
}

void Netplay::HandleEvents()
{
	GgrsEventC e;
	while (s_session && ggrs_next_event(s_session, &e))
	{
		switch (e.kind)
		{
			case GGRS_EVENT_SYNCHRONIZING:
				Host::AddIconOSDMessage(OSD_KEY, ICON_FA_NETWORK_WIRED,
					fmt::format("Netplay: synchronizing {}/{}", e.a, e.b), Host::OSD_QUICK_DURATION);
				break;
			case GGRS_EVENT_SYNCHRONIZED:
				Console.WriteLn("Netplay: synchronized with the remote peer");
				break;
			case GGRS_EVENT_NETWORK_INTERRUPTED:
				Host::AddIconOSDMessage(OSD_KEY, ICON_FA_TRIANGLE_EXCLAMATION,
					fmt::format("Netplay: connection interrupted, disconnecting in {} ms", e.a), Host::OSD_WARNING_DURATION);
				break;
			case GGRS_EVENT_NETWORK_RESUMED:
				Host::AddIconOSDMessage(OSD_KEY, ICON_FA_NETWORK_WIRED, "Netplay: connection resumed.",
					Host::OSD_INFO_DURATION);
				break;
			case GGRS_EVENT_WAIT_RECOMMENDATION:
				// This peer runs far ahead: delay the schedule by the recommended frames.
				VMManager::Internal::DelayFrameLimiter(FRAME_MS * static_cast<double>(e.a));
				s_stats.timesync_ms += FRAME_MS * static_cast<double>(e.a);
				break;
			case GGRS_EVENT_DESYNC_DETECTED:
				Console.Error(fmt::format("Netplay: DESYNC at frame {} (local {:016x}{:016x}, remote {:016x}{:016x})", e.a,
					e.local_checksum_hi, e.local_checksum_lo, e.remote_checksum_hi, e.remote_checksum_lo));
				Host::AddIconOSDMessage(OSD_KEY, ICON_FA_TRIANGLE_EXCLAMATION,
					fmt::format("Netplay: desync detected at frame {}", e.a), Host::OSD_ERROR_DURATION);
				break;
			case GGRS_EVENT_DISCONNECTED:
				Fail("the remote peer disconnected");
				return;
			default:
				break;
		}
	}
}

void Netplay::DumpSnapshot(const Rollback::Snapshot& snapshot, int frame)
{
	const std::string dir = Path::Combine(EmuFolders::Logs, "netplay");
	FileSystem::EnsureDirectoryExists(dir.c_str(), true);
	for (size_t i = 0; i < snapshot.state.GetLength(); i++)
	{
		const ArchiveEntry& entry = snapshot.state[i];
		const std::string path = Path::Combine(dir, fmt::format("p{}_f{}_{}", s_options.local_player + 1, frame,
			entry.GetFilename()));
		FileSystem::WriteBinaryFile(path.c_str(), snapshot.state.GetPtr(entry.GetDataIndex()), entry.GetDataSize());
	}
	const std::string inputs = Path::Combine(dir, fmt::format("p{}_f{}_inputs.bin", s_options.local_player + 1, frame));
	FileSystem::WriteBinaryFile(inputs.c_str(), snapshot.inputs.data(), sizeof(snapshot.inputs));
	Console.WriteLn(fmt::format("Netplay: dumped frame {} to {}", frame, dir));
}

Rollback::PadInput Netplay::LocalInput(u32 port)
{
	if (!s_options.test_input)
		return Rollback::CaptureLocalInput(port);

	// Testing. Button bits (active low): 4 triangle, 5 circle, 6 cross, 7 square, 8 select, 11 start,
	// 12-15 d-pad.
	static constexpr u16 CIRCLE = 1u << 5;
	static constexpr u16 RANDOM_MASK = 0xf0b0u; // d-pad, triangle, circle, square (no cross = "back")

	Rollback::PadInput input;

	// First 10 seconds: tap circle (5 frames pressed, 25 released) to confirm the preselected characters
	// on the character select screen.
	const int frame = ggrs_current_frame(s_session);
	if (frame < 600)
	{
		input.buttons = ((frame % 30) < 5) ? static_cast<u16>(~CIRCLE) : 0xffff;
		return input;
	}

	// Then a new random combination of d-pad and face buttons every 6 to 21 frames, so that the remote
	// side mispredicts and rolls back. Start and Select are never pressed (they pause the game).
	static u32 s_hold = 0;
	static u16 s_buttons = 0xffff;
	static u32 s_rng = 0x9e3779b9u ^ (s_options.local_player * 0x85ebca6bu) ^ s_options.local_port;
	if (s_hold == 0)
	{
		s_rng = s_rng * 1664525u + 1013904223u;
		const u16 pressed = static_cast<u16>((s_rng >> 8) & RANDOM_MASK);
		s_buttons = static_cast<u16>(~pressed);
		s_hold = 6 + ((s_rng >> 24) & 15);
	}
	s_hold--;

	input.buttons = s_buttons;
	return input;
}

std::pair<u64, u64> Netplay::Checksum(const Rollback::Snapshot& snapshot)
{
	// Gameplay state only. The internal structures contain host pointers, and GS state differs between
	// renderers, so neither is comparable between two machines.
	static constexpr const char* parts[] = {
		"eeMemory.bin", "iopMemory.bin", "Scratchpad.bin", "vu0Memory.bin", "vu1Memory.bin"};

	XXH3_state_t state;
	XXH3_128bits_reset(&state);
	for (size_t i = 0; i < snapshot.state.GetLength(); i++)
	{
		const ArchiveEntry& entry = snapshot.state[i];
		for (const char* part : parts)
		{
			if (entry.GetFilename() == part)
			{
				XXH3_128bits_update(&state, snapshot.state.GetPtr(entry.GetDataIndex()), entry.GetDataSize());
				break;
			}
		}
	}
	const XXH128_hash_t hash = XXH3_128bits_digest(&state);
	return {hash.low64, hash.high64};
}
