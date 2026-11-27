// Minimal C API over GGRS (Rust) for the PCSX2 rollback fork. See src/lib.rs for details.
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Size of one player's input for one frame (Pad::FrameInput).
#define GGRS_INPUT_SIZE 18

#define GGRS_REQUEST_SAVE 0
#define GGRS_REQUEST_LOAD 1
#define GGRS_REQUEST_ADVANCE 2

#define GGRS_INPUT_CONFIRMED 0
#define GGRS_INPUT_PREDICTED 1
#define GGRS_INPUT_DISCONNECTED 2

#define GGRS_ERR_PREDICTION_THRESHOLD (-1)
#define GGRS_ERR_NOT_SYNCHRONIZED (-2)
#define GGRS_ERR_MISMATCHED_CHECKSUM (-3)
#define GGRS_ERR_OTHER (-4)

#define GGRS_EVENT_SYNCHRONIZING 0
#define GGRS_EVENT_SYNCHRONIZED 1
#define GGRS_EVENT_DISCONNECTED 2
#define GGRS_EVENT_NETWORK_INTERRUPTED 3
#define GGRS_EVENT_NETWORK_RESUMED 4
#define GGRS_EVENT_WAIT_RECOMMENDATION 5
#define GGRS_EVENT_DESYNC_DETECTED 6

typedef struct GgrsSession GgrsSession;

typedef struct GgrsP2PConfig
{
	uint32_t num_players;     ///< 2 for this game
	uint32_t local_player;    ///< handle of the local player, every other handle is remote
	uint16_t local_port;      ///< local UDP port
	const char* remote_addr;  ///< "ip:port", or "any" on the host side (peer address learned from its first packet)
	uint32_t input_delay;     ///< frames
	uint32_t max_prediction;  ///< rollback depth in frames
	uint32_t desync_interval; ///< frames between checksum exchanges, 0 = off
	bool sparse_saving;       ///< only save the last confirmed frame
	uint32_t fps;             ///< 0 = GGRS default
} GgrsP2PConfig;

typedef struct GgrsEventC
{
	int32_t kind;
	int64_t a; ///< Synchronizing: count. WaitRecommendation: frames. NetworkInterrupted: timeout ms. Desync: frame.
	int64_t b; ///< Synchronizing: total.
	uint64_t local_checksum_lo, local_checksum_hi;
	uint64_t remote_checksum_lo, remote_checksum_hi;
} GgrsEventC;

typedef struct GgrsNetworkStatsC
{
	uint64_t ping_ms;
	uint64_t send_queue_len;
	int32_t local_frames_behind;
	int32_t remote_frames_behind;
} GgrsNetworkStatsC;

uint32_t ggrs_capi_version(void);
size_t ggrs_last_error(char* buf, size_t buf_len);

GgrsSession* ggrs_p2p_start(const GgrsP2PConfig* cfg);
GgrsSession* ggrs_synctest_start(uint32_t num_players, uint32_t check_distance, uint32_t input_delay, uint32_t max_prediction);
void ggrs_free(GgrsSession* s);

/* Session handshake before GGRS starts: exchanges one payload (at most 512 bytes) with the peer on the
 * socket the session will use. remote_addr: "ip:port" (join) or "any" (host, learns the joiner's address).
 * ggrs_net_poll returns 0 while waiting, the peer's payload length when done (copied to out), < 0 on error.
 * ggrs_p2p_start_net then starts the session on that socket (cfg->local_port and remote_addr are ignored). */
typedef struct GgrsNet GgrsNet;
GgrsNet* ggrs_net_open(uint16_t local_port, const char* remote_addr);
int32_t ggrs_net_poll(GgrsNet* net, const uint8_t* local, size_t local_len, uint8_t* out, size_t out_len);
size_t ggrs_net_peer_address(GgrsNet* net, char* buf, size_t buf_len);
GgrsSession* ggrs_p2p_start_net(GgrsNet* net, const GgrsP2PConfig* cfg);
void ggrs_net_free(GgrsNet* net);

