//! Minimal C API over GGRS for the PCSX2 rollback fork.
//!
//! Pull model, no callbacks: the caller advances the session, then walks the returned requests
//! (save, load, advance) in order and answers each one. The game state itself stays on the C++ side;
//! GGRS only stores a slot number per saved frame.
//!
//! All functions must be called from one thread. Errors are reported through return values plus
//! `ggrs_last_error()`.

use std::cell::{Cell, RefCell};
use std::collections::VecDeque;
use std::ffi::{CStr, c_char};
use std::io::ErrorKind;
use bincode::Options;
use std::net::{IpAddr, Ipv4Addr, SocketAddr, UdpSocket};
use std::rc::Rc;

use ggrs::{
    Config, DesyncDetection, GgrsError, GgrsEvent, GgrsRequest, InputStatus, Message, NonBlockingSocket, P2PSession,
    PlayerType, PredictRepeatLast, SessionBuilder, SessionState, SyncTestSession,
};
use serde::{Deserialize, Serialize};

/// Size of one player's input for one frame (Pad::FrameInput in the fork).
pub const GGRS_INPUT_SIZE: usize = 18;

/// One frame of DualShock 2 input: buttons (u16, active low), rx, ry, lx, ly, 12 pressures.
#[derive(Copy, Clone, PartialEq, Eq, Debug, Serialize, Deserialize)]
pub struct PadInput([u8; GGRS_INPUT_SIZE]);

impl Default for PadInput {
    /// "No input": no button pressed (active low), sticks centered, no pressure.
    fn default() -> Self {
        let mut b = [0u8; GGRS_INPUT_SIZE];
        b[0] = 0xff;
        b[1] = 0xff;
        b[2..6].fill(0x7f);
        PadInput(b)
    }
}

struct Cfg;
impl Config for Cfg {
    type Input = PadInput;
    type InputPredictor = PredictRepeatLast;
    /// Slot number of the snapshot on the C++ side.
    type State = u32;
    type Address = SocketAddr;
}

/// UDP socket for one remote peer.
///
/// Join side: the peer address is given. Host side ("any"): the address is learned from the first valid
/// GGRS packet, so the joining player does not need port forwarding (their router may rewrite the
/// source port). After that, packets from other addresses are ignored. GGRS itself always sees the
/// peer under one fixed placeholder address.
type SharedAddr = Rc<Cell<Option<SocketAddr>>>;

struct PeerSocket {
    socket: UdpSocket,
    buffer: Box<[u8; 4096]>,
    placeholder: SocketAddr,
    /// Actual peer address; shared with the session so the caller can query it.
    peer: SharedAddr,
}

fn bind_udp(port: u16) -> std::io::Result<UdpSocket> {
    let socket = UdpSocket::bind(SocketAddr::new(IpAddr::V4(Ipv4Addr::UNSPECIFIED), port))?;
    socket.set_nonblocking(true)?;
    Ok(socket)
}

impl PeerSocket {
    fn new(socket: UdpSocket, placeholder: SocketAddr, peer: SharedAddr) -> Self {
        Self { socket, buffer: Box::new([0; 4096]), placeholder, peer }
    }
}

impl NonBlockingSocket<SocketAddr> for PeerSocket {
    fn send_to(&mut self, msg: &Message, addr: &SocketAddr) {
        let (Some(peer), true) = (self.peer.get(), *addr == self.placeholder) else { return };
        if let Ok(buf) = bincode::serialize(msg) {
            let _ = self.socket.send_to(&buf, peer);
        }
    }

    fn receive_all_messages(&mut self) -> Vec<(SocketAddr, Message)> {
        let mut received = Vec::new();
        loop {
            match self.socket.recv_from(&mut self.buffer[..]) {
                Ok((n, src)) => {
                    // Late session handshake packets (see GgrsNet) are not GGRS messages.
                    if self.buffer[..n].starts_with(HS_MAGIC) {
                        continue;
                    }
                    // Packets come from the internet: decode with a size limit (same format as
                    // bincode::serialize: fixed-size integers, trailing bytes allowed), so a crafted length
                    // field cannot make the decoder allocate more than a packet can hold.
                    let Ok(msg) = bincode::DefaultOptions::new()
                        .with_fixint_encoding()
                        .allow_trailing_bytes()
                        .with_limit(self.buffer.len() as u64)
                        .deserialize::<Message>(&self.buffer[..n])
                    else {
                        continue;
                    };
                    if self.peer.get().is_none() {
                        self.peer.set(Some(src));
                    }
                    if self.peer.get() == Some(src) {
                        received.push((self.placeholder, msg));
                    }
                }
                Err(ref e) if e.kind() == ErrorKind::WouldBlock => return received,
                // Windows reports ICMP "port unreachable" from earlier sends this way; not fatal.
                Err(ref e) if e.kind() == ErrorKind::ConnectionReset => {}
                Err(_) => return received,
            }
        }
    }
}

