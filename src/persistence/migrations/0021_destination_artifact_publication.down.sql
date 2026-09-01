-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE file_publication_journal DROP COLUMN content_kind;
UPDATE schema_version SET version = 20;
