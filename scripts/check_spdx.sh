#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-3.0-only

set -eu

project_root=${1:-.}

find "$project_root/src" "$project_root/tests" "$project_root/benchmarks" "$project_root/fuzz" \
    "$project_root/cmake" \
    -type f \( -name '*.cpp' -o -name '*.hpp' -o -name 'CMakeLists.txt' -o -name '*.cmake' \) \
    -exec sh -c '
        for source_file do
            if ! head -n 3 "$source_file" | grep -q "SPDX-License-Identifier: GPL-3.0-only"; then
                printf "missing GPL-3.0-only SPDX header: %s\n" "$source_file" >&2
                exit 1
            fi
        done
    ' sh {} +
