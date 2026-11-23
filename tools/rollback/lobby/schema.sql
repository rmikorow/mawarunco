-- Tables of the Mawarunco lobby (MariaDB/MySQL). Run once with the database's main login, e.g. in the
-- hosting's database tool. api.php then only needs a read/write login.
--
-- Updating from an earlier version: the rooms table only holds rooms of the last seconds, so it can be
-- replaced. Run "DROP TABLE IF EXISTS mwr_rooms;" first, then this file.
CREATE TABLE IF NOT EXISTS mwr_rooms (
    id CHAR(16) PRIMARY KEY,
    token_hash CHAR(64) NOT NULL,
    ver CHAR(40) NOT NULL,
    host_name VARCHAR(20) NOT NULL,
    country CHAR(2) NULL,
    host_key CHAR(64) NOT NULL,      -- keyed hash of the host's IP address (finds the previous room)
    host_ip VARCHAR(255) NOT NULL,   -- the host's IP address, encrypted (AES-256-GCM)
    host_port INT NOT NULL,
    delay_frames TINYINT NOT NULL,
    rollback_frames TINYINT NOT NULL,
    code_hash VARCHAR(255) NULL,
    status VARCHAR(8) NOT NULL,
    guest_name VARCHAR(20) NULL,
    created INT NOT NULL,
    last_seen INT NOT NULL,
    INDEX rooms_list (ver, status, last_seen),
    INDEX rooms_host (host_key)
);

CREATE TABLE IF NOT EXISTS mwr_limits (
    client CHAR(64) NOT NULL,
    bucket VARCHAR(16) NOT NULL,
    window_start INT NOT NULL,
    hits INT NOT NULL,
    PRIMARY KEY (client, bucket)
);
