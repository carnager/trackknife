-- SPDX-License-Identifier: GPL-3.0-only

DROP INDEX local_source_relocations_source;
DROP TABLE local_source_relocations;
UPDATE schema_version SET version = 14;
