-- SPDX-License-Identifier: GPL-3.0-only

-- Schema 19 permits integer_argument = 1 on remove-field action kinds 2 and
-- 15 to persist exact-native rather than separator-folded logical matching.
-- The same address is retained in an operation journal across a crash.
ALTER TABLE operation_journal_changes ADD COLUMN exact_native_name BLOB;
UPDATE schema_version SET version = 19;
