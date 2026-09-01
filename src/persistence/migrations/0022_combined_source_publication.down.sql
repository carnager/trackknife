-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE local_source_relocations DROP COLUMN metadata_refreshed;
UPDATE schema_version SET version = 21;
