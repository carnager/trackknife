-- SPDX-License-Identifier: GPL-3.0-only

DROP INDEX metadata_operation_backups_state_time;
DROP TABLE metadata_operation_backups;
UPDATE schema_version SET version = 7;