enum Session {
    P2P(P2PSession<Cfg>),
    SyncTest(SyncTestSession<Cfg>),
}

/// Opaque session handle for C.
pub struct GgrsSession {
    session: Session,
    /// Remote peer address (learned on the host side), shared with the socket.
    peer_addr: Option<SharedAddr>,
    num_players: usize,
    requests: Vec<GgrsRequest<Cfg>>,
    events: VecDeque<GgrsEvent<Cfg>>,
}

thread_local! {
    static LAST_ERROR: RefCell<String> = const { RefCell::new(String::new()) };
}

fn set_error(msg: impl Into<String>) {
    LAST_ERROR.with(|e| *e.borrow_mut() = msg.into());
}

// ---------------------------------------------------------------------------------------------------
// C types (mirrored in ggrs_capi.h)
// ---------------------------------------------------------------------------------------------------

/// Parameters of a peer-to-peer session.
#[repr(C)]
pub struct GgrsP2PConfig {
    /// Number of players (2 for this game).
    pub num_players: u32,
    /// Handle of the local player (0 or 1); every other handle is the remote player.
    pub local_player: u32,
    /// Local UDP port to bind.
    pub local_port: u16,
    /// Remote address as "ip:port", or "any" on the host side: the peer address is then learned from
    /// its first packet, so the joining side needs no port forwarding (NUL terminated).
    pub remote_addr: *const c_char,
    /// Input delay in frames.
    pub input_delay: u32,
    /// Maximum number of frames predicted ahead of the last confirmed frame (rollback depth).
    pub max_prediction: u32,
    /// Desync detection interval in frames, 0 = off.
    pub desync_interval: u32,
    /// Only save the last confirmed frame instead of every frame.
    pub sparse_saving: bool,
    /// Frames per second of the game (used for time synchronization), 0 = GGRS default.
    pub fps: u32,
}

pub const GGRS_REQUEST_SAVE: i32 = 0;
pub const GGRS_REQUEST_LOAD: i32 = 1;
pub const GGRS_REQUEST_ADVANCE: i32 = 2;

pub const GGRS_INPUT_CONFIRMED: i32 = 0;
pub const GGRS_INPUT_PREDICTED: i32 = 1;
pub const GGRS_INPUT_DISCONNECTED: i32 = 2;

/// Error codes (negative return values).
pub const GGRS_ERR_PREDICTION_THRESHOLD: i32 = -1;
pub const GGRS_ERR_NOT_SYNCHRONIZED: i32 = -2;
pub const GGRS_ERR_MISMATCHED_CHECKSUM: i32 = -3;
pub const GGRS_ERR_OTHER: i32 = -4;

pub const GGRS_EVENT_SYNCHRONIZING: i32 = 0;
pub const GGRS_EVENT_SYNCHRONIZED: i32 = 1;
pub const GGRS_EVENT_DISCONNECTED: i32 = 2;
pub const GGRS_EVENT_NETWORK_INTERRUPTED: i32 = 3;
pub const GGRS_EVENT_NETWORK_RESUMED: i32 = 4;
pub const GGRS_EVENT_WAIT_RECOMMENDATION: i32 = 5;
pub const GGRS_EVENT_DESYNC_DETECTED: i32 = 6;

/// An event. Fields that do not apply to the kind are 0.
#[repr(C)]
#[derive(Default)]
pub struct GgrsEventC {
    pub kind: i32,
    /// Synchronizing: count in `a`, total in `b`. WaitRecommendation: skip_frames in `a`.
    /// NetworkInterrupted: timeout in ms in `a`. DesyncDetected: frame in `a`.
    pub a: i64,
    pub b: i64,
    pub local_checksum_lo: u64,
    pub local_checksum_hi: u64,
    pub remote_checksum_lo: u64,
    pub remote_checksum_hi: u64,
}

