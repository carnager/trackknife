-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE list_items ADD COLUMN logical_reference BLOB;
ALTER TABLE list_items ADD COLUMN segment_start_sample INTEGER;
ALTER TABLE list_items ADD COLUMN segment_end_sample INTEGER;
UPDATE schema_version SET version = 4;
