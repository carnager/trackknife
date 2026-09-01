-- SPDX-License-Identifier: GPL-3.0-only

DROP TABLE operation_journal_artwork;
ALTER TABLE operation_journal DROP COLUMN content_kind;
UPDATE schema_version SET version = 22;
