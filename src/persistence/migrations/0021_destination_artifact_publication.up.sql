-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE file_publication_journal
    ADD COLUMN content_kind INTEGER NOT NULL DEFAULT 0
    CHECK(content_kind BETWEEN 0 AND 1);
UPDATE schema_version SET version = 21;
