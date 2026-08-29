# Third-party dependency notes

Trackknife source is GPL-3.0-only. Dependencies retain their own licenses and
must be inventoried before binary distribution.

## Current build and test dependencies

| Dependency | Use | Linking/status | License note |
| --- | --- | --- | --- |
| Qt 6 Core/Gui/Widgets/Concurrent/Test | Product UI, concurrency adapters, and tests | Dynamic system libraries | LGPLv3 option; replacement/relinking obligations apply. |
| utf8proc 2.9+ | Core UTF-8 validation and Unicode mapping | Dynamic system library | MIT plus Unicode data notice; compatible with GPLv3. |
| libmpdclient 2.22+ | MPD command/protocol adapter (M2) | Dynamic system library | BSD-2/3-Clause source notices; compatible with GPLv3. Exact packaged notice is retained when linked. |
| SQLite 3.37+ | Transactional profiles, workspace, and list-document state | Dynamic system library | Public domain; compatible with GPLv3. |
| FFmpeg libavformat 60+, libavcodec 60+, libavutil 58+, libswresample 4+ | M4 local media probe/decode/sample conversion boundary | Dynamic system libraries | FFmpeg is LGPL-2.1-or-later by default and may include GPL components depending on the distribution build; dynamically linked packaged configurations must remain GPLv3-compatible and retain FFmpeg notices. |
| PipeWire 0.3.50+ | M4 direct local audio output and real-time stream callback | Dynamic system library | MIT; compatible with GPLv3. PipeWire and SPA notices remain with their distribution packages. |
| nlohmann/json 3.11+ | Test-only title-format corpus loader | Header-only, test target only | MIT; compatible with GPLv3. |

TagLib and libebur128 are selected in ADR-0006 but are not linked by
the current source targets. Their exact versions, licenses, and transitive
dependency inventory will be recorded when adapters are added.
