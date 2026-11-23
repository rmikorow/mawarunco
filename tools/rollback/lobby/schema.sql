-- Tables of the Mawarunco lobby (MariaDB/MySQL). Run once with the database's main login, e.g. in the
-- hosting's database tool. api.php then only needs a read/write login.
CREATE TABLE IF NOT EXISTS mwr_rooms (
    id CHAR(16) PRIMARY KEY,
    token_hash CHAR(64) NOT NULL,
    ver CHAR(40) NOT NULL,
    host_name VARCHAR(20) NOT NULL,
    host_ip VARCHAR(15) NOT NULL,
    host_port INT NOT NULL,
    delay_frames TINYINT NOT NULL,
    rollback_frames TINYINT NOT NULL,
    code_hash VARCHAR(255) NULL,
    status VARCHAR(8) NOT NULL,
    guest_name VARCHAR(20) NULL,
    created INT NOT NULL,
    last_seen INT NOT NULL,
    INDEX rooms_list (ver, status, last_seen),
    INDEX rooms_host (host_ip)
);

CREATE TABLE IF NOT EXISTS mwr_limits (
    client CHAR(64) NOT NULL,
    bucket VARCHAR(16) NOT NULL,
    window_start INT NOT NULL,
    hits INT NOT NULL,
    PRIMARY KEY (client, bucket)
);
