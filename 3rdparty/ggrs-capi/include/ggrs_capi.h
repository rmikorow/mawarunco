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
	const char* remote_addr;  ///< "ip:port"
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

#ifdef __cplusplus
}
#endif
