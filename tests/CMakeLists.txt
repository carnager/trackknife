# SPDX-License-Identifier: GPL-3.0-only

find_package(Threads REQUIRED)

add_executable(trackknife_core_tests core_primitives_test.cpp)
target_link_libraries(
    trackknife_core_tests
    PRIVATE Trackknife::Core trackknife_project_options trackknife_project_warnings
)
add_test(NAME core-primitives COMMAND trackknife_core_tests)

add_executable(trackknife_list_repository_tests list_repository_test.cpp)
target_link_libraries(
    trackknife_list_repository_tests
    PRIVATE Trackknife::Persistence trackknife_project_options trackknife_project_warnings
)
add_test(NAME list-repository COMMAND trackknife_list_repository_tests)

add_executable(trackknife_mpd_domain_tests mpd_domain_test.cpp)
target_link_libraries(
    trackknife_mpd_domain_tests
    PRIVATE Trackknife::Mpd trackknife_project_options trackknife_project_warnings
)
add_test(NAME mpd-domain COMMAND trackknife_mpd_domain_tests)

add_executable(trackknife_mpd_client_tests mpd_client_test.cpp)
target_link_libraries(
    trackknife_mpd_client_tests
    PRIVATE Trackknife::Mpd trackknife_project_options trackknife_project_warnings
)
add_test(NAME mpd-client COMMAND trackknife_mpd_client_tests)

add_executable(trackknife_mpd_session_tests mpd_session_test.cpp)
target_link_libraries(
    trackknife_mpd_session_tests
    PRIVATE Trackknife::Mpd trackknife_project_options trackknife_project_warnings
)
add_test(NAME mpd-session COMMAND trackknife_mpd_session_tests)

add_executable(trackknife_formats_probe_tests formats_probe_test.cpp)
target_link_libraries(
    trackknife_formats_probe_tests
    PRIVATE Trackknife::Formats trackknife_project_options trackknife_project_warnings
)
add_test(
    NAME formats-probe
    COMMAND trackknife_formats_probe_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_local_playback_tests local_playback_test.cpp)
target_link_libraries(
    trackknife_local_playback_tests
    PRIVATE Trackknife::Audio Threads::Threads trackknife_project_options
            trackknife_project_warnings
)
add_test(NAME local-playback COMMAND trackknife_local_playback_tests)

add_executable(trackknife_local_audition_tests local_audition_test.cpp)
target_link_libraries(
    trackknife_local_audition_tests
    PRIVATE Trackknife::Audio Threads::Threads trackknife_project_options
            trackknife_project_warnings
)
add_test(NAME local-audition COMMAND trackknife_local_audition_tests)
set_tests_properties(
    local-audition
    PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 15
        ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
)

add_executable(trackknife_pipewire_output_tests pipewire_output_test.cpp)
target_link_libraries(
    trackknife_pipewire_output_tests
    PRIVATE Trackknife::Audio Threads::Threads trackknife_project_options
            trackknife_project_warnings
)
add_test(NAME pipewire-output COMMAND trackknife_pipewire_output_tests)
set_tests_properties(
    pipewire-output
    PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 10
        ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
)

add_executable(trackknife_titleformat_parser_tests titleformat_parser_test.cpp)
target_link_libraries(
    trackknife_titleformat_parser_tests
    PRIVATE Trackknife::TitleFormat trackknife_project_options trackknife_project_warnings
)
add_test(NAME titleformat-parser COMMAND trackknife_titleformat_parser_tests)

add_executable(trackknife_titleformat_compiler_tests titleformat_compiler_test.cpp)
target_link_libraries(
    trackknife_titleformat_compiler_tests
    PRIVATE Trackknife::TitleFormat trackknife_project_options trackknife_project_warnings
)
add_test(NAME titleformat-compiler COMMAND trackknife_titleformat_compiler_tests)

add_executable(trackknife_titleformat_evaluator_tests titleformat_evaluator_test.cpp)
target_link_libraries(
    trackknife_titleformat_evaluator_tests
    PRIVATE Trackknife::TitleFormat trackknife_project_options trackknife_project_warnings
)
add_test(NAME titleformat-evaluator COMMAND trackknife_titleformat_evaluator_tests)

find_package(nlohmann_json 3.11 CONFIG REQUIRED)
add_executable(trackknife_titleformat_corpus_runner titleformat_corpus_runner.cpp)
target_link_libraries(
    trackknife_titleformat_corpus_runner
    PRIVATE Trackknife::TitleFormat nlohmann_json::nlohmann_json
            trackknife_project_options trackknife_project_warnings
)
add_test(
    NAME titleformat-corpus
    COMMAND trackknife_titleformat_corpus_runner
            "${CMAKE_CURRENT_SOURCE_DIR}/titleformat/tkfmt-corpus"
)

add_test(
    NAME titleformat-cli-smoke
    COMMAND trackknife_titleformat_cli --field artist=Trackknife --show-dependencies
            "%artist%"
)

add_test(
    NAME titleformat-cli-metadata-smoke
    COMMAND trackknife_titleformat_cli --field artist=One --field artist=Two --info channels=2
            "$join(artist, + )|$info(channels)"
)

if(TRACKKNIFE_BUILD_UI)
    qt_add_executable(trackknife_paged_model_tests paged_track_model_test.cpp)
    target_link_libraries(
        trackknife_paged_model_tests
        PRIVATE Trackknife::QtModels Qt6::Test trackknife_project_options trackknife_project_warnings
    )
    add_test(NAME paged-track-model COMMAND trackknife_paged_model_tests)
    set_tests_properties(paged-track-model PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(trackknife_mpd_queue_model_tests mpd_queue_model_test.cpp)
    target_link_libraries(
        trackknife_mpd_queue_model_tests
        PRIVATE Trackknife::MpdUi Qt6::Test trackknife_project_options trackknife_project_warnings
    )
    add_test(NAME mpd-queue-model COMMAND trackknife_mpd_queue_model_tests)

    qt_add_executable(trackknife_main_window_tests main_window_test.cpp)
    target_link_libraries(
        trackknife_main_window_tests
        PRIVATE Trackknife::Ui Qt6::Test trackknife_project_options trackknife_project_warnings
    )
    add_test(NAME main-window-workspace COMMAND trackknife_main_window_tests)
    set_tests_properties(main-window-workspace PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(trackknife_local_folder_tree_model_tests local_folder_tree_model_test.cpp)
    target_link_libraries(
        trackknife_local_folder_tree_model_tests
        PRIVATE Trackknife::Ui Qt6::Test trackknife_project_options trackknife_project_warnings
    )
    add_test(NAME local-folder-tree-model COMMAND trackknife_local_folder_tree_model_tests)
    set_tests_properties(local-folder-tree-model PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(trackknife_server_library_tree_model_tests server_library_tree_model_test.cpp)
    target_link_libraries(
        trackknife_server_library_tree_model_tests
        PRIVATE Trackknife::Ui Qt6::Test trackknife_project_options trackknife_project_warnings
    )
    add_test(NAME server-library-tree-model COMMAND trackknife_server_library_tree_model_tests)
    set_tests_properties(server-library-tree-model PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_test(NAME ui-smoke COMMAND trackknife --smoke-test --rows 1000000)
    set_tests_properties(
        ui-smoke
        PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
    )
endif()

add_test(
    NAME spdx-headers
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check_spdx.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
