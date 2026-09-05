# SPDX-License-Identifier: GPL-3.0-only

qt_add_library(
    trackknife_bench_ui STATIC
    bench_main_window.cpp
    bench_main_window.hpp
    bench_main_window_helpers.cpp
    bench_main_window_helpers.hpp
    bench_list_tabs.cpp
    bench_media_ingest.cpp
    bench_metadata_operations.cpp
    bench_mpd.cpp
    bench_track_views.cpp
    bench_transport.cpp
    bench_workspace_layout.cpp
    convert_dialog.cpp
    convert_dialog.hpp
    local_list_model.cpp
    local_list_model.hpp
    metadata_artwork_section.cpp
    metadata_artwork_section.hpp
    metadata_dialog_helpers.cpp
    metadata_dialog_helpers.hpp
    metadata_exact_value_dialog.cpp
    metadata_exact_value_dialog.hpp
    metadata_grid_model.cpp
    metadata_grid_model.hpp
    metadata_properties_dialog.cpp
    metadata_properties_dialog.hpp
    metadata_rule_script_import_dialog.cpp
    metadata_rule_script_import_dialog.hpp
    metadata_scalar_delegate.cpp
    metadata_scalar_delegate.hpp
    settings_dialog.cpp
    settings_dialog.hpp
    metadata_transformation_dialog.cpp
    metadata_transformation_dialog.hpp
    metadata_transformation_preview_model.cpp
    musicbrainz_fetch_service.cpp
    musicbrainz_fetch_service.hpp
    musicbrainz_identify_dialog.cpp
    musicbrainz_identify_dialog.hpp
    metadata_transformation_preview_model.hpp
    preparation_feedback_dialog.cpp
    preparation_feedback_dialog.hpp
)
add_library(Trackknife::BenchUi ALIAS trackknife_bench_ui)

target_link_libraries(
    trackknife_bench_ui
    PUBLIC Qt6::Widgets Trackknife::Core Trackknife::UiCommon Trackknife::Persistence
           Trackknife::Convert Trackknife::Formats Trackknife::Loudness Trackknife::Metadata Trackknife::MusicBrainz
           Trackknife::Operations
           trackknife_project_options
    PRIVATE Trackknife::Audio Trackknife::MpdUi Trackknife::Ui Qt6::Concurrent Qt6::Network
            trackknife_project_warnings
)
target_include_directories(trackknife_bench_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(trackknife_bench_ui PUBLIC cxx_std_23)

qt_add_executable(trackknife main.cpp)
target_link_libraries(
    trackknife
    PRIVATE Trackknife::BenchUi Qt6::Widgets trackknife_project_options
            trackknife_project_warnings
)
set_target_properties(trackknife PROPERTIES OUTPUT_NAME trackknife)
