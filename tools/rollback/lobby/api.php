<?php
// Mawarunco lobby: a list of open netplay rooms. The match itself never goes through this server: the
// joiner gets the host's IP address and port and connects directly (UDP), as with a typed-in address.
//
//   GET  ?action=list&ver=<build>                          open rooms of this build (never IP addresses),
//                                                          players online, matches running
//   POST action=create  ver, name, delay, rollback, port, [code], [country]   -> id, token
//   POST action=heartbeat  id, token                       keeps the room listed -> status, guest
//   POST action=join    id, ver, name, [code]              -> host address, port, delay, rollback
//   POST action=reopen  id, token                          the joiner did not connect: list the room again
//   POST action=close   id, token
//
// Tables: schema.sql (created once with the database's main login; this script only needs a login that
// can read and write rows).
//
// Stored per room: host name, the country the host chose to show (optional; no IP lookup), room settings,
// the host's IP address and port (only to hand them to the joiner), a hash of the room code and of the
// host's token. Rooms disappear 30 s after the host's last
// heartbeat, together with the IP address. The IP address is stored encrypted (AES-256-GCM, key derived
// from the secret in the config), so the database alone does not reveal it; a keyed hash of it finds the
// host's previous room. Rate limits keep a hash of the client IP with a key that changes every day, for at
// most an hour. Nothing else is stored or logged by this script.
//
// Every value from a client is checked against a fixed format; all database access uses prepared
// statements. Responses are JSON only.

declare(strict_types=1);

const PROTOCOL = 1;
const ROOM_TIMEOUT = 30;        // seconds without heartbeat until a room is gone
const PLAYING_TIMEOUT = 30;     // same for a room in a match (host closes it when the match ends)
const MAX_ROOMS = 200;          // listed rooms in total
const NAME_PATTERN = '/^[A-Za-z0-9 .!?-]{1,20}$/';  // the game's font, as in the launcher
const CODE_PATTERN = '/^[A-Za-z0-9]{4,16}$/';
const COUNTRY_PATTERN = '/^[A-Z]{2}$/';   // ISO 3166-1 alpha-2, chosen by the host
const VER_PATTERN = '/^[0-9a-f]{40}$/';
const ID_PATTERN = '/^[0-9a-f]{16}$/';
const TOKEN_PATTERN = '/^[0-9a-f]{64}$/';

// Requests per client IP and window: [limit, seconds].
const LIMITS = [
    'list' => [120, 60],
    'create' => [10, 600],
    'heartbeat' => [60, 60],
    'join' => [15, 600],        // each join reveals one host address: keep harvesting slow
    'join_fail' => [10, 600],   // wrong room codes
    'reopen' => [20, 600],
    'close' => [20, 60],
];

// No PHP messages in replies (they can contain paths or database details), whatever the hosting's default.
ini_set('display_errors', '0');
set_exception_handler(static function (Throwable $e): void {
    error_log('mawarunco lobby: ' . get_class($e));
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'server error', 'protocol' => PROTOCOL]);
});

header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');
header('X-Content-Type-Options: nosniff');
header('Referrer-Policy: no-referrer');
header("Content-Security-Policy: default-src 'none'; frame-ancestors 'none'");
header('Strict-Transport-Security: max-age=31536000');

// The lobby hands out IP addresses: refuse plain HTTP outright (the launcher only uses HTTPS). A test
// server on the same PC (127.0.0.1) may use plain HTTP, nothing leaves the machine there.
$https = (!empty($_SERVER['HTTPS']) && strtolower((string) $_SERVER['HTTPS']) !== 'off') ||
    ((string) ($_SERVER['SERVER_PORT'] ?? '') === '443');
$local = in_array((string) ($_SERVER['REMOTE_ADDR'] ?? ''), ['127.0.0.1', '::1'], true);
if (!$https && !$local) {
    http_response_code(403);
    echo json_encode(['ok' => false, 'error' => 'HTTPS only', 'protocol' => PROTOCOL]);
    exit;
}

function respond(int $status, array $body): never
{
    http_response_code($status);
    echo json_encode($body + ['protocol' => PROTOCOL], JSON_UNESCAPED_SLASHES);
    exit;
}

