# SPDX-License-Identifier: GPL-3.0-only

find_package(Threads REQUIRED)

add_library(
    trackknife_operations STATIC
    src/file_publication.cpp
    src/file_publication_journal.cpp
    src/metadata_apply.cpp
    src/metadata_commit.cpp
    src/output_path_plan.cpp
    src/output_path_preflight.cpp
)
add_library(Trackknife::Operations ALIAS trackknife_operations)

target_include_directories(
    trackknife_operations
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_operations
    PUBLIC Trackknife::Core Trackknife::Metadata Threads::Threads trackknife_project_options
    PRIVATE trackknife_project_warnings
)
target_compile_features(trackknife_operations PUBLIC cxx_std_23)
