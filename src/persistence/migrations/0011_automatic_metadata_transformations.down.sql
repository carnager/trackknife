-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE metadata_transformation_chains DROP COLUMN automatic;
UPDATE schema_version SET version = 10;
