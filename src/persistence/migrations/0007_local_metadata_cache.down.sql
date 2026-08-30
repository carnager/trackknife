-- SPDX-License-Identifier: GPL-3.0-only

DROP TABLE local_metadata_refreshes;
DROP TABLE local_metadata_cache_fields;
DROP TABLE local_metadata_cache;
ALTER TABLE list_items DROP COLUMN observed_mtime_nanoseconds;
ALTER TABLE list_items DROP COLUMN observed_mtime_seconds;
ALTER TABLE list_items DROP COLUMN observed_size;
ALTER TABLE list_items DROP COLUMN observed_inode;
ALTER TABLE list_items DROP COLUMN observed_device;
ALTER TABLE list_item_fields DROP COLUMN description;
ALTER TABLE list_item_fields DROP COLUMN language;
ALTER TABLE list_item_fields DROP COLUMN provenance;
ALTER TABLE list_item_fields DROP COLUMN native_name;
UPDATE schema_version SET version = 6;
