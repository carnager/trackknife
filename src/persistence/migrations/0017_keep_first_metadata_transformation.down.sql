-- SPDX-License-Identifier: GPL-3.0-only

CREATE TEMP TABLE metadata_keep_first_downgrade_guard (
    value INTEGER NOT NULL CHECK(value = 0)
);
INSERT INTO metadata_keep_first_downgrade_guard
    SELECT COUNT(*) FROM metadata_transformation_actions WHERE kind = 14;
DROP TABLE metadata_keep_first_downgrade_guard;

ALTER TABLE metadata_transformation_action_values
    RENAME TO metadata_transformation_action_values_v17;
ALTER TABLE metadata_transformation_actions
    RENAME TO metadata_transformation_actions_v17;

CREATE TABLE metadata_transformation_actions (
    chain_id TEXT NOT NULL
        REFERENCES metadata_transformation_chains(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 13),
    target_field BLOB NOT NULL,
    argument BLOB,
    dialect BLOB,
    dialect_version INTEGER,
    compiler_schema INTEGER,
    integer_argument INTEGER,
    integer_argument_2 INTEGER,
    PRIMARY KEY(chain_id, position)
);
CREATE TABLE metadata_transformation_action_values (
    chain_id TEXT NOT NULL,
    action_position INTEGER NOT NULL,
    position INTEGER NOT NULL,
    value BLOB NOT NULL,
    PRIMARY KEY(chain_id, action_position, position),
    FOREIGN KEY(chain_id, action_position)
        REFERENCES metadata_transformation_actions(chain_id, position)
        ON DELETE CASCADE
);

INSERT INTO metadata_transformation_actions
    SELECT * FROM metadata_transformation_actions_v17;
INSERT INTO metadata_transformation_action_values
    SELECT * FROM metadata_transformation_action_values_v17;

DROP TABLE metadata_transformation_action_values_v17;
DROP TABLE metadata_transformation_actions_v17;
UPDATE schema_version SET version = 16;
