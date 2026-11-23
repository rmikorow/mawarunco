// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Rollback/Netplay.h"
#include "Rollback/BootShortcut.h"
#include "Rollback/FucButtons.h"
#include "Rollback/FucNames.h"
#include "Rollback/Rollback.h"
#include "Rollback/RunAhead.h"

#include "BuildVersion.h"
#include "Config.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "Memory.h"
#include "R5900.h"
#include "ps2/BiosTools.h"
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

#include <algorithm>
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
		int desync_test = -1;    // testing: player 2 changes one byte of EE RAM in this frame (desync diagnostics)
	};

	static constexpr u32 NUM_PLAYERS = Rollback::NUM_PORTS;
	static constexpr u16 DEFAULT_PORT = 7000;
	static constexpr u32 LOCAL_CONTROLLER_PORT = 0; // host controller bound to port 1
	static constexpr const char* OSD_KEY = "Netplay";
	static constexpr double FRAME_MS = 1000.0 / 59.94;

	static std::optional<Options> s_pending; // set by SetLaunchOptions, started at the first exit
	static Options s_options;
	static GgrsSession* s_session = nullptr;
	// Room handshake before the GGRS session (direct IP): the host sends its room settings (input delay,
	// rollback depth) and both sides compare program versions. The session starts once it is done.
	static GgrsNet* s_net = nullptr;
	static constexpr u32 MAX_DELAY = 8;
	static constexpr u32 MAX_ROLLBACK = 8;
	static std::string RoomPayload();
	static bool ApplyRoomPayload(std::string_view remote, Error* error);
	static void PollRoomHandshake();
	static bool StartSession(Error* error);
	static void ShowWaiting();
	static std::vector<std::unique_ptr<Rollback::Snapshot>> s_ring; // not movable, so held by pointer

	static bool s_exit_pending = false;
	static bool s_need_advance = false; // all requests answered, the next frame needs ggrs_advance_frame()
	static int s_req_count = 0;
	static int s_req_index = 0;
	static bool s_was_running = false;
	static int s_advance_frame = -1; // frame the next advance emulates (from the save before it)
	// Session handshake carried in the input stream: in its first input (frame = its input delay) each peer
	// sends its in-game button layout and a BIOS id instead of controller input. Both peers decode the
	// inputs of frames 0..HANDSHAKE_FRAME identically (also when resimulating) and write both layouts into
	// the game at HANDSHAKE_FRAME: host layout -> P1, joiner layout -> P2.
	static constexpr int HANDSHAKE_FRAME = 5; // > maximum input delay
	static constexpr u8 HANDSHAKE_MAGIC0 = 0xF5;
	static constexpr u8 HANDSHAKE_MAGIC1 = 0xC0;
	static constexpr u8 HANDSHAKE_VERSION = 1;
	static constexpr u8 HANDSHAKE_NAME_MAGIC1 = 0xC1; // second input: the player name
	struct Handshake
	{
		FucButtons::Layout layout;
		u32 bios = 0;
	};
	static std::array<std::optional<Handshake>, NUM_PLAYERS> s_handshake;
	static std::array<std::optional<FucNames::Name>, NUM_PLAYERS> s_names;
	static bool s_handshake_sent = false;
	static bool s_bios_warning_shown = false;
	static Rollback::PadInput HandshakeInput();
	static std::optional<Handshake> DecodeHandshake(const Rollback::PadInput& input);
	static Rollback::PadInput NameInput();
	static std::optional<FucNames::Name> DecodeName(const Rollback::PadInput& input);
	static int s_first_desync_frame = -1; // first frame GGRS reported as desynced, -1 = none
	static u32 s_desync_reports = 0;      // further reports after the first
	static int s_timesync_frame = -1; // frame for which the time sync wait was applied
	static Common::Timer s_wait_timer; // since the session started (shown while waiting for the peer)
	static u32 s_wait_shown_seconds = ~0u;

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

	// Run-ahead during netplay (see RunAhead.h): after the real frame of a request list, snapshot, show
	// speculative frames with the latest local input and the predicted remote input, restore. Skipped
	// in frames with a rollback (the rollback has priority for the time budget).
	enum class RunAheadState
	{
		Off,
		Real, // the real frame of this list runs with run-ahead flags
		Spec, // speculative frames run
	};
	static RunAheadState s_ra_state = RunAheadState::Off;
	static u32 s_ra_index = 0;
	static bool s_list_had_load = false;
	static Rollback::PadInput s_last_local_input = {};
	// Run-ahead saves the state after the real frame straight into the ring slot of the next frame. GGRS
	// asks to save exactly that state at the start of the next request list, so that save is skipped.
	static size_t s_ra_slot = 0;
	static int s_ra_presaved_frame = -1;
	static bool StartRunAhead();
	static void ContinueRunAhead();

	static bool Start(const Options& opts, Error* error);
	static void LogStats();
	static void ProcessRequests();
	static void HandleEvents();
	static bool AdvanceSession();
	static std::pair<u64, u64> Checksum(const Rollback::Snapshot& snapshot, int frame);
	static void LogDesyncMap(int frame);

	// Desync diagnostics: per-block hashes of the gameplay memory of the last checksum frames. When GGRS
	// reports a desync for a frame, both peers log the hashes of that frame; comparing the two logs shows
	// which memory region ran apart first.
	struct Fingerprint
	{
		int frame = -1;
		std::vector<std::pair<std::string, std::vector<u32>>> parts; // gameplay parts, one hash per block
		std::vector<std::pair<std::string, u32>> others;            // other savestate parts, one hash each
	};
	static constexpr size_t FINGERPRINT_BLOCK = 64 * 1024;
	static std::array<Fingerprint, 16> s_fingerprints;
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
		else if (key == "boot")
		{
			// Boot the game with the shortcut instead of loading a shared savestate ("boot" = built-in script).
			if (!BootShortcut::Request(value.empty() ? std::string_view("vs2p") : value, error))
				return false;
		}
		else if (key == "buttons")
		{
			// The local player's in-game button layout (FucButtons text form); the launcher sets it directly.
			const std::optional<FucButtons::Layout> layout = FucButtons::FromString(value);
			if (!layout)
			{
				Error::SetString(error, fmt::format("Netplay: invalid button layout '{}'", value));
				return false;
			}
			FucButtons::SetLocal(*layout);
		}
		else if (key == "name")
		{
			// The local player's name for the fight HUD; the launcher sets it directly.
			FucNames::SetLocal(value);
		}
		else if (key == "desynctest")
		{
			const std::optional<u32> v = number(1000000);
			if (!v)
				return false;
			opts.desync_test = static_cast<int>(*v);
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
	return s_session || s_net || s_pending.has_value();
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
		// GGRS starts after the room handshake (PollRoomHandshake).
		s_net = ggrs_net_open(opts.local_port, opts.remote.c_str());
	}

	if (!s_session && !s_net)
	{
		Rollback::EndInputInjection();
		Error::SetString(error, fmt::format("GGRS: {}", LastGgrsError()));
		return false;
	}

	s_options = opts;
	if (opts.synctest)
	{
		// One slot per frame that can be rolled back to, plus the current frame and a margin.
		const u32 depth = std::max(opts.max_prediction, opts.check_distance + 1);
		s_ring.clear();
		for (u32 i = 0; i < depth + 3; i++)
			s_ring.push_back(std::make_unique<Rollback::Snapshot>());
	}
	s_need_advance = true;
	s_req_count = s_req_index = 0;
	s_was_running = false;
	s_timesync_frame = -1;
	s_wait_timer.Reset();
	s_wait_shown_seconds = ~0u;
	s_stats = {};
	s_first_desync_frame = -1;
	s_desync_reports = 0;
	s_handshake = {};
	s_names = {};
	s_handshake_sent = false;
	s_bios_warning_shown = false;

	if (opts.synctest)
	{
		Console.WriteLn(fmt::format("Netplay: sync test started (check distance {}, delay {})", opts.check_distance,
			opts.input_delay));
	}
	else
	{
		Console.WriteLn(fmt::format("Netplay: waiting for the room handshake, local player {} on UDP port {}, remote {}",
			opts.local_player + 1, opts.local_port, opts.remote));
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
	if (s_net)
	{
		ggrs_net_free(s_net);
		s_net = nullptr;
		if (!s_session)
		{
			Rollback::EndInputInjection();
			Console.WriteLn("Netplay: stopped before the session started");
			return;
		}
	}
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
	s_ra_state = RunAheadState::Off;
	s_ra_presaved_frame = -1;
	RunAhead::ClearFlags();
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

std::string Netplay::RoomPayload()
{
	// Text, so it can grow (lobby, more settings) without breaking older peers' parsing.
	return fmt::format("FUC1;ver={};delay={};rollback={}", BuildVersion::GitHash, s_options.input_delay,
		s_options.max_prediction);
}

bool Netplay::ApplyRoomPayload(std::string_view remote, Error* error)
{
	std::string version;
	std::optional<u32> delay, rollback;
	for (const std::string_view part : StringUtil::SplitString(remote, ';', true))
	{
		const size_t eq = part.find('=');
		if (eq == std::string_view::npos)
			continue;
		const std::string_view key = part.substr(0, eq);
		const std::string_view value = part.substr(eq + 1);
		if (key == "ver")
			version = value;
		else if (key == "delay")
			delay = StringUtil::FromChars<u32>(value);
		else if (key == "rollback")
			rollback = StringUtil::FromChars<u32>(value);
	}
	if (!remote.starts_with("FUC1;"))
	{
		Error::SetString(error, "the other side is not a compatible netplay build");
		return false;
	}
	if (version != BuildVersion::GitHash)
	{
		Error::SetString(error, fmt::format("the other player uses another program version ({} / yours {}). Both need the "
											"same version.", version.substr(0, 9), std::string_view(BuildVersion::GitHash).substr(0, 9)));
		return false;
	}
	// The host decides the room settings.
	if (s_options.local_player != 0)
	{
		if (!delay || !rollback || *delay > MAX_DELAY || *rollback < 1 || *rollback > MAX_ROLLBACK)
		{
			Error::SetString(error, "invalid room settings from the host");
			return false;
		}
		s_options.input_delay = *delay;
		s_options.max_prediction = *rollback;
	}
	return true;
}

void Netplay::ShowWaiting()
{
	// The game does not run until both peers are connected; keep saying so, otherwise the frozen screen
	// looks like a hang.
	const u32 seconds = static_cast<u32>(s_wait_timer.GetTimeSeconds());
	if (seconds != s_wait_shown_seconds)
	{
		s_wait_shown_seconds = seconds;
		Host::AddIconOSDMessage(OSD_KEY, ICON_FA_NETWORK_WIRED,
			(s_options.remote == "any") ?
				fmt::format("Netplay: waiting for player 2 (UDP port {}) ... {} s", s_options.local_port, seconds) :
				fmt::format("Netplay: connecting to {} ... {} s", s_options.remote, seconds),
			2.0f);
	}
}

void Netplay::PollRoomHandshake()
{
	const std::string local = RoomPayload();
	std::array<u8, 512> remote;
	const s32 n = ggrs_net_poll(s_net, reinterpret_cast<const u8*>(local.data()), local.size(), remote.data(), remote.size());
	if (n < 0)
	{
		Fail(fmt::format("connection failed: {}", LastGgrsError()));
		return;
	}
	if (n == 0)
	{
		ShowWaiting();
		return;
	}

	Error error;
	const std::string_view remote_text(reinterpret_cast<const char*>(remote.data()), std::min<size_t>(n, remote.size()));
	if (!ApplyRoomPayload(remote_text, &error) || !StartSession(&error))
	{
		Fail(error.GetDescription());
		return;
	}
}

bool Netplay::StartSession(Error* error)
{
	GgrsP2PConfig cfg = {};
	cfg.num_players = NUM_PLAYERS;
	cfg.local_player = s_options.local_player;
	cfg.local_port = s_options.local_port;
	cfg.remote_addr = s_options.remote.c_str();
	cfg.input_delay = s_options.input_delay;
	cfg.max_prediction = s_options.max_prediction;
	cfg.desync_interval = s_options.desync_interval;
	cfg.sparse_saving = s_options.sparse_saving;
	cfg.fps = 60;
	s_session = ggrs_p2p_start_net(s_net, &cfg);
	char peer[64] = {};
	ggrs_net_peer_address(s_net, peer, sizeof(peer));
	ggrs_net_free(s_net);
	s_net = nullptr;
	if (!s_session)
	{
		Error::SetString(error, fmt::format("GGRS: {}", LastGgrsError()));
		return false;
	}

	// One slot per frame that can be rolled back to, plus the current frame and a margin.
	s_ring.clear();
	for (u32 i = 0; i < s_options.max_prediction + 3; i++)
		s_ring.push_back(std::make_unique<Rollback::Snapshot>());
	Console.WriteLn(fmt::format("Netplay: session started, local player {}, peer {}, delay {}, rollback {}{}, "
								"desync check every {} frames{}",
		s_options.local_player + 1, peer, s_options.input_delay, s_options.max_prediction,
		(s_options.local_player != 0) ? " (room settings from the host)" : "", s_options.desync_interval,
		s_options.sparse_saving ? ", sparse saving" : ""));
	return true;
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

	// A speculative run-ahead frame finished: next one, or restore and end the cycle.
	if (s_ra_state == RunAheadState::Spec)
	{
		ContinueRunAhead();
		return;
	}

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
	if (s_net && !s_session)
	{
		// Room handshake: the CPU does not run yet.
		PollRoomHandshake();
		Threading::Sleep(1);
		return false;
	}
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

	if (!ggrs_is_running(s_session))
	{
		ShowWaiting();
		return false;
	}

	if (!s_was_running)
	{
		s_was_running = true;
		if (!s_options.synctest)
		{
			char peer[64] = {};
			ggrs_peer_address(s_session, peer, sizeof(peer));
			// The host decides delay and rollback (ApplyRoomPayload); tell both players what the room uses,
			// the joiner may have picked other values in the launcher.
			const std::string room = fmt::format("delay {}, rollback {}{}", s_options.input_delay,
				s_options.max_prediction, (s_options.local_player != 0) ? " (set by the host)" : "");
			Console.WriteLn(fmt::format("Netplay: connected to {}, running, {}", peer, room));
			Host::AddIconOSDMessage(OSD_KEY, ICON_FA_NETWORK_WIRED,
				fmt::format("Netplay: connected to {}.\nRoom: {}.", peer, room), 10.0f);
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
		const Rollback::PadInput real_input = LocalInput(LOCAL_CONTROLLER_PORT);
		s_last_local_input = real_input;
		// The first input of the session carries the handshake (repeated until GGRS has taken it).
		const int input_frame = ggrs_current_frame(s_session);
		const Rollback::PadInput input = (input_frame == 0) ? HandshakeInput() : (input_frame == 1) ? NameInput() : real_input;
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
	s_list_had_load = false;

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
			s_advance_frame = frame;
			const u32 slot = static_cast<u32>(frame) % static_cast<u32>(s_ring.size());
			const bool presaved = (frame == s_ra_presaved_frame && !s_list_had_load);
			s_ra_presaved_frame = -1;
			if (!presaved && !Rollback::Save(*s_ring[slot], &error))
			{
				Fail(fmt::format("snapshot failed: {}", error.GetDescription()));
				return;
			}
			// Checksums cost about 2 ms, so only compute them where GGRS can use them.
			const bool want_checksum = s_options.synctest || s_options.sparse_saving ||
									   (s_options.desync_interval > 0 && (frame % s_options.desync_interval) == 0);
			const auto [lo, hi] = want_checksum ? Checksum(*s_ring[slot], frame) : std::pair<u64, u64>(0, 0);
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
			s_list_had_load = true;
		}
		else if (kind == GGRS_REQUEST_ADVANCE)
		{
			std::array<Rollback::PadInput, NUM_PLAYERS> inputs;
			ggrs_request_inputs(s_session, index, reinterpret_cast<u8*>(inputs.data()), nullptr);
			// Session handshake (see s_handshake): decode, keep it away from the game, apply both layouts.
			if (!s_options.synctest && s_advance_frame >= 0 && s_advance_frame <= HANDSHAKE_FRAME)
			{
				for (u32 port = 0; port < NUM_PLAYERS; port++)
				{
					if (std::optional<Handshake> hs = DecodeHandshake(inputs[port]))
					{
						s_handshake[port] = hs;
						inputs[port] = Rollback::PadInput{};
					}
					else if (std::optional<FucNames::Name> name = DecodeName(inputs[port]))
					{
						s_names[port] = name;
						inputs[port] = Rollback::PadInput{};
					}
				}
				if (s_advance_frame == HANDSHAKE_FRAME)
				{
					for (u32 port = 0; port < NUM_PLAYERS; port++)
						FucButtons::Write(port, s_handshake[port] ? s_handshake[port]->layout : FucButtons::Default());
					FucNames::Apply(s_names[0].value_or(FucNames::Name{}), s_names[1].value_or(FucNames::Name{}));
					// Once both are known (possibly only in a resimulated frame): log, and warn about different BIOS
					// versions, the usual reason why two boots differ.
					if (s_handshake[0] && s_handshake[1] && !s_bios_warning_shown)
					{
						s_bios_warning_shown = true;
						auto name_text = [](const std::optional<FucNames::Name>& n) {
							return n ? std::string(n->data(), strnlen(n->data(), n->size())) : std::string();
						};
						Console.WriteLn(fmt::format("Netplay: button layouts P1 {} P2 {}, BIOS {:08x} / {:08x}, names '{}' / '{}'",
							FucButtons::ToString(s_handshake[0]->layout), FucButtons::ToString(s_handshake[1]->layout),
							s_handshake[0]->bios, s_handshake[1]->bios, name_text(s_names[0]), name_text(s_names[1])));
						if (s_handshake[0]->bios != s_handshake[1]->bios)
						{
							Console.Error("Netplay: the players use different BIOS versions");
							Host::AddIconOSDMessage("NetplayBios", ICON_FA_TRIANGLE_EXCLAMATION,
								"Netplay: the players use different BIOS versions. The game may desync.", 30.0f);
						}
					}
				}
			}
			for (u32 port = 0; port < NUM_PLAYERS; port++)
				Rollback::SetInput(port, inputs[port]);

			// Testing the desync diagnostics: a deliberate difference on one peer, also in resimulated frames.
			if (s_options.desync_test >= 0 && s_advance_frame == s_options.desync_test && s_options.local_player == 1)
			{
				eeMem->Main[0x1000000] ^= 0x01;
				Console.Warning(fmt::format("Netplay: desync test, changed EE RAM 0x1000000 in frame {}", s_advance_frame));
			}

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
			else if (!s_options.synctest && RunAhead::Frames() > 0 && !s_list_had_load && s_advance_frame > HANDSHAKE_FRAME)
			{
				// The real frame of this list, followed by run-ahead.
				RunAhead::BeginRealFrame();
				s_ra_state = RunAheadState::Real;
			}
			else
			{
				RunAhead::ClearFlags();
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
	if (s_ra_state == RunAheadState::Real && StartRunAhead())
		return; // the CPU emulates the first speculative frame
	s_need_advance = true;
}

bool Netplay::StartRunAhead()
{
	// After the advance, GGRS is at the next frame; its first request will be to save this state.
	const int next_frame = ggrs_current_frame(s_session);
	s_ra_slot = static_cast<size_t>(next_frame) % s_ring.size();
	Error error;
	if (!Rollback::Save(*s_ring[s_ra_slot], &error))
	{
		Console.Error(fmt::format("Netplay: run-ahead snapshot failed: {}", error.GetDescription()));
		s_ra_state = RunAheadState::Off;
		s_ra_presaved_frame = -1;
		RunAhead::ClearFlags();
		return false;
	}
	s_ra_presaved_frame = next_frame;
	// Latest local input; the remote input stays at its last value (GGRS predicts "repeat last" too).
	Rollback::SetInput(s_options.local_player, s_last_local_input);
	s_ra_index = 1;
	RunAhead::BeginSpeculativeFrame(s_ra_index);
	s_ra_state = RunAheadState::Spec;
	return true;
}

void Netplay::ContinueRunAhead()
{
	if (s_ra_index < RunAhead::Frames())
	{
		RunAhead::BeginSpeculativeFrame(++s_ra_index);
		return;
	}
	Error error;
	if (!Rollback::Load(*s_ring[s_ra_slot], &error))
	{
		Fail(fmt::format("run-ahead restore failed: {}", error.GetDescription()));
		return;
	}
	s_ra_state = RunAheadState::Off;
	RunAhead::EndCycle(); // limiter sleep and host input poll, then the next advance
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
				// The first one tells where it started; after that every check reports again, so later
				// ones only go to the log (the diagnostics map only for the first few).
				if (s_first_desync_frame < 0)
				{
					s_first_desync_frame = static_cast<int>(e.a);
					LogDesyncMap(s_first_desync_frame);
					// At the first comparison the peers never ran apart: they started from different states.
					// With the boot shortcut that means a different BIOS, disc image or settings.
					const bool from_start = (e.a <= static_cast<s64>(s_options.desync_interval));
					const std::string text =
						from_start ? std::string("Netplay: the game differs between the players from the start. Both "
												 "need the same BIOS version, game image and settings.") :
									 fmt::format("Netplay: desync detected at frame {}", e.a);
					Console.Error(fmt::format("Netplay: FIRST DESYNC at frame {}{}", e.a, from_start ? " (from the start)" : ""));
					// Stays on screen for the rest of the session.
					Host::AddIconOSDMessage(OSD_KEY, ICON_FA_TRIANGLE_EXCLAMATION, text, 24.0f * 3600.0f);
				}
				else if (++s_desync_reports <= 3)
				{
					LogDesyncMap(static_cast<int>(e.a));
				}
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

Rollback::PadInput Netplay::HandshakeInput()
{
	std::array<u8, sizeof(Rollback::PadInput)> b = {};
	b[0] = HANDSHAKE_MAGIC0;
	b[1] = HANDSHAKE_MAGIC1;
	b[2] = HANDSHAKE_VERSION;
	const FucButtons::Layout& layout = FucButtons::GetLocal();
	std::memcpy(&b[3], layout.data(), layout.size());
	std::memcpy(&b[3 + layout.size()], &BiosChecksum, sizeof(BiosChecksum));
	Rollback::PadInput input;
	std::memcpy(&input, b.data(), sizeof(input));
	return input;
}

std::optional<Netplay::Handshake> Netplay::DecodeHandshake(const Rollback::PadInput& input)
{
	std::array<u8, sizeof(Rollback::PadInput)> b;
	std::memcpy(b.data(), &input, sizeof(input));
	if (b[0] != HANDSHAKE_MAGIC0 || b[1] != HANDSHAKE_MAGIC1 || b[2] != HANDSHAKE_VERSION)
		return std::nullopt;
	Handshake hs;
	std::memcpy(hs.layout.data(), &b[3], hs.layout.size());
	for (u8& a : hs.layout)
		a = std::min<u8>(a, FucButtons::NUM_ACTIONS - 1);
	std::memcpy(&hs.bios, &b[3 + hs.layout.size()], sizeof(hs.bios));
	return hs;
}

Rollback::PadInput Netplay::NameInput()
{
	std::array<u8, sizeof(Rollback::PadInput)> b = {};
	b[0] = HANDSHAKE_MAGIC0;
	b[1] = HANDSHAKE_NAME_MAGIC1;
	b[2] = HANDSHAKE_VERSION;
	const FucNames::Packed packed = FucNames::Pack(FucNames::GetLocal());
	static_assert(3 + std::tuple_size_v<FucNames::Packed> <= sizeof(Rollback::PadInput));
	std::memcpy(&b[3], packed.data(), packed.size());
	Rollback::PadInput input;
	std::memcpy(&input, b.data(), sizeof(input));
	return input;
}

std::optional<FucNames::Name> Netplay::DecodeName(const Rollback::PadInput& input)
{
	std::array<u8, sizeof(Rollback::PadInput)> b;
	std::memcpy(b.data(), &input, sizeof(input));
	if (b[0] != HANDSHAKE_MAGIC0 || b[1] != HANDSHAKE_NAME_MAGIC1 || b[2] != HANDSHAKE_VERSION)
		return std::nullopt;
	// Unpacking only yields characters the game's font has.
	FucNames::Packed packed;
	std::memcpy(packed.data(), &b[3], packed.size());
	return FucNames::Unpack(packed);
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

std::pair<u64, u64> Netplay::Checksum(const Rollback::Snapshot& snapshot, int frame)
{
	// Gameplay state only. The internal structures contain host pointers, and GS state differs between
	// renderers, so neither is comparable between two machines. The checksum is a hash over per-block
	// hashes, which are kept for the desync diagnostics (same cost as hashing the parts directly).
	static constexpr const char* parts[] = {
		"eeMemory.bin", "iopMemory.bin", "Scratchpad.bin", "vu0Memory.bin", "vu1Memory.bin"};

	Fingerprint& fp = s_fingerprints[static_cast<size_t>(frame / std::max<u32>(s_options.desync_interval, 1)) %
									 s_fingerprints.size()];
	fp.frame = frame;
	fp.parts.clear();
	fp.others.clear();

	XXH3_state_t state;
	XXH3_128bits_reset(&state);
	for (const char* part : parts)
	{
		for (size_t i = 0; i < snapshot.state.GetLength(); i++)
		{
			const ArchiveEntry& entry = snapshot.state[i];
			if (entry.GetFilename() != part)
				continue;
			const u8* data = snapshot.state.GetPtr(entry.GetDataIndex());
			const size_t size = entry.GetDataSize();
			std::vector<u32> blocks;
			blocks.reserve((size + FINGERPRINT_BLOCK - 1) / FINGERPRINT_BLOCK);
			for (size_t off = 0; off < size; off += FINGERPRINT_BLOCK)
			{
				const XXH64_hash_t h = XXH3_64bits(data + off, std::min(FINGERPRINT_BLOCK, size - off));
				XXH3_128bits_update(&state, &h, sizeof(h));
				blocks.push_back(static_cast<u32>(h));
			}
			fp.parts.emplace_back(part, std::move(blocks));
			break;
		}
	}

	// The rest only for the diagnostics (not compared by GGRS).
	for (size_t i = 0; i < snapshot.state.GetLength(); i++)
	{
		const ArchiveEntry& entry = snapshot.state[i];
		const std::string& name = entry.GetFilename();
		if (name == "PCSX2 Internal Structures.dat" ||
			std::any_of(std::begin(parts), std::end(parts), [&name](const char* p) { return name == p; }))
			continue;
		fp.others.emplace_back(name,
			static_cast<u32>(XXH3_64bits(snapshot.state.GetPtr(entry.GetDataIndex()), entry.GetDataSize())));
	}

	const XXH128_hash_t hash = XXH3_128bits_digest(&state);
	return {hash.low64, hash.high64};
}

void Netplay::LogDesyncMap(int frame)
{
	const auto it = std::find_if(s_fingerprints.begin(), s_fingerprints.end(),
		[frame](const Fingerprint& fp) { return fp.frame == frame; });
	if (it == s_fingerprints.end())
	{
		Console.Error(fmt::format("DESYNC MAP frame {}: no fingerprint kept", frame));
		return;
	}
	// One line per part: block size and the low 32 bits of each block's hash. Compare with the other
	// peer's log (same frame) to find the first differing block: address = block index * block size.
	for (const auto& [name, blocks] : it->parts)
	{
		std::string line = fmt::format("DESYNC MAP frame {} {} block {:x}:", frame, name, FINGERPRINT_BLOCK);
		for (const u32 h : blocks)
			line += fmt::format(" {:08x}", h);
		Console.WriteLn(line);
	}
	std::string others = fmt::format("DESYNC MAP frame {} others:", frame);
	for (const auto& [name, h] : it->others)
		others += fmt::format(" {}={:08x}", name, h);
	Console.WriteLn(others);
}