function fail(int $status, string $message): never
{
    respond($status, ['ok' => false, 'error' => $message]);
}

function param(string $key, string $pattern): string
{
    $source = ($_SERVER['REQUEST_METHOD'] === 'POST') ? $_POST : $_GET;
    $value = $source[$key] ?? null;
    if (!is_string($value) || !preg_match($pattern, $value)) {
        fail(400, "invalid or missing value: $key");
    }
    return $value;
}

function optional_param(string $key, string $pattern): ?string
{
    $value = $_POST[$key] ?? '';
    if (!is_string($value) || $value === '') {
        return null;
    }
    if (!preg_match($pattern, $value)) {
        fail(400, "invalid value: $key");
    }
    return $value;
}

function int_param(string $key, int $min, int $max): int
{
    $value = param($key, '/^[0-9]{1,5}$/');
    $number = (int) $value;
    if ($number < $min || $number > $max) {
        fail(400, "value out of range: $key");
    }
    return $number;
}

function load_config(): array
{
    // Preferably outside the web folder; config.php next to this file is blocked by .htaccess.
    foreach ([__DIR__ . '/../mawarunco-lobby-config.php', __DIR__ . '/config.php'] as $file) {
        if (is_file($file)) {
            $config = require $file;
            if (is_array($config)) {
                return $config;
            }
        }
    }
    fail(500, 'server not configured');
}

function db(array $config): PDO
{
    try {
        $pdo = new PDO($config['dsn'], $config['user'], $config['password'], [
            PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
            PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
            PDO::ATTR_EMULATE_PREPARES => false,
        ]);
    } catch (PDOException $e) {
        // Only the driver's error number (e.g. 1045 access denied, 1049 unknown database, 2002 server not
        // found), never the message: that can contain the host or user name.
        $code = (int) ($e->errorInfo[1] ?? 0);
        if ($code === 0 && str_contains($e->getMessage(), 'could not find driver')) {
            fail(503, 'database unavailable (no MySQL driver in PHP)');
        }
        fail(503, 'database unavailable' . ($code > 0 ? " ($code)" : ''));
    }
    return $pdo;
}

function client_ip(): string
{
    // Direct connection on shared hosting: REMOTE_ADDR is the client. Forwarded headers are not trusted.
    return (string) ($_SERVER['REMOTE_ADDR'] ?? '');
}

/// Key for one purpose, derived from the secret in the config.
function derived_key(array $config, string $purpose): string
{
    return hash_hmac('sha256', $purpose, (string) $config['salt'], true);
}

/// Rate-limit key of the client: the key changes every day (UTC), so entries cannot be linked across days.
function client_key(array $config): string
{
    return hash_hmac('sha256', client_ip(), derived_key($config, 'limits-' . gmdate('Y-m-d')));
}

/// Finds a host's previous room without storing the address in the clear.
function host_key(array $config, string $ip): string
{
    return hash_hmac('sha256', $ip, derived_key($config, 'host'));
}

function encrypt_ip(array $config, string $ip): string
{
    $iv = random_bytes(12);
    $tag = '';
    $cipher = openssl_encrypt($ip, 'aes-256-gcm', derived_key($config, 'ip-encryption'), OPENSSL_RAW_DATA, $iv, $tag);
    if ($cipher === false) {
        fail(500, 'encryption unavailable');
    }
    return base64_encode($iv . $tag . $cipher);
}

function decrypt_ip(array $config, string $stored): ?string
{
    $raw = base64_decode($stored, true);
    if ($raw === false || strlen($raw) < 29) {
        return null;
    }
    $ip = openssl_decrypt(substr($raw, 28), 'aes-256-gcm', derived_key($config, 'ip-encryption'), OPENSSL_RAW_DATA,
        substr($raw, 0, 12), substr($raw, 12, 16));
    return ($ip === false) ? null : $ip;
}

