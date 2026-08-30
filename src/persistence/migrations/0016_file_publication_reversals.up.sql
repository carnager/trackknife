-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE file_publication_journal
    ADD COLUMN reverses_id TEXT REFERENCES file_publication_journal(id);
CREATE INDEX file_publication_journal_reverses
    ON file_publication_journal(reverses_id);
UPDATE schema_version SET version = 16;
