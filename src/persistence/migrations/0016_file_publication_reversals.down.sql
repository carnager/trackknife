-- SPDX-License-Identifier: GPL-3.0-only

DROP INDEX file_publication_journal_reverses;
ALTER TABLE file_publication_journal DROP COLUMN reverses_id;
UPDATE schema_version SET version = 15;
