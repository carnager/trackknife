-- SPDX-License-Identifier: GPL-3.0-only

CREATE TABLE operation_journal (
    id TEXT PRIMARY KEY NOT NULL,
    kind INTEGER NOT NULL CHECK(kind = 0),
    state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 5),
    source_path BLOB NOT NULL,
    prepared_path BLOB NOT NULL,
    backup_path BLOB NOT NULL,
    expected_device BLOB NOT NULL,
    expected_inode BLOB NOT NULL,
    expected_size BLOB NOT NULL,
    expected_mtime_seconds BLOB NOT NULL,
    expected_mtime_nanoseconds BLOB NOT NULL,
    prepared_device BLOB,
    prepared_inode BLOB,
    prepared_size BLOB,
    prepared_mtime_seconds BLOB,
    prepared_mtime_nanoseconds BLOB,
    published_device BLOB,
    published_inode BLOB,
    published_size BLOB,
    published_mtime_seconds BLOB,
    published_mtime_nanoseconds BLOB,
    error_code INTEGER,
    error_message BLOB
);
CREATE INDEX operation_journal_state ON operation_journal(state);
CREATE TABLE operation_journal_occurrences (
    journal_id TEXT NOT NULL REFERENCES operation_journal(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    item_index INTEGER NOT NULL,
    PRIMARY KEY(journal_id, position)
);
CREATE TABLE operation_journal_changes (
    journal_id TEXT NOT NULL REFERENCES operation_journal(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    field_index INTEGER NOT NULL,
    canonical_name BLOB NOT NULL,
    property_name BLOB NOT NULL,
    original_present INTEGER NOT NULL CHECK(original_present IN (0, 1)),
    patch_kind INTEGER NOT NULL CHECK(patch_kind BETWEEN 0 AND 1),
    PRIMARY KEY(journal_id, position)
);
CREATE TABLE operation_journal_values (
    journal_id TEXT NOT NULL,
    change_position INTEGER NOT NULL,
    value_kind INTEGER NOT NULL CHECK(value_kind BETWEEN 0 AND 1),
    position INTEGER NOT NULL,
    value BLOB NOT NULL,
    PRIMARY KEY(journal_id, change_position, value_kind, position),
    FOREIGN KEY(journal_id, change_position)
        REFERENCES operation_journal_changes(journal_id, position) ON DELETE CASCADE
);
CREATE TABLE operation_journal_intents (
    journal_id TEXT NOT NULL,
    change_position INTEGER NOT NULL,
    position INTEGER NOT NULL,
    item_index INTEGER NOT NULL,
    PRIMARY KEY(journal_id, change_position, position),
    FOREIGN KEY(journal_id, change_position)
        REFERENCES operation_journal_changes(journal_id, position) ON DELETE CASCADE
);
UPDATE schema_version SET version = 6;
