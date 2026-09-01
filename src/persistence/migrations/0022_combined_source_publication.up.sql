-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE local_source_relocations
    ADD COLUMN metadata_refreshed INTEGER NOT NULL DEFAULT 0
    CHECK(metadata_refreshed IN (0,1));
UPDATE schema_version SET version = 22;