#[repr(C)]
#[derive(Default)]
pub struct GgrsNetworkStatsC {
    pub ping_ms: u64,
    pub send_queue_len: u64,
    pub local_frames_behind: i32,
    pub remote_frames_behind: i32,
}

// ---------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------

fn handle<'a>(s: *mut GgrsSession) -> Option<&'a mut GgrsSession> {
    // SAFETY: the caller passes a pointer returned by a ggrs_*_start function that is not freed yet.
    unsafe { s.as_mut() }
}

fn drain_events(h: &mut GgrsSession) {
    match &mut h.session {
        Session::P2P(s) => h.events.extend(s.events()),
        Session::SyncTest(_) => {}
    }
}

fn split_checksum(c: u128) -> (u64, u64) {
    (c as u64, (c >> 64) as u64)
}

fn error_code(e: GgrsError) -> i32 {
    let code = match &e {
        GgrsError::PredictionThreshold => GGRS_ERR_PREDICTION_THRESHOLD,
        GgrsError::NotSynchronized => GGRS_ERR_NOT_SYNCHRONIZED,
        GgrsError::MismatchedChecksum { .. } => GGRS_ERR_MISMATCHED_CHECKSUM,
        _ => GGRS_ERR_OTHER,
    };
    set_error(e.to_string());
    code
}

fn new_session(session: Session, num_players: usize) -> *mut GgrsSession {
    Box::into_raw(Box::new(GgrsSession {
        session,
        peer_addr: None,
        num_players,
        requests: Vec::new(),
        events: VecDeque::new(),
    }))
}

// ---------------------------------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------------------------------

/// Version of this API (bump on incompatible changes).
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_capi_version() -> u32 {
    1
}

/// Copies the last error message into `buf` (NUL terminated, truncated). Returns the full length.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_last_error(buf: *mut c_char, buf_len: usize) -> usize {
    LAST_ERROR.with(|e| {
        let e = e.borrow();
        let bytes = e.as_bytes();
        if !buf.is_null() && buf_len > 0 {
            let n = bytes.len().min(buf_len - 1);
            // SAFETY: the caller provides a buffer of buf_len bytes.
            unsafe {
                std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
                *buf.add(n) = 0;
            }
        }
        bytes.len()
    })
}

fn parse_remote(cfg: &GgrsP2PConfig) -> Result<Option<SocketAddr>, String> {
    if cfg.remote_addr.is_null() {
        return Err("null remote address".into());
    }
    // SAFETY: NUL terminated string from the caller.
    let addr_str = unsafe { CStr::from_ptr(cfg.remote_addr) }.to_string_lossy();
    // "any": host side, the peer address is learned from its first packet.
    if addr_str.trim().eq_ignore_ascii_case("any") {
        return Ok(None);
    }
    addr_str.trim().parse().map(Some).map_err(|e| format!("invalid remote address '{addr_str}': {e}"))
}

/// Builds the P2P session on a bound socket. Returns null on error.
fn start_p2p(cfg: &GgrsP2PConfig, socket: UdpSocket, peer: Option<SocketAddr>) -> *mut GgrsSession {
    // GGRS identifies the peer by this address; the socket maps it to the actual one.
    let remote = SocketAddr::new(IpAddr::V4(Ipv4Addr::new(0, 0, 0, 1)), 1);
    let peer_addr: SharedAddr = Rc::new(Cell::new(peer));

    let num_players = cfg.num_players as usize;
    let build = || -> Result<P2PSession<Cfg>, String> {
        let mut builder = SessionBuilder::<Cfg>::new()
            .with_num_players(num_players)
            .map_err(|e| e.to_string())?
            .with_input_delay(cfg.input_delay as usize)
            .with_max_prediction_window(cfg.max_prediction as usize)
            .with_sparse_saving_mode(cfg.sparse_saving)
            .with_desync_detection_mode(if cfg.desync_interval > 0 {
                DesyncDetection::On { interval: cfg.desync_interval }
            } else {
                DesyncDetection::Off
            });
        if cfg.fps > 0 {
            builder = builder.with_fps(cfg.fps as usize).map_err(|e| e.to_string())?;
        }
        for player in 0..num_players {
            let kind =
                if player == cfg.local_player as usize { PlayerType::Local } else { PlayerType::Remote(remote) };
            builder = builder.add_player(kind, player).map_err(|e| e.to_string())?;
        }
        builder.start_p2p_session(PeerSocket::new(socket, remote, peer_addr.clone())).map_err(|e| e.to_string())
    };

    match build() {
        Ok(session) => {
            let s = new_session(Session::P2P(session), num_players);
            // SAFETY: freshly allocated session.
            unsafe { (*s).peer_addr = Some(peer_addr) };
            s
        }
        Err(e) => {
            set_error(e);
            std::ptr::null_mut()
        }
    }
}

