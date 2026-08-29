-- SPDX-License-Identifier: GPL-3.0-only

DROP INDEX IF EXISTS list_item_fields_item;
DROP TABLE IF EXISTS list_item_fields;
DROP TABLE IF EXISTS list_items;
DROP TABLE IF EXISTS list_documents;
UPDATE schema_version SET version = 0;
