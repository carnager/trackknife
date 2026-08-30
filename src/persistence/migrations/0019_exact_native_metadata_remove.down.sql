-- SPDX-License-Identifier: GPL-3.0-only

CREATE TEMP TABLE metadata_exact_remove_downgrade_guard (
    value INTEGER NOT NULL CHECK(value = 0)
);
INSERT INTO metadata_exact_remove_downgrade_guard
    SELECT
        (SELECT COUNT(*) FROM metadata_transformation_actions
         WHERE kind IN (2, 15) AND integer_argument = 1) +
        (SELECT COUNT(*) FROM operation_journal_changes
         WHERE exact_native_name IS NOT NULL);
DROP TABLE metadata_exact_remove_downgrade_guard;

ALTER TABLE operation_journal_changes DROP COLUMN exact_native_name;
UPDATE schema_version SET version = 18;
