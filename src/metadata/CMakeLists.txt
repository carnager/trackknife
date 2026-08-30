# SPDX-License-Identifier: GPL-3.0-only

add_library(
    trackknife_metadata STATIC
    src/capture_pattern.cpp
    src/document.cpp
    src/draft_document.cpp
    src/field_suggestions.cpp
    src/flac_mapping.cpp
    src/flac_writer.cpp
    src/local_reader.cpp
    src/rule_script_import.cpp
    src/staged_patch.cpp
    src/staged_selection.cpp
    src/transformation.cpp
    src/write_plan.cpp
)
add_library(Trackknife::Metadata ALIAS trackknife_metadata)

target_include_directories(
    trackknife_metadata
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_metadata
    PUBLIC Trackknife::Core Trackknife::TitleFormat trackknife_project_options
    PRIVATE PkgConfig::TAGLIB trackknife_project_warnings
)
target_compile_features(trackknife_metadata PUBLIC cxx_std_23)
