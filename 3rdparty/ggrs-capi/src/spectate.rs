//! Spectators for the PCSX2 rollback fork.
//!
//! The host keeps the confirmed inputs of both players from session frame 0 on (36 bytes per frame) and
//! serves them to up to four spectators on the session's UDP socket. A spectator starts its emulator at
//! frame 0 like the players and plays the frames back, as fast as it can while it is behind, so it can
//! join a match that already runs. Spectators never affect the players: they only ask for frames.
//!
//! Packets start with SPEC_MAGIC (never a valid GGRS message for the peer socket):
//!   REQ       spectator -> host: proto u8, token u32, next_frame u32, window u8, version_len u8, version
//!   CHALLENGE host -> spectator: token u32. Data is only sent to an address that echoes its token, so a
//!             forged sender address cannot make the host send large answers to someone else.
//!   DATA      host -> spectator: total u32, start u32, count u8, ended u8, count * frames,
//!             checksum_count u8, checksum_count * (frame u32, lo u64, hi u64)
//!   WAIT      host -> spectator: the match has not started yet
//!   FULL      host -> spectator: four spectators already watch
//!   REJECT    host -> spectator: reason_len u8, reason (another program version)
//! All numbers little endian.

use std::collections::{BTreeMap, HashMap};
use std::ffi::{CStr, c_char};
use std::io::ErrorKind;
use std::net::{SocketAddr, UdpSocket};
use std::time::{Duration, Instant};

use crate::{GGRS_ERR_OTHER, GGRS_INPUT_SIZE, set_error};

pub const SPEC_MAGIC: &[u8; 4] = b"FUCS";
const SPEC_PROTO: u8 = 1;
const K_REQ: u8 = 1;
const K_DATA: u8 = 2;
const K_WAIT: u8 = 3;
const K_FULL: u8 = 4;
const K_REJECT: u8 = 5;
const K_CHALLENGE: u8 = 7;

/// Both players' inputs of one frame.
pub const SPEC_FRAME_BYTES: usize = 2 * GGRS_INPUT_SIZE;
pub const MAX_SPECTATORS: usize = 4;
/// Frames per DATA packet (24 * 36 = 864 bytes plus header: below common MTUs).
const BATCH: usize = 24;
const MAX_WINDOW: u8 = 8;
const CLIENT_TIMEOUT: Duration = Duration::from_secs(10);
const HOST_TIMEOUT: Duration = Duration::from_secs(15);

fn packet(kind: u8, payload: &[u8]) -> Vec<u8> {
    let mut p = Vec::with_capacity(5 + payload.len());
    p.extend_from_slice(SPEC_MAGIC);
    p.push(kind);
    p.extend_from_slice(payload);
    p
}

fn read_u32(b: &[u8], at: usize) -> Option<u32> {
    b.get(at..at + 4).map(|s| u32::from_le_bytes(s.try_into().unwrap()))
}

fn read_u64(b: &[u8], at: usize) -> Option<u64> {
    b.get(at..at + 8).map(|s| u64::from_le_bytes(s.try_into().unwrap()))
}

/// Reply to a spectator before the match runs (handshake socket on the host).
pub fn wait_packet() -> Vec<u8> {
    packet(K_WAIT, &[])
}

// ---------------------------------------------------------------------------------------------------
// Host side
// ---------------------------------------------------------------------------------------------------

pub struct SpecHub {
    socket: Option<UdpSocket>,
    frames: Vec<[u8; SPEC_FRAME_BYTES]>,
    checksums: HashMap<u32, (u64, u64)>,
    version: Vec<u8>,
    clients: Vec<(SocketAddr, Instant)>,
    secret: u64,
    ended: bool,
}

