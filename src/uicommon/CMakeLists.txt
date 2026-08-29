# SPDX-License-Identifier: GPL-3.0-only

qt_add_library(
    trackknife_ui_common STATIC
    command_palette.cpp
    command_palette.hpp
    list_persistence_service.cpp
    list_persistence_service.hpp
    local_folder_tree_model.cpp
    local_folder_tree_model.hpp
)
add_library(Trackknife::UiCommon ALIAS trackknife_ui_common)

target_link_libraries(
    trackknife_ui_common
    PUBLIC Qt6::Widgets Trackknife::Core Trackknife::Persistence
           trackknife_project_options
    PRIVATE Qt6::Concurrent trackknife_project_warnings
)
target_include_directories(trackknife_ui_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(trackknife_ui_common PUBLIC cxx_std_23)
