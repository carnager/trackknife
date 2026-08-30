-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE metadata_transformation_chains
    ADD COLUMN automatic INTEGER NOT NULL DEFAULT 0
    CHECK(automatic IN (0,1));
UPDATE schema_version SET version = 11;
