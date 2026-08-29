# SPDX-License-Identifier: GPL-3.0-only

qt_add_library(
    trackknife_ui STATIC
    format_sandbox.cpp
    format_sandbox.hpp
    library_tree_editor_dialog.cpp
    library_tree_editor_dialog.hpp
    main_window.cpp
    main_window.hpp
    mpd_connection_dialog.cpp
    mpd_connection_dialog.hpp
    server_library_tree_model.cpp
    server_library_tree_model.hpp
)
add_library(Trackknife::Ui ALIAS trackknife_ui)

target_link_libraries(
    trackknife_ui
    PUBLIC Qt6::Widgets Trackknife::Core Trackknife::QtModels Trackknife::MpdUi
           Trackknife::UiCommon Trackknife::Persistence trackknife_project_options
    PRIVATE Trackknife::TitleFormat Trackknife::Mpd
    PRIVATE Qt6::Concurrent trackknife_project_warnings
)
target_include_directories(trackknife_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(trackknife_ui PUBLIC cxx_std_23)