/// Starts a peer-to-peer session. Returns null on error (see ggrs_last_error).
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_p2p_start(cfg: *const GgrsP2PConfig) -> *mut GgrsSession {
    // SAFETY: the caller passes a valid config or null.
    let Some(cfg) = (unsafe { cfg.as_ref() }) else {
        set_error("null config");
        return std::ptr::null_mut();
    };
    let peer = match parse_remote(cfg) {
        Ok(p) => p,
        Err(e) => {
            set_error(e);
            return std::ptr::null_mut();
        }
    };
    match bind_udp(cfg.local_port) {
        Ok(socket) => start_p2p(cfg, socket, peer),
        Err(e) => {
            set_error(format!("cannot bind UDP port {}: {e}", cfg.local_port));
            std::ptr::null_mut()
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// Session handshake before GGRS starts (room settings, versions)
// ---------------------------------------------------------------------------------------------------

/// Marks handshake packets; they are never valid GGRS messages for the peer socket.
const HS_MAGIC: &[u8; 4] = b"FUCH";
const HS_HELLO: u8 = 1; // joiner -> host, payload: joiner info
const HS_WELCOME: u8 = 2; // host -> joiner, payload: host info (room settings)
const HS_READY: u8 = 3; // joiner -> host: got the settings, GGRS starts
const HS_MAX_PAYLOAD: usize = 512;

/// A UDP socket that exchanges one payload with the peer before the GGRS session starts on it.
///
/// The joining side sends HELLO with its payload until the host answers with WELCOME and its payload,
/// then confirms with READY and starts GGRS. The host learns the joiner's address from its first HELLO
/// (later packets from other addresses are ignored) and answers every HELLO; it is done on READY, or when
/// GGRS packets from the joiner arrive (the joiner has moved on, READY was lost).
pub struct GgrsNet {
    socket: Option<UdpSocket>,
    peer: Option<SocketAddr>,
    is_host: bool,
    remote_payload: Option<Vec<u8>>,
    done: bool,
    last_hello: Option<std::time::Instant>,
}

fn hs_packet(kind: u8, payload: &[u8]) -> Vec<u8> {
    let mut p = Vec::with_capacity(5 + payload.len());
    p.extend_from_slice(HS_MAGIC);
    p.push(kind);
    p.extend_from_slice(payload);
    p
}

/// Opens the socket for a session handshake. `remote_addr` is "ip:port" (join) or "any" (host).
/// Returns null on error.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_net_open(local_port: u16, remote_addr: *const c_char) -> *mut GgrsNet {
    if remote_addr.is_null() {
        set_error("null remote address");
        return std::ptr::null_mut();
    }
    // SAFETY: NUL terminated string from the caller.
    let addr_str = unsafe { CStr::from_ptr(remote_addr) }.to_string_lossy();
    let peer = if addr_str.trim().eq_ignore_ascii_case("any") {
        None
    } else {
        match addr_str.trim().parse::<SocketAddr>() {
            Ok(a) => Some(a),
            Err(e) => {
                set_error(format!("invalid remote address '{addr_str}': {e}"));
                return std::ptr::null_mut();
            }
        }
    };
    match bind_udp(local_port) {
        Ok(socket) => Box::into_raw(Box::new(GgrsNet {
            socket: Some(socket),
            peer,
            is_host: peer.is_none(),
            remote_payload: None,
            done: false,
            last_hello: None,
        })),
        Err(e) => {
            set_error(format!("cannot bind UDP port {local_port}: {e}"));
            std::ptr::null_mut()
        }
    }
}

/// Runs the handshake; call it regularly. `local` is this side's payload (at most 512 bytes). Returns 0
/// while waiting, the length of the peer's payload (copied to `out`, truncated to `out_len`) when done, or
/// a negative value on error.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_net_poll(
    net: *mut GgrsNet,
    local: *const u8,
    local_len: usize,
    out: *mut u8,
    out_len: usize,
) -> i32 {
    // SAFETY: pointer from ggrs_net_open, not freed yet.
    let Some(net) = (unsafe { net.as_mut() }) else { return GGRS_ERR_OTHER };
    if local.is_null() || local_len > HS_MAX_PAYLOAD {
        set_error("invalid handshake payload");
        return GGRS_ERR_OTHER;
    }
    // SAFETY: the caller provides local_len bytes.
    let local = unsafe { std::slice::from_raw_parts(local, local_len) };
    let Some(socket) = net.socket.as_ref() else {
        set_error("socket already handed to the session");
        return GGRS_ERR_OTHER;
    };

    let mut buf = [0u8; 4096];
    if !net.done {
        loop {
            let (n, src) = match socket.recv_from(&mut buf) {
                Ok(r) => r,
                Err(ref e) if e.kind() == ErrorKind::WouldBlock => break,
                Err(ref e) if e.kind() == ErrorKind::ConnectionReset => continue,
                Err(e) => {
                    set_error(format!("network error: {e}"));
                    return GGRS_ERR_OTHER;
                }
            };
            let packet = &buf[..n];
            let is_hs = packet.len() >= 5 && packet.starts_with(HS_MAGIC);
            if net.is_host {
                if is_hs && packet[4] == HS_HELLO && net.peer.is_none_or(|p| p == src) {
                    net.peer = Some(src);
                    net.remote_payload = Some(packet[5..].to_vec());
                    let _ = socket.send_to(&hs_packet(HS_WELCOME, local), src);
                } else if Some(src) == net.peer && net.remote_payload.is_some() && (!is_hs || packet[4] == HS_READY) {
                    // READY, or the joiner's GGRS already runs (READY lost).
                    net.done = true;
                    break;
                }
            } else if is_hs && packet[4] == HS_WELCOME && Some(src) == net.peer {
                net.remote_payload = Some(packet[5..].to_vec());
                net.done = true;
                // Several times: losing READY only delays the host until GGRS packets arrive.
                for _ in 0..3 {
                    let _ = socket.send_to(&hs_packet(HS_READY, &[]), src);
                }
                break;
            }
        }
    }

    // The joiner says hello every 100 ms until the host answers.
    if !net.is_host && !net.done {
        let now = std::time::Instant::now();
        if net.last_hello.is_none_or(|t| now.duration_since(t).as_millis() >= 100) {
            net.last_hello = Some(now);
            if let Some(peer) = net.peer {
                let _ = socket.send_to(&hs_packet(HS_HELLO, local), peer);
            }
        }
    }

    if !net.done {
        return 0;
    }
    let payload = net.remote_payload.as_deref().unwrap_or(&[]);
    if !out.is_null() && out_len > 0 {
        let n = payload.len().min(out_len);
        // SAFETY: the caller provides out_len bytes.
        unsafe { std::ptr::copy_nonoverlapping(payload.as_ptr(), out, n) };
    }
    // At least 1, so "done" is never confused with "waiting" (payloads are never empty in practice).
    payload.len().max(1) as i32
}

/// Remote address learned by the handshake as "ip:port" (NUL terminated, truncated). Returns the full
/// length, 0 while unknown.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_net_peer_address(net: *mut GgrsNet, buf: *mut c_char, buf_len: usize) -> usize {
    // SAFETY: pointer from ggrs_net_open, not freed yet.
    let Some(net) = (unsafe { net.as_mut() }) else { return 0 };
    let Some(addr) = net.peer else { return 0 };
    let text = addr.to_string();
    if !buf.is_null() && buf_len > 0 {
        let n = text.len().min(buf_len - 1);
        // SAFETY: the caller provides a buffer of buf_len bytes.
        unsafe {
            std::ptr::copy_nonoverlapping(text.as_ptr(), buf as *mut u8, n);
            *buf.add(n) = 0;
        }
    }
    text.len()
}

