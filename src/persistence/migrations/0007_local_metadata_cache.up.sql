-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE list_item_fields ADD COLUMN native_name BLOB NOT NULL DEFAULT X'';
ALTER TABLE list_item_fields ADD COLUMN provenance INTEGER NOT NULL DEFAULT 0
    CHECK(provenance BETWEEN 0 AND 5);
ALTER TABLE list_item_fields ADD COLUMN language BLOB;
ALTER TABLE list_item_fields ADD COLUMN description BLOB;
ALTER TABLE list_items ADD COLUMN observed_device BLOB;
ALTER TABLE list_items ADD COLUMN observed_inode BLOB;
ALTER TABLE list_items ADD COLUMN observed_size BLOB;
ALTER TABLE list_items ADD COLUMN observed_mtime_seconds BLOB;
ALTER TABLE list_items ADD COLUMN observed_mtime_nanoseconds BLOB;

CREATE TABLE local_metadata_cache (
    source_reference BLOB PRIMARY KEY NOT NULL,
    previous_device BLOB NOT NULL,
    previous_inode BLOB NOT NULL,
    previous_size BLOB NOT NULL,
    previous_mtime_seconds BLOB NOT NULL,
    previous_mtime_nanoseconds BLOB NOT NULL,
    published_device BLOB NOT NULL,
    published_inode BLOB NOT NULL,
    published_size BLOB NOT NULL,
    published_mtime_seconds BLOB NOT NULL,
    published_mtime_nanoseconds BLOB NOT NULL
);
CREATE TABLE local_metadata_cache_fields (
    source_reference BLOB NOT NULL REFERENCES local_metadata_cache(source_reference)
        ON DELETE CASCADE,
    position INTEGER NOT NULL,
    name BLOB NOT NULL,
    value BLOB NOT NULL,
    native_name BLOB NOT NULL,
    provenance INTEGER NOT NULL CHECK(provenance BETWEEN 0 AND 5),
    language BLOB,
    description BLOB,
    PRIMARY KEY(source_reference, position)
);
CREATE TABLE local_metadata_refreshes (
    operation_id TEXT PRIMARY KEY NOT NULL,
    source_reference BLOB NOT NULL,
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
    affected_occurrences INTEGER NOT NULL CHECK(affected_occurrences > 0)
);
UPDATE schema_version SET version = 7;