function rate_limit(PDO $pdo, string $client, string $bucket, bool $count = true): void
{
    [$limit, $window] = LIMITS[$bucket];
    $now = time();
    $row = $pdo->prepare('SELECT window_start, hits FROM mwr_limits WHERE client = ? AND bucket = ?');
    $row->execute([$client, $bucket]);
    $entry = $row->fetch();
    if ($entry && $entry['window_start'] > $now - $window) {
        if ($entry['hits'] >= $limit) {
            fail(429, 'too many requests, try again later');
        }
        if ($count) {
            $pdo->prepare('UPDATE mwr_limits SET hits = hits + 1 WHERE client = ? AND bucket = ?')->execute([$client, $bucket]);
        }
    } elseif ($count) {
        $pdo->prepare('REPLACE INTO mwr_limits (client, bucket, window_start, hits) VALUES (?, ?, ?, 1)')
            ->execute([$client, $bucket, $now]);
    }
}

function cleanup(PDO $pdo): void
{
    $now = time();
    $pdo->prepare('DELETE FROM mwr_rooms WHERE (status = ? AND last_seen < ?) OR (status = ? AND last_seen < ?)')
        ->execute(['open', $now - ROOM_TIMEOUT, 'playing', $now - PLAYING_TIMEOUT]);
    $pdo->prepare('DELETE FROM mwr_limits WHERE window_start < ?')->execute([$now - 3600]);
}

function owned_room(PDO $pdo): array
{
    $id = param('id', ID_PATTERN);
    $token = param('token', TOKEN_PATTERN);
    $query = $pdo->prepare('SELECT * FROM mwr_rooms WHERE id = ?');
    $query->execute([$id]);
    $room = $query->fetch();
    if (!$room || !hash_equals($room['token_hash'], hash('sha256', $token))) {
        fail(404, 'room not found');
    }
    return $room;
}

// ---- Request ------------------------------------------------------------------------------------------

$method = $_SERVER['REQUEST_METHOD'] ?? 'GET';
$action = ($method === 'POST') ? ($_POST['action'] ?? '') : ($_GET['action'] ?? '');
if (!is_string($action) || !array_key_exists($action, LIMITS) || $action === 'join_fail') {
    fail(400, 'unknown action');
}
if (($action === 'list') !== ($method === 'GET')) {
    fail(405, 'wrong method');
}

$config = load_config();
$pdo = db($config);
$client = client_key($config);
rate_limit($pdo, $client, $action);
cleanup($pdo);
$now = time();

