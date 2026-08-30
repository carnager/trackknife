# SPDX-License-Identifier: GPL-3.0-only

option(TRACKKNIFE_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)
option(TRACKKNIFE_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)

function(trackknife_enable_sanitizers target)
    if(NOT TRACKKNIFE_ENABLE_SANITIZERS AND NOT TRACKKNIFE_ENABLE_THREAD_SANITIZER)
        return()
    endif()

    if(MSVC)
        message(FATAL_ERROR "The Trackknife sanitizer presets currently support GCC and Clang")
    endif()

    if(TRACKKNIFE_ENABLE_SANITIZERS AND TRACKKNIFE_ENABLE_THREAD_SANITIZER)
        message(FATAL_ERROR "AddressSanitizer/UBSan and ThreadSanitizer cannot be enabled together")
    endif()

    if(TRACKKNIFE_ENABLE_THREAD_SANITIZER)
        target_compile_definitions(${target} INTERFACE TRACKKNIFE_THREAD_SANITIZER=1)
        target_compile_options(${target} INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    else()
        target_compile_options(${target} INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
endfunction()