/// Starts the P2P session on the handshake's socket, with the peer address it learned. `cfg.local_port`
/// and `cfg.remote_addr` are ignored. The handle stays valid for ggrs_net_free. Returns null on error.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_p2p_start_net(net: *mut GgrsNet, cfg: *const GgrsP2PConfig) -> *mut GgrsSession {
    // SAFETY: pointers from the caller (ggrs_net_open / a valid config).
    let (Some(net), Some(cfg)) = (unsafe { net.as_mut() }, unsafe { cfg.as_ref() }) else {
        set_error("null handshake or config");
        return std::ptr::null_mut();
    };
    if !net.done || net.peer.is_none() {
        set_error("handshake not finished");
        return std::ptr::null_mut();
    }
    let Some(socket) = net.socket.take() else {
        set_error("socket already handed to a session");
        return std::ptr::null_mut();
    };
    start_p2p(cfg, socket, net.peer)
}

/// Frees a handshake handle (and its socket, unless a session took it).
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_net_free(net: *mut GgrsNet) {
    if !net.is_null() {
        // SAFETY: pointer from Box::into_raw, freed once.
        drop(unsafe { Box::from_raw(net) });
    }
}

/// Starts a local sync test session: every frame, GGRS rolls back `check_distance` frames and compares
/// the checksums of the resimulated frames. Returns null on error.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_synctest_start(
    num_players: u32,
    check_distance: u32,
    input_delay: u32,
    max_prediction: u32,
) -> *mut GgrsSession {
    let num_players = num_players as usize;
    let build = || -> Result<SyncTestSession<Cfg>, String> {
        SessionBuilder::<Cfg>::new()
            .with_num_players(num_players)
            .map_err(|e| e.to_string())?
            .with_check_distance(check_distance as usize)
            .with_input_delay(input_delay as usize)
            .with_max_prediction_window(max_prediction as usize)
            .start_synctest_session()
            .map_err(|e| e.to_string())
    };
    match build() {
        Ok(session) => new_session(Session::SyncTest(session), num_players),
        Err(e) => {
            set_error(e);
            std::ptr::null_mut()
        }
    }
}