switch ($action) {
    case 'list':
        $ver = param('ver', VER_PATTERN);
        $query = $pdo->prepare('SELECT id, host_name, country, delay_frames, rollback_frames, code_hash, created FROM mwr_rooms
            WHERE ver = ? AND status = ? ORDER BY created DESC LIMIT ' . MAX_ROOMS);
        $query->execute([$ver, 'open']);
        $rooms = [];
        foreach ($query as $room) {
            $rooms[] = [
                'id' => $room['id'],
                'host' => $room['host_name'],
                'country' => $room['country'],
                'delay' => (int) $room['delay_frames'],
                'rollback' => (int) $room['rollback_frames'],
                'locked' => $room['code_hash'] !== null,
                'age' => $now - (int) $room['created'],
            ];
        }
        $playing = $pdo->prepare('SELECT COUNT(*) FROM mwr_rooms WHERE ver = ? AND status = ?');
        $playing->execute([$ver, 'playing']);
        $matches = (int) $playing->fetchColumn();
        // Players online (all versions), from data the lobby keeps anyway: launchers that looked at the room
        // list in the last minute (one rate-limit entry each), hosts waiting in an open room, two players per
        // running match. Players behind one IP address count once.
        $browsing = $pdo->prepare('SELECT COUNT(*) FROM mwr_limits WHERE bucket = ? AND window_start > ?');
        $browsing->execute(['list', $now - 70]);
        $open = (int) $pdo->query("SELECT COUNT(*) FROM mwr_rooms WHERE status = 'open'")->fetchColumn();
        $all_matches = (int) $pdo->query("SELECT COUNT(*) FROM mwr_rooms WHERE status = 'playing'")->fetchColumn();
        $online = (int) $browsing->fetchColumn() + $open + 2 * $all_matches;
        respond(200, ['ok' => true, 'rooms' => $rooms, 'playing' => $matches, 'online' => $online]);

    case 'create':
        $ver = param('ver', VER_PATTERN);
        $name = param('name', NAME_PATTERN);
        $delay = int_param('delay', 0, 8);
        $rollback = int_param('rollback', 1, 8);
        $port = int_param('port', 1024, 65535);
        $code = optional_param('code', CODE_PATTERN);
        $country = optional_param('country', COUNTRY_PATTERN);
        $ip = client_ip();
        // Netplay is IPv4 only, and the address must be reachable from the internet.
        $flags = FILTER_FLAG_IPV4 | (empty($config['allow_private_hosts']) ? FILTER_FLAG_NO_PRIV_RANGE | FILTER_FLAG_NO_RES_RANGE : 0);
        if (!filter_var($ip, FILTER_VALIDATE_IP, $flags)) {
            fail(422, 'hosting needs a public IPv4 address');
        }
        // One room per IP address: a new room replaces the old one.
        $host = host_key($config, $ip);
        $pdo->prepare('DELETE FROM mwr_rooms WHERE host_key = ?')->execute([$host]);
        if ((int) $pdo->query('SELECT COUNT(*) FROM mwr_rooms')->fetchColumn() >= MAX_ROOMS) {
            fail(503, 'the lobby is full, try again later');
        }
        $id = bin2hex(random_bytes(8));
        $token = bin2hex(random_bytes(32));
        $pdo->prepare('INSERT INTO mwr_rooms (id, token_hash, ver, host_name, country, host_key, host_ip, host_port, delay_frames,
            rollback_frames, code_hash, status, guest_name, created, last_seen) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, ?, ?)')
            ->execute([$id, hash('sha256', $token), $ver, $name, $country, $host, encrypt_ip($config, $ip), $port, $delay, $rollback,
                $code === null ? null : password_hash(strtoupper($code), PASSWORD_DEFAULT), 'open', $now, $now]);
        respond(200, ['ok' => true, 'id' => $id, 'token' => $token]);

    case 'heartbeat':
        $room = owned_room($pdo);
        $pdo->prepare('UPDATE mwr_rooms SET last_seen = ? WHERE id = ?')->execute([$now, $room['id']]);
        respond(200, ['ok' => true, 'status' => $room['status'], 'guest' => $room['guest_name']]);

    case 'join':
        $id = param('id', ID_PATTERN);
        $ver = param('ver', VER_PATTERN);
        $name = param('name', NAME_PATTERN);
        $code = optional_param('code', CODE_PATTERN);
        rate_limit($pdo, $client, 'join_fail', false);
        $query = $pdo->prepare('SELECT * FROM mwr_rooms WHERE id = ?');
        $query->execute([$id]);
        $room = $query->fetch();
        if (!$room || $room['status'] !== 'open') {
            fail(404, 'the room is gone or already playing');
        }
        if ($room['ver'] !== $ver) {
            fail(409, 'the host uses another Mawarunco version');
        }
        if ($room['code_hash'] !== null && ($code === null || !password_verify(strtoupper($code), $room['code_hash']))) {
            rate_limit($pdo, $client, 'join_fail');
            fail(403, 'wrong room code');
        }
        // Only one joiner: the first one takes the room.
        $take = $pdo->prepare('UPDATE mwr_rooms SET status = ?, guest_name = ? WHERE id = ? AND status = ?');
        $take->execute(['playing', $name, $id, 'open']);
        if ($take->rowCount() !== 1) {
            fail(404, 'the room is gone or already playing');
        }
        $address = decrypt_ip($config, $room['host_ip']);
        if ($address === null) {
            fail(500, 'room data unreadable');
        }
        respond(200, ['ok' => true, 'address' => $address, 'port' => (int) $room['host_port'],
            'delay' => (int) $room['delay_frames'], 'rollback' => (int) $room['rollback_frames'], 'host' => $room['host_name']]);

    case 'reopen':
        // The host's game saw no connection from the joiner: back into the list for the next player.
        $room = owned_room($pdo);
        $pdo->prepare('UPDATE mwr_rooms SET status = ?, guest_name = NULL, last_seen = ? WHERE id = ? AND status = ?')
            ->execute(['open', $now, $room['id'], 'playing']);
        respond(200, ['ok' => true]);

    case 'close':
        $room = owned_room($pdo);
        $pdo->prepare('DELETE FROM mwr_rooms WHERE id = ?')->execute([$room['id']]);
        respond(200, ['ok' => true]);
}
