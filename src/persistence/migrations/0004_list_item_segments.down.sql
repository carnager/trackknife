-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE list_items DROP COLUMN segment_end_sample;
ALTER TABLE list_items DROP COLUMN segment_start_sample;
ALTER TABLE list_items DROP COLUMN logical_reference;
UPDATE schema_version SET version = 3;
