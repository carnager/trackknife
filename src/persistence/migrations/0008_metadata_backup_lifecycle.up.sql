-- SPDX-License-Identifier: GPL-3.0-only

CREATE TABLE metadata_operation_backups (
    journal_id TEXT PRIMARY KEY NOT NULL
        REFERENCES operation_journal(id) ON DELETE CASCADE,
    state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),
    undo_id TEXT,
    completed_at_unix_seconds INTEGER NOT NULL,
    updated_at_unix_seconds INTEGER NOT NULL,
    error_code INTEGER,
    error_message BLOB
);
CREATE INDEX metadata_operation_backups_state_time
    ON metadata_operation_backups(state, completed_at_unix_seconds DESC);
INSERT INTO metadata_operation_backups(
    journal_id, state, undo_id, completed_at_unix_seconds,
    updated_at_unix_seconds, error_code, error_message)
SELECT id, 0, NULL, CAST(strftime('%s', 'now') AS INTEGER),
       CAST(strftime('%s', 'now') AS INTEGER), NULL, NULL
FROM operation_journal
WHERE state = 3;
UPDATE schema_version SET version = 8;
