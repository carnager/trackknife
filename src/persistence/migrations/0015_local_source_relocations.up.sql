-- SPDX-License-Identifier: GPL-3.0-only

CREATE TABLE local_source_relocations (
    sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    operation_id TEXT NOT NULL UNIQUE,
    source_reference BLOB NOT NULL,
    target_reference BLOB NOT NULL,
    previous_device BLOB NOT NULL,
    previous_inode BLOB NOT NULL,
    previous_size BLOB NOT NULL,
    previous_mtime_seconds BLOB NOT NULL,
    previous_mtime_nanoseconds BLOB NOT NULL,
    published_device BLOB NOT NULL,
    published_inode BLOB NOT NULL,
    published_size BLOB NOT NULL,
    published_mtime_seconds BLOB NOT NULL,
    published_mtime_nanoseconds BLOB NOT NULL,
    affected_occurrences INTEGER NOT NULL CHECK(affected_occurrences > 0),
    cache_rekeyed INTEGER NOT NULL CHECK(cache_rekeyed IN (0,1)),
    CHECK(source_reference != target_reference)
);
CREATE INDEX local_source_relocations_source
    ON local_source_relocations(source_reference, sequence);
UPDATE schema_version SET version = 15;