/// Ends a session and frees it.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_free(s: *mut GgrsSession) {
    if !s.is_null() {
        // SAFETY: pointer from Box::into_raw, freed once.
        drop(unsafe { Box::from_raw(s) });
    }
}

/// Receives and sends network packets. Call it every frame (also while waiting).
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_poll(s: *mut GgrsSession) {
    let Some(h) = handle(s) else { return };
    if let Session::P2P(p) = &mut h.session {
        p.poll_remote_clients();
    }
    drain_events(h);
}

/// True when the session runs (peers synchronized), false while synchronizing.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_is_running(s: *mut GgrsSession) -> bool {
    let Some(h) = handle(s) else { return false };
    match &h.session {
        Session::P2P(p) => p.current_state() == SessionState::Running,
        Session::SyncTest(_) => true,
    }
}

/// Adds the input of a local player for the next frame. `input` points to GGRS_INPUT_SIZE bytes.
/// Returns 0 on success.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_add_local_input(s: *mut GgrsSession, player: u32, input: *const u8) -> i32 {
    let Some(h) = handle(s) else { return GGRS_ERR_OTHER };
    if input.is_null() {
        set_error("null input");
        return GGRS_ERR_OTHER;
    }
    let mut bytes = [0u8; GGRS_INPUT_SIZE];
    // SAFETY: the caller provides GGRS_INPUT_SIZE bytes.
    unsafe { std::ptr::copy_nonoverlapping(input, bytes.as_mut_ptr(), GGRS_INPUT_SIZE) };
    let result = match &mut h.session {
        Session::P2P(p) => p.add_local_input(player as usize, PadInput(bytes)),
        Session::SyncTest(t) => t.add_local_input(player as usize, PadInput(bytes)),
    };
    match result {
        Ok(()) => 0,
        Err(e) => error_code(e),
    }
}

/// Advances the session by one frame. Returns the number of requests to answer in order
/// (ggrs_request_*), or a negative GGRS_ERR_* code. GGRS_ERR_PREDICTION_THRESHOLD means: do not run a
/// frame now, poll and try again (the remote input is too far behind).
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_advance_frame(s: *mut GgrsSession) -> i32 {
    let Some(h) = handle(s) else { return GGRS_ERR_OTHER };
    h.requests.clear();
    let result = match &mut h.session {
        Session::P2P(p) => p.advance_frame(),
        Session::SyncTest(t) => t.advance_frame(),
    };
    drain_events(h);
    match result {
        Ok(requests) => {
            h.requests = requests;
            h.requests.len() as i32
        }
        Err(e) => error_code(e),
    }
}

