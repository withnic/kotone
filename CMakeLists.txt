cmake_minimum_required(VERSION 3.20)

project(kotone
    VERSION 1.0.0
    DESCRIPTION "Kotone (琴音) OBS Studio Audio Filter Plugin and Core DSP Engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KOTONE_BUILD_OBS_PLUGIN "Build OBS Studio plugin shared library (requires libobs)" OFF)
option(KOTONE_BUILD_TESTS "Build C++ unit tests" ON)

find_package(Threads REQUIRED)

# Pure C++17 zero-dependency DSP core and profile repository static library
add_library(kotone_dsp STATIC
    plugin/src/dsp/dsp_types.cpp
    plugin/src/dsp/biquad.cpp
    plugin/src/dsp/dynamics.cpp
    plugin/src/dsp/voice_cleaner.cpp
    plugin/src/dsp/dsp_engine.cpp
    plugin/src/profile/profile_loader.cpp
)

target_include_directories(kotone_dsp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/plugin/include>
)

target_compile_features(kotone_dsp PUBLIC cxx_std_17)
set_target_properties(kotone_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(kotone_dsp PRIVATE /W4 /utf-8 /permissive-)
else()
    target_compile_options(kotone_dsp PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(kotone_dsp PUBLIC Threads::Threads)

# Unit tests & CLI profile verifier
if(KOTONE_BUILD_TESTS)
    enable_testing()

    add_executable(kotone_tests
        plugin/tests/test_main.cpp
        plugin/tests/test_dsp_limiter.cpp
        plugin/tests/test_dsp_eq_comp.cpp
        plugin/tests/test_profile_loader.cpp
        plugin/tests/test_voice_cleaner.cpp
    )

    target_link_libraries(kotone_tests PRIVATE kotone_dsp Threads::Threads)

    if(MSVC)
        target_compile_options(kotone_tests PRIVATE /W4 /utf-8 /permissive-)
    else()
        target_compile_options(kotone_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME kotone_unit_tests COMMAND kotone_tests)
endif()

# OBS Studio plugin module (requires OBS Studio 30.x / 31.x libobs SDK)
if(KOTONE_BUILD_OBS_PLUGIN)
    find_package(libobs QUIET)
    if(NOT libobs_FOUND AND NOT TARGET OBS::libobs)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBOBS_PKG REQUIRED IMPORTED_TARGET libobs)
        add_library(OBS::libobs ALIAS PkgConfig::LIBOBS_PKG)
    endif()

    add_library(kotone MODULE
        plugin/src/obs/plugin_main.cpp
        plugin/src/obs/kotone_filter.cpp
    )

    target_link_libraries(kotone PRIVATE kotone_dsp OBS::libobs)

    set_target_properties(kotone PROPERTIES
        PREFIX ""
        OUTPUT_NAME "kotone"
    )

    if(MSVC)
        target_compile_options(kotone PRIVATE /W4 /utf-8 /permissive-)
    else()
        target_compile_options(kotone PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # Copy locale files alongside build output for local testing
    add_custom_command(TARGET kotone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/data"
            "$<TARGET_FILE_DIR:kotone>/data"
        COMMENT "Copying Kotone locale resources to build output"
    )

    # Install targets for OBS Studio plugin layout
    if(WIN32)
        install(TARGETS kotone
            LIBRARY DESTINATION "obs-plugins/64bit"
            RUNTIME DESTINATION "obs-plugins/64bit"
        )
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/plugin/data/"
            DESTINATION "data/obs-plugins/kotone"
        )
    else()
        include(GNUInstallDirs)
        install(TARGETS kotone
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
        )
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/plugin/data/"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/kotone"
        )
    endif()
endif()
