-- Grouped numbering actions cannot be represented below version 26; their
-- rows are removed before the constraint narrows again.
DELETE FROM metadata_transformation_action_values
    WHERE (chain_id, action_position) IN
        (SELECT chain_id, position FROM metadata_transformation_actions WHERE kind = 17);
DELETE FROM metadata_transformation_actions WHERE kind = 17;
ALTER TABLE metadata_transformation_action_values
    RENAME TO metadata_transformation_action_values_v26;
ALTER TABLE metadata_transformation_actions
    RENAME TO metadata_transformation_actions_v26;
CREATE TABLE metadata_transformation_actions (
    chain_id TEXT NOT NULL REFERENCES metadata_transformation_chains(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 16),
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
        REFERENCES metadata_transformation_actions(chain_id, position) ON DELETE CASCADE
);
INSERT INTO metadata_transformation_actions
    SELECT * FROM metadata_transformation_actions_v26;
INSERT INTO metadata_transformation_action_values
    SELECT * FROM metadata_transformation_action_values_v26;
DROP TABLE metadata_transformation_action_values_v26;
DROP TABLE metadata_transformation_actions_v26;
UPDATE schema_version SET version = 25;