/// Kind of request `index` (GGRS_REQUEST_*), -1 if out of range.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_request_kind(s: *mut GgrsSession, index: u32) -> i32 {
    let Some(h) = handle(s) else { return -1 };
    match h.requests.get(index as usize) {
        Some(GgrsRequest::SaveGameState { .. }) => GGRS_REQUEST_SAVE,
        Some(GgrsRequest::LoadGameState { .. }) => GGRS_REQUEST_LOAD,
        Some(GgrsRequest::AdvanceFrame { .. }) => GGRS_REQUEST_ADVANCE,
        None => -1,
    }
}

/// Frame of a save or load request, -1 otherwise.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_request_frame(s: *mut GgrsSession, index: u32) -> i32 {
    let Some(h) = handle(s) else { return -1 };
    match h.requests.get(index as usize) {
        Some(GgrsRequest::SaveGameState { frame, .. }) | Some(GgrsRequest::LoadGameState { frame, .. }) => *frame,
        _ => -1,
    }
}

/// Answers a save request: the caller saved the current state into its snapshot `slot`. The checksum
/// (128 bit, split in two halves) is used for sync tests and desync detection.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_request_save(
    s: *mut GgrsSession,
    index: u32,
    slot: u32,
    has_checksum: bool,
    checksum_lo: u64,
    checksum_hi: u64,
) -> i32 {
    let Some(h) = handle(s) else { return GGRS_ERR_OTHER };
    match h.requests.get(index as usize) {
        Some(GgrsRequest::SaveGameState { cell, frame }) => {
            let checksum = has_checksum.then(|| (checksum_lo as u128) | ((checksum_hi as u128) << 64));
            cell.save(*frame, Some(slot), checksum);
            0
        }
        _ => {
            set_error("not a save request");
            GGRS_ERR_OTHER
        }
    }
}

/// Answers a load request: returns the snapshot slot to restore, or -1.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_request_load(s: *mut GgrsSession, index: u32) -> i64 {
    let Some(h) = handle(s) else { return -1 };
    match h.requests.get(index as usize) {
        Some(GgrsRequest::LoadGameState { cell, .. }) => cell.load().map_or(-1, |slot| slot as i64),
        _ => {
            set_error("not a load request");
            -1
        }
    }
}

/// Inputs of an advance request: writes num_players * GGRS_INPUT_SIZE bytes to `inputs_out` and one
/// GGRS_INPUT_* status per player to `status_out` (either may be null). Returns the number of players.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_request_inputs(
    s: *mut GgrsSession,
    index: u32,
    inputs_out: *mut u8,
    status_out: *mut i32,
) -> i32 {
    let Some(h) = handle(s) else { return GGRS_ERR_OTHER };
    match h.requests.get(index as usize) {
        Some(GgrsRequest::AdvanceFrame { inputs }) => {
            for (i, (input, status)) in inputs.iter().enumerate().take(h.num_players) {
                // SAFETY: the caller provides num_players * GGRS_INPUT_SIZE bytes and num_players statuses.
                unsafe {
                    if !inputs_out.is_null() {
                        std::ptr::copy_nonoverlapping(
                            input.0.as_ptr(),
                            inputs_out.add(i * GGRS_INPUT_SIZE),
                            GGRS_INPUT_SIZE,
                        );
                    }
                    if !status_out.is_null() {
                        *status_out.add(i) = match status {
                            InputStatus::Confirmed => GGRS_INPUT_CONFIRMED,
                            InputStatus::Predicted => GGRS_INPUT_PREDICTED,
                            InputStatus::Disconnected => GGRS_INPUT_DISCONNECTED,
                        };
                    }
                }
            }
            h.num_players as i32
        }
        _ => {
            set_error("not an advance request");
            GGRS_ERR_OTHER
        }
    }
}

