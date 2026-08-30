# SPDX-License-Identifier: GPL-3.0-only

qt_add_library(
    trackknife_bench_ui STATIC
    bench_main_window.cpp
    bench_main_window.hpp
    local_list_model.cpp
    local_list_model.hpp
    metadata_dialog_helpers.cpp
    metadata_dialog_helpers.hpp
    metadata_exact_value_dialog.cpp
    metadata_exact_value_dialog.hpp
    metadata_grid_model.cpp
    metadata_grid_model.hpp
    metadata_properties_dialog.cpp
    metadata_properties_dialog.hpp
    metadata_transformation_preview_model.cpp
    metadata_transformation_preview_model.hpp
)
add_library(Trackknife::BenchUi ALIAS trackknife_bench_ui)

target_link_libraries(
    trackknife_bench_ui
    PUBLIC Qt6::Widgets Trackknife::Core Trackknife::UiCommon Trackknife::Persistence
           Trackknife::Formats Trackknife::Metadata Trackknife::Operations
           trackknife_project_options
    PRIVATE Trackknife::Audio Trackknife::MpdUi Trackknife::Ui Qt6::Concurrent
            trackknife_project_warnings
)
target_include_directories(trackknife_bench_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(trackknife_bench_ui PUBLIC cxx_std_23)

qt_add_executable(trackbench main.cpp)
target_link_libraries(
    trackbench
    PRIVATE Trackknife::BenchUi Qt6::Widgets trackknife_project_options
            trackknife_project_warnings
)
set_target_properties(trackbench PROPERTIES OUTPUT_NAME trackbench)
