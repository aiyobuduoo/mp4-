cmake_minimum_required(VERSION 3.16)

project(basic_player LANGUAGES CXX)

# PortAudio (required for audio output)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)

add_executable(basic_player
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/portaudio_sink.cpp
)

target_include_directories(basic_player PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

target_compile_features(basic_player PRIVATE cxx_std_17)

target_link_libraries(basic_player PRIVATE
    micro_decoder
    PkgConfig::PORTAUDIO
)

if(ENABLE_SANITIZERS)
    target_compile_options(basic_player PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(basic_player PRIVATE -fsanitize=address,undefined)
endif()
