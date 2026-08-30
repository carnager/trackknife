-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE metadata_transformation_action_values
    RENAME TO metadata_transformation_action_values_v9;
ALTER TABLE metadata_transformation_actions
    RENAME TO metadata_transformation_actions_v9;

CREATE TABLE metadata_transformation_actions (
    chain_id TEXT NOT NULL
        REFERENCES metadata_transformation_chains(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 10),
    target_field BLOB NOT NULL,
    argument BLOB,
    dialect BLOB,
    dialect_version INTEGER,
    compiler_schema INTEGER,
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
    SELECT * FROM metadata_transformation_actions_v9;
INSERT INTO metadata_transformation_action_values
    SELECT * FROM metadata_transformation_action_values_v9;

DROP TABLE metadata_transformation_action_values_v9;
DROP TABLE metadata_transformation_actions_v9;
UPDATE schema_version SET version = 10;
