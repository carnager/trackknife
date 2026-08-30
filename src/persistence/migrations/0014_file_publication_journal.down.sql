-- SPDX-License-Identifier: GPL-3.0-only

DROP TABLE file_publication_journal_directories;
DROP TABLE file_publication_journal_occurrences;
DROP INDEX file_publication_journal_state;
DROP TABLE file_publication_journal;
UPDATE schema_version SET version = 13;
