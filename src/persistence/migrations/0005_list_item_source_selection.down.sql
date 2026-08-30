-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE list_items DROP COLUMN codec_subsong_index;
ALTER TABLE list_items DROP COLUMN selected_audio_stream;
UPDATE schema_version SET version = 4;