impl SpecHub {
    pub fn new(socket: Option<UdpSocket>) -> Self {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0x9e37_79b9_7f4a_7c15);
        Self {
            socket,
            frames: Vec::new(),
            checksums: HashMap::new(),
            version: Vec::new(),
            clients: Vec::new(),
            secret: nanos ^ 0x5bd1_e995_1234_5678,
            ended: false,
        }
    }

    fn token(&self, addr: &SocketAddr) -> u32 {
        // FNV-1a over the address and a per-session secret.
        let mut h: u64 = 0xcbf2_9ce4_8422_2325 ^ self.secret;
        for b in addr.to_string().bytes() {
            h ^= b as u64;
            h = h.wrapping_mul(0x0100_0000_01b3);
        }
        (h ^ (h >> 32)) as u32 | 1
    }

    fn send(&self, to: SocketAddr, data: &[u8]) {
        if let Some(s) = &self.socket {
            let _ = s.send_to(data, to);
        }
    }

    pub fn set_version(&mut self, v: &[u8]) {
        self.version = v.to_vec();
    }

    /// Adds the confirmed inputs of `frame` (frames come in order). Returns false on a gap.
    pub fn push(&mut self, frame: u32, inputs: &[u8; SPEC_FRAME_BYTES]) -> bool {
        let len = self.frames.len() as u32;
        if frame == len {
            self.frames.push(*inputs);
            true
        } else {
            frame < len
        }
    }

    pub fn set_checksum(&mut self, frame: u32, lo: u64, hi: u64) {
        self.checksums.insert(frame, (lo, hi));
    }

    pub fn active(&self) -> usize {
        let now = Instant::now();
        self.clients.iter().filter(|(_, t)| now.duration_since(*t) < CLIENT_TIMEOUT).count()
    }

    pub fn end(&mut self) {
        self.ended = true;
        // Tell everyone right away (a spectator that is behind keeps asking and gets the rest).
        let clients: Vec<SocketAddr> = self.clients.iter().map(|(a, _)| *a).collect();
        for addr in clients {
            self.send_data(addr, self.frames.len() as u32, 1);
        }
    }

    pub fn handle(&mut self, src: SocketAddr, p: &[u8]) {
        if p.len() < 5 || p[4] != K_REQ {
            return;
        }
        let (Some(&proto), Some(token), Some(next), Some(&window), Some(&vlen)) =
            (p.get(5), read_u32(p, 6), read_u32(p, 10), p.get(14), p.get(15))
        else {
            return;
        };
        if proto != SPEC_PROTO {
            self.reject(src, "another Mawarunco version");
            return;
        }
        let expected = self.token(&src);
        if token != expected {
            self.send(src, &packet(K_CHALLENGE, &expected.to_le_bytes()));
            return;
        }
        let version = p.get(16..16 + vlen as usize).unwrap_or(&[]);
        if !self.version.is_empty() && version != self.version.as_slice() {
            self.reject(src, "another Mawarunco version");
            return;
        }
        let now = Instant::now();
        if let Some(c) = self.clients.iter_mut().find(|(a, _)| *a == src) {
            c.1 = now;
        } else {
            self.clients.retain(|(_, t)| now.duration_since(*t) < CLIENT_TIMEOUT);
            if self.clients.len() >= MAX_SPECTATORS {
                self.send(src, &packet(K_FULL, &[]));
                return;
            }
            self.clients.push((src, now));
        }
        self.send_data(src, next, window.clamp(1, MAX_WINDOW));
    }

    fn reject(&self, to: SocketAddr, reason: &str) {
        let mut payload = vec![reason.len().min(200) as u8];
        payload.extend_from_slice(&reason.as_bytes()[..reason.len().min(200)]);
        self.send(to, &packet(K_REJECT, &payload));
    }

    fn send_data(&self, to: SocketAddr, mut next: u32, window: u8) {
        let total = self.frames.len() as u32;
        for i in 0..window {
            let count = (total.saturating_sub(next) as usize).min(BATCH);
            if count == 0 && i > 0 {
                break;
            }
            let mut payload = Vec::with_capacity(14 + count * SPEC_FRAME_BYTES + 64);
            payload.extend_from_slice(&total.to_le_bytes());
            payload.extend_from_slice(&next.to_le_bytes());
            payload.push(count as u8);
            payload.push(self.ended as u8);
            for f in next..next + count as u32 {
                payload.extend_from_slice(&self.frames[f as usize]);
            }
            let sums: Vec<(u32, (u64, u64))> = (next..next + count as u32)
                .filter_map(|f| self.checksums.get(&f).map(|c| (f, *c)))
                .collect();
            payload.push(sums.len() as u8);
            for (f, (lo, hi)) in sums {
                payload.extend_from_slice(&f.to_le_bytes());
                payload.extend_from_slice(&lo.to_le_bytes());
                payload.extend_from_slice(&hi.to_le_bytes());
            }
            self.send(to, &packet(K_DATA, &payload));
            next += count as u32;
            if count < BATCH {
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// Spectator side
// ---------------------------------------------------------------------------------------------------

pub const SPEC_CONNECTING: i32 = 0;
pub const SPEC_WAITING: i32 = 1;
pub const SPEC_RUNNING: i32 = 2;
pub const SPEC_REJECTED: i32 = 4;
pub const SPEC_FULL: i32 = 5;
pub const SPEC_TIMEOUT: i32 = 6;

#[repr(C)]
#[derive(Default)]
pub struct GgrsSpecStatusC {
    /// SPEC_* state.
    pub state: i32,
    /// Frames the host has confirmed so far.
    pub host_frames: u32,
    /// Frames from the next one to take on that are already here (contiguous).
    pub buffered: u32,
    /// The host's session has ended (no more frames will come after host_frames).
    pub ended: bool,
}

pub struct GgrsSpec {
    socket: UdpSocket,
    host: SocketAddr,
    version: Vec<u8>,
    token: u32,
    frames: BTreeMap<u32, [u8; SPEC_FRAME_BYTES]>,
    checksums: HashMap<u32, (u64, u64)>,
    next: u32,
    requested_until: u32,
    host_frames: u32,
    ended: bool,
    state: i32,
    reason: String,
    last_req: Option<Instant>,
    last_heard: Instant,
}

impl GgrsSpec {
    fn contiguous_end(&self) -> u32 {
        let mut f = self.next;
        while self.frames.contains_key(&f) {
            f += 1;
        }
        f
    }

    fn receive(&mut self) {
        let mut buf = [0u8; 2048];
        loop {
            let (n, src) = match self.socket.recv_from(&mut buf) {
                Ok(r) => r,
                Err(ref e) if e.kind() == ErrorKind::WouldBlock => return,
                Err(ref e) if e.kind() == ErrorKind::ConnectionReset => continue,
                Err(_) => return,
            };
            let p = &buf[..n];
            if src != self.host || n < 5 || !p.starts_with(SPEC_MAGIC) {
                continue;
            }
            self.last_heard = Instant::now();
            match p[4] {
                K_CHALLENGE => {
                    if let Some(t) = read_u32(p, 5) {
                        self.token = t;
                        self.last_req = None; // ask again right away
                    }
                }
                K_WAIT => self.state = SPEC_WAITING,
                K_FULL => self.state = SPEC_FULL,
                K_REJECT => {
                    let len = p.get(5).copied().unwrap_or(0) as usize;
                    self.reason = String::from_utf8_lossy(p.get(6..6 + len).unwrap_or(&[])).into_owned();
                    self.state = SPEC_REJECTED;
                }
                K_DATA => self.on_data(p),
                _ => {}
            }
        }
    }

    fn on_data(&mut self, p: &[u8]) {
        let (Some(total), Some(start), Some(&count), Some(&ended)) =
            (read_u32(p, 5), read_u32(p, 9), p.get(13), p.get(14))
        else {
            return;
        };
        let count = count as usize;
        let frames_at = 15;
        let sums_at = frames_at + count * SPEC_FRAME_BYTES;
        if p.len() < sums_at + 1 || count > BATCH {
            return;
        }
        self.state = SPEC_RUNNING;
        self.host_frames = self.host_frames.max(total);
        self.ended |= ended != 0;
        for i in 0..count {
            let f = start + i as u32;
            if f < self.next {
                continue;
            }
            let at = frames_at + i * SPEC_FRAME_BYTES;
            let mut frame = [0u8; SPEC_FRAME_BYTES];
            frame.copy_from_slice(&p[at..at + SPEC_FRAME_BYTES]);
            self.frames.insert(f, frame);
        }
        let n_sums = p[sums_at] as usize;
        for i in 0..n_sums {
            let at = sums_at + 1 + i * 20;
            if let (Some(f), Some(lo), Some(hi)) = (read_u32(p, at), read_u64(p, at + 4), read_u64(p, at + 12)) {
                if f >= self.next {
                    self.checksums.insert(f, (lo, hi));
                }
            }
        }
    }

    fn request(&mut self) {
        if matches!(self.state, SPEC_REJECTED | SPEC_FULL | SPEC_TIMEOUT) {
            return;
        }
        let now = Instant::now();
        let have = self.contiguous_end();
        let behind = self.host_frames.saturating_sub(have);
        // Live: ask often for few frames. Behind: keep several batches in flight; if an answer is
        // overdue (lost packet), ask again from the first missing frame.
        let interval = if behind > BATCH as u32 { 10 } else { 16 };
        let Some(last) = self.last_req else {
            return self.send_request(have, if behind > BATCH as u32 { MAX_WINDOW } else { 2 });
        };
        let since = now.duration_since(last).as_millis();
        if since < interval {
            return;
        }
        let overdue = since > 150;
        let from = if overdue || self.requested_until <= have { have } else { self.requested_until };
        // Do not buffer more than ~20 s ahead of playback.
        if from.saturating_sub(self.next) > 1200 && !overdue {
            return;
        }
        let window = if behind > BATCH as u32 { MAX_WINDOW } else { 2 };
        self.send_request(from, window);
    }

    fn send_request(&mut self, from: u32, window: u8) {
        let mut payload = Vec::with_capacity(12 + self.version.len());
        payload.push(SPEC_PROTO);
        payload.extend_from_slice(&self.token.to_le_bytes());
        payload.extend_from_slice(&from.to_le_bytes());
        payload.push(window);
        payload.push(self.version.len().min(200) as u8);
        payload.extend_from_slice(&self.version[..self.version.len().min(200)]);
        let _ = self.socket.send_to(&packet(K_REQ, &payload), self.host);
        self.last_req = Some(Instant::now());
        self.requested_until = from + window as u32 * BATCH as u32;
    }
}

/// Connects to a host as a spectator. `host_addr` is "ip:port"; `version` must match the host's.
/// Returns null on error.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_spec_open(host_addr: *const c_char, version: *const u8, version_len: usize) -> *mut GgrsSpec {
    if host_addr.is_null() {
        set_error("null host address");
        return std::ptr::null_mut();
    }
    // SAFETY: NUL terminated string from the caller.
    let text = unsafe { CStr::from_ptr(host_addr) }.to_string_lossy();
    let host: SocketAddr = match text.trim().parse() {
        Ok(a) => a,
        Err(e) => {
            set_error(format!("invalid host address '{text}': {e}"));
            return std::ptr::null_mut();
        }
    };
    let version = if version.is_null() {
        Vec::new()
    } else {
        // SAFETY: the caller provides version_len bytes.
        unsafe { std::slice::from_raw_parts(version, version_len) }.to_vec()
    };
    let bind = if host.is_ipv6() { "[::]:0" } else { "0.0.0.0:0" };
    let socket = match UdpSocket::bind(bind).and_then(|s| s.set_nonblocking(true).map(|_| s)) {
        Ok(s) => s,
        Err(e) => {
            set_error(format!("cannot open a UDP socket: {e}"));
            return std::ptr::null_mut();
        }
    };
    Box::into_raw(Box::new(GgrsSpec {
        socket,
        host,
        version,
        token: 0,
        frames: BTreeMap::new(),
        checksums: HashMap::new(),
        next: 0,
        requested_until: 0,
        host_frames: 0,
        ended: false,
        state: SPEC_CONNECTING,
        reason: String::new(),
        last_req: None,
        last_heard: Instant::now(),
    }))
}

/// Receives frames and asks for more; call it often (every frame, and every millisecond while waiting).
/// Writes the status to `out` (may be null) and returns the state.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_spec_poll(spec: *mut GgrsSpec, out: *mut GgrsSpecStatusC) -> i32 {
    // SAFETY: pointer from ggrs_spec_open, not freed yet.
    let Some(s) = (unsafe { spec.as_mut() }) else { return GGRS_ERR_OTHER };
    s.receive();
    if !matches!(s.state, SPEC_REJECTED | SPEC_FULL) && Instant::now().duration_since(s.last_heard) > HOST_TIMEOUT {
        s.state = SPEC_TIMEOUT;
    }
    s.request();
    if !out.is_null() {
        let buffered = s.contiguous_end() - s.next;
        // SAFETY: the caller provides a valid status struct.
        unsafe {
            *out = GgrsSpecStatusC { state: s.state, host_frames: s.host_frames, buffered, ended: s.ended };
        }
    }
    s.state
}

/// Takes the inputs of `frame` (the next frame to play): SPEC_FRAME_BYTES bytes to `inputs_out`, and the
/// host's checksum of the state at the start of that frame if it sent one. Returns false if the frame is
/// not here yet.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_spec_take(
    spec: *mut GgrsSpec,
    frame: u32,
    inputs_out: *mut u8,
    has_checksum: *mut bool,
    checksum_lo: *mut u64,
    checksum_hi: *mut u64,
) -> bool {
    // SAFETY: pointer from ggrs_spec_open, not freed yet.
    let Some(s) = (unsafe { spec.as_mut() }) else { return false };
    let Some(inputs) = s.frames.remove(&frame) else { return false };
    let sum = s.checksums.remove(&frame);
    // Everything before this frame is played.
    s.next = frame + 1;
    while let Some((&f, _)) = s.frames.first_key_value() {
        if f > frame {
            break;
        }
        s.frames.remove(&f);
    }
    s.checksums.retain(|&f, _| f > frame);
    // SAFETY: the caller provides SPEC_FRAME_BYTES bytes and valid pointers (or null).
    unsafe {
        if !inputs_out.is_null() {
            std::ptr::copy_nonoverlapping(inputs.as_ptr(), inputs_out, SPEC_FRAME_BYTES);
        }
        if !has_checksum.is_null() {
            *has_checksum = sum.is_some();
        }
        if let Some((lo, hi)) = sum {
            if !checksum_lo.is_null() {
                *checksum_lo = lo;
            }
            if !checksum_hi.is_null() {
                *checksum_hi = hi;
            }
        }
    }
    true
}

/// Why the host turned the spectator away (NUL terminated, truncated). Returns the full length.
#[unsafe(no_mangle)]
pub extern "C" fn ggrs_spec_reason(spec: *mut GgrsSpec, buf: *mut c_char, buf_len: usize) -> usize {
    // SAFETY: pointer from ggrs_spec_open, not freed yet.
    let Some(s) = (unsafe { spec.as_mut() }) else { return 0 };
    let bytes = s.reason.as_bytes();
    if !buf.is_null() && buf_len > 0 {
        let n = bytes.len().min(buf_len - 1);
        // SAFETY: the caller provides buf_len bytes.
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
            *buf.add(n) = 0;
        }
    }
    bytes.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn ggrs_spec_free(spec: *mut GgrsSpec) {
    if !spec.is_null() {
        // SAFETY: pointer from Box::into_raw, freed once.
        drop(unsafe { Box::from_raw(spec) });
    }
}
