-- SPDX-License-Identifier: GPL-3.0-only

CREATE TABLE file_publication_journal (
    id TEXT PRIMARY KEY NOT NULL,
    state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 7),
    publication_kind INTEGER NOT NULL CHECK(publication_kind BETWEEN 1 AND 2),
    source_path BLOB NOT NULL,
    target_path BLOB NOT NULL,
    prepared_path BLOB NOT NULL,
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
    target_device BLOB,
    target_inode BLOB,
    target_size BLOB,
    target_mtime_seconds BLOB,
    target_mtime_nanoseconds BLOB,
    error_code INTEGER,
    error_message BLOB
);
CREATE INDEX file_publication_journal_state
    ON file_publication_journal(state);
CREATE TABLE file_publication_journal_occurrences (
    journal_id TEXT NOT NULL
        REFERENCES file_publication_journal(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    item_index INTEGER NOT NULL,
    PRIMARY KEY(journal_id, position)
);
CREATE TABLE file_publication_journal_directories (
    journal_id TEXT NOT NULL
        REFERENCES file_publication_journal(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    raw_path BLOB NOT NULL,
    PRIMARY KEY(journal_id, position)
);
UPDATE schema_version SET version = 14;
