<?php
// Copy to "mawarunco-lobby-config.php" one folder ABOVE api.php (outside the web folder), or, if the
// hosting does not allow that, to "config.php" next to api.php (.htaccess blocks access to it).
// Never commit the filled-in copy.
return [
    // MariaDB/MySQL database of the hosting package (tables from schema.sql, prefix mwr_). Host: the
    // database server shown in the hosting's database overview. Use the read/write login, not the main one.
    'dsn' => 'mysql:host=DATABASE_SERVER;dbname=DATABASE_NAME;charset=utf8mb4',
    'user' => 'DATABASE_RW_USER',
    'password' => 'DATABASE_RW_PASSWORD',
    // Random secret for the rate-limit hashes of client IPs, e.g. from
    //   php -r "echo bin2hex(random_bytes(32));"
    'salt' => 'REPLACE_WITH_64_RANDOM_HEX_CHARACTERS',
    // Local tests only: accept hosts with private addresses (127.0.0.1, 192.168.x.x). Keep false online.
    'allow_private_hosts' => false,
];
