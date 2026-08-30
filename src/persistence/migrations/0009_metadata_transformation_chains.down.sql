-- SPDX-License-Identifier: GPL-3.0-only

DROP TABLE metadata_transformation_action_values;
DROP TABLE metadata_transformation_actions;
DROP TABLE metadata_transformation_chains;
UPDATE schema_version SET version = 8;