void ggrs_poll(GgrsSession* s);
bool ggrs_is_running(GgrsSession* s);
int32_t ggrs_add_local_input(GgrsSession* s, uint32_t player, const uint8_t* input);

/// Returns the number of requests to answer in order, or a negative GGRS_ERR_* code.
int32_t ggrs_advance_frame(GgrsSession* s);
int32_t ggrs_request_kind(GgrsSession* s, uint32_t index);
int32_t ggrs_request_frame(GgrsSession* s, uint32_t index);
int32_t ggrs_request_save(GgrsSession* s, uint32_t index, uint32_t slot, bool has_checksum, uint64_t checksum_lo, uint64_t checksum_hi);
int64_t ggrs_request_load(GgrsSession* s, uint32_t index);
int32_t ggrs_request_inputs(GgrsSession* s, uint32_t index, uint8_t* inputs_out, int32_t* status_out);

bool ggrs_next_event(GgrsSession* s, GgrsEventC* out);
int32_t ggrs_frames_ahead(GgrsSession* s);
int32_t ggrs_current_frame(GgrsSession* s);
int32_t ggrs_confirmed_frame(GgrsSession* s);
int32_t ggrs_network_stats(GgrsSession* s, uint32_t player, GgrsNetworkStatsC* out);
/// Remote peer address "ip:port" (learned on the host side). Returns the length, 0 while unknown.
size_t ggrs_peer_address(GgrsSession* s, char* buf, size_t buf_len);

/* Spectators (see src/spectate.rs). The host keeps the confirmed inputs of both players from frame 0 on
 * and serves them to up to four spectators on the session socket; a spectator plays them back from frame 0,
 * fast while it is behind, so it can join a running match. */
#define GGRS_SPEC_FRAME_BYTES (2 * GGRS_INPUT_SIZE)
#define GGRS_SPEC_CONNECTING 0
#define GGRS_SPEC_WAITING 1  ///< the host's match has not started yet
#define GGRS_SPEC_RUNNING 2
#define GGRS_SPEC_REJECTED 4 ///< see ggrs_spec_reason
#define GGRS_SPEC_FULL 5     ///< four spectators already watch
#define GGRS_SPEC_TIMEOUT 6  ///< nothing heard from the host for 15 s

/* Host side, on a P2P session. */
void ggrs_spec_host_version(GgrsSession* s, const uint8_t* version, size_t len);
/// Confirmed inputs of `frame` (GGRS_SPEC_FRAME_BYTES), in frame order. Returns 0 or GGRS_ERR_OTHER.
int32_t ggrs_spec_host_push(GgrsSession* s, uint32_t frame, const uint8_t* inputs);
/// Checksum of the state at the start of `frame`, for the spectators' sync check.
void ggrs_spec_host_checksum(GgrsSession* s, uint32_t frame, uint64_t lo, uint64_t hi);
uint32_t ggrs_spec_host_count(GgrsSession* s);
void ggrs_spec_host_end(GgrsSession* s);

/* Spectator side. */
typedef struct GgrsSpec GgrsSpec;
typedef struct GgrsSpecStatusC
{
	int32_t state;        ///< GGRS_SPEC_*
	uint32_t host_frames; ///< frames the host has confirmed
	uint32_t buffered;    ///< frames here from the next one to take on
	bool ended;           ///< the host's session has ended
} GgrsSpecStatusC;
GgrsSpec* ggrs_spec_open(const char* host_addr, const uint8_t* version, size_t version_len);
int32_t ggrs_spec_poll(GgrsSpec* spec, GgrsSpecStatusC* out);
bool ggrs_spec_take(GgrsSpec* spec, uint32_t frame, uint8_t* inputs_out, bool* has_checksum, uint64_t* checksum_lo, uint64_t* checksum_hi);
size_t ggrs_spec_reason(GgrsSpec* spec, char* buf, size_t buf_len);
void ggrs_spec_free(GgrsSpec* spec);

#ifdef __cplusplus
}
#endif