/// Takes the next event. Returns false when there is none.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_next_event(s: *mut GgrsSession, out: *mut GgrsEventC) -> bool {
    let Some(h) = handle(s) else { return false };
    let Some(event) = h.events.pop_front() else { return false };
    let mut e = GgrsEventC::default();
    match event {
        GgrsEvent::Synchronizing { count, total, .. } => {
            e.kind = GGRS_EVENT_SYNCHRONIZING;
            e.a = count as i64;
            e.b = total as i64;
        }
        GgrsEvent::Synchronized { .. } => e.kind = GGRS_EVENT_SYNCHRONIZED,
        GgrsEvent::Disconnected { .. } => e.kind = GGRS_EVENT_DISCONNECTED,
        GgrsEvent::NetworkInterrupted { disconnect_timeout, .. } => {
            e.kind = GGRS_EVENT_NETWORK_INTERRUPTED;
            e.a = disconnect_timeout.min(i64::MAX as u128) as i64;
        }
        GgrsEvent::NetworkResumed { .. } => e.kind = GGRS_EVENT_NETWORK_RESUMED,
        GgrsEvent::WaitRecommendation { skip_frames } => {
            e.kind = GGRS_EVENT_WAIT_RECOMMENDATION;
            e.a = skip_frames as i64;
        }
        GgrsEvent::DesyncDetected { frame, local_checksum, remote_checksum, .. } => {
            e.kind = GGRS_EVENT_DESYNC_DETECTED;
            e.a = frame as i64;
            (e.local_checksum_lo, e.local_checksum_hi) = split_checksum(local_checksum);
            (e.remote_checksum_lo, e.remote_checksum_hi) = split_checksum(remote_checksum);
        }
    }
    if !out.is_null() {
        // SAFETY: the caller provides a valid event struct.
        unsafe { *out = e };
    }
    true
}

/// Frames this peer is ahead of the remote (time synchronization), 0 for sync tests.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_frames_ahead(s: *mut GgrsSession) -> i32 {
    let Some(h) = handle(s) else { return 0 };
    match &h.session {
        Session::P2P(p) => p.frames_ahead(),
        Session::SyncTest(_) => 0,
    }
}

/// Current frame of the session.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_current_frame(s: *mut GgrsSession) -> i32 {
    let Some(h) = handle(s) else { return -1 };
    match &h.session {
        Session::P2P(p) => p.current_frame(),
        Session::SyncTest(t) => t.current_frame(),
    }
}

/// Last frame for which all inputs are confirmed (P2P), -1 otherwise.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_confirmed_frame(s: *mut GgrsSession) -> i32 {
    let Some(h) = handle(s) else { return -1 };
    match &h.session {
        Session::P2P(p) => p.confirmed_frame(),
        Session::SyncTest(_) => -1,
    }
}

/// Network statistics for a remote player. Returns 0 on success.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_network_stats(s: *mut GgrsSession, player: u32, out: *mut GgrsNetworkStatsC) -> i32 {
    let Some(h) = handle(s) else { return GGRS_ERR_OTHER };
    let Session::P2P(p) = &h.session else {
        set_error("no network stats for sync tests");
        return GGRS_ERR_OTHER;
    };
    match p.network_stats(player as usize) {
        Ok(stats) => {
            if !out.is_null() {
                // SAFETY: the caller provides a valid stats struct.
                unsafe {
                    *out = GgrsNetworkStatsC {
                        ping_ms: stats.ping.min(u64::MAX as u128) as u64,
                        send_queue_len: stats.send_queue_len as u64,
                        local_frames_behind: stats.local_frames_behind,
                        remote_frames_behind: stats.remote_frames_behind,
                    };
                }
            }
            0
        }
        Err(e) => error_code(e),
    }
}

/// Remote peer address as "ip:port" (NUL terminated, truncated), learned on the host side. Returns the
/// full length, 0 while unknown or for sync tests.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_peer_address(s: *mut GgrsSession, buf: *mut c_char, buf_len: usize) -> usize {
    let Some(h) = handle(s) else { return 0 };
    let Some(addr) = h.peer_addr.as_ref().and_then(|a| a.get()) else { return 0 };
    let text = addr.to_string();
    if !buf.is_null() && buf_len > 0 {
        let n = text.len().min(buf_len - 1);
        // SAFETY: the caller provides a buffer of buf_len bytes.
        unsafe {
            std::ptr::copy_nonoverlapping(text.as_ptr(), buf as *mut u8, n);
            *buf.add(n) = 0;
        }
    }
    text.len()
}
