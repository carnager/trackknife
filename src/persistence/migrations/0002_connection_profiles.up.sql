-- SPDX-License-Identifier: GPL-3.0-only

CREATE TABLE connection_profiles (
    id TEXT PRIMARY KEY NOT NULL,
    name BLOB NOT NULL,
    host BLOB NOT NULL,
    port INTEGER NOT NULL CHECK(port BETWEEN 1 AND 65535),
    local_music_root BLOB,
    auto_connect INTEGER NOT NULL CHECK(auto_connect IN (0,1)),
    position INTEGER NOT NULL UNIQUE
);
UPDATE schema_version SET version = 2;
