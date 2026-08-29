# SPDX-License-Identifier: GPL-3.0-only

option(TRACKKNIFE_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)

function(trackknife_enable_sanitizers target)
    if(NOT TRACKKNIFE_ENABLE_SANITIZERS)
        return()
    endif()

    if(MSVC)
        message(FATAL_ERROR "The Trackknife sanitizer preset currently supports GCC and Clang")
    endif()

    target_compile_options(${target} INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
endfunction()
