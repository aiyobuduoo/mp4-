# Host platform configuration for micro-decoder

include(FetchContent)

function(micro_decoder_configure_host TARGET_LIB SOURCE_DIR)
    # =========================================================================
    # Include paths
    # =========================================================================
    target_include_directories(${TARGET_LIB} PUBLIC ${SOURCE_DIR}/include)
    target_include_directories(${TARGET_LIB} PRIVATE ${SOURCE_DIR}/src)

    # =========================================================================
    # Host sources (HTTP client via libcurl)
    # =========================================================================
    target_sources(${TARGET_LIB} PRIVATE ${MICRO_DECODER_HOST_SOURCES})

    # =========================================================================
    # Compiler settings
    # =========================================================================
    target_compile_features(${TARGET_LIB} PUBLIC cxx_std_17)
    target_compile_options(${TARGET_LIB} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${TARGET_LIB} PRIVATE -Wno-gnu-zero-variadic-macro-arguments)
    endif()
    if(ENABLE_WERROR)
        target_compile_options(${TARGET_LIB} PRIVATE -Werror)
    endif()
    if(ENABLE_SANITIZERS)
        target_compile_options(${TARGET_LIB} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${TARGET_LIB} PRIVATE -fsanitize=address,undefined)
    endif()

    # =========================================================================
    # Codec options
    # Declared here (inside the function) rather than at file scope because the
    # ESP-IDF build configures codecs via Kconfig, not CMake options. Keeping
    # them inside the host-only function avoids exposing unused options in the
    # ESP build path. Cache variables are global regardless of function scope,
    # so -D overrides from the command line work as expected.
    # =========================================================================
    option(MICRO_DECODER_CODEC_FLAC "Enable FLAC codec" ON)
    option(MICRO_DECODER_CODEC_MP3  "Enable MP3 codec"  ON)
    option(MICRO_DECODER_CODEC_OPUS "Enable Opus codec" ON)
    option(MICRO_DECODER_CODEC_WAV  "Enable WAV codec"  ON)

    if(MICRO_DECODER_CODEC_FLAC)
        target_compile_definitions(${TARGET_LIB} PUBLIC MICRO_DECODER_CODEC_FLAC=1)
    endif()
    if(MICRO_DECODER_CODEC_MP3)
        target_compile_definitions(${TARGET_LIB} PUBLIC MICRO_DECODER_CODEC_MP3=1)
    endif()
    if(MICRO_DECODER_CODEC_OPUS)
        target_compile_definitions(${TARGET_LIB} PUBLIC MICRO_DECODER_CODEC_OPUS=1)
    endif()
    if(MICRO_DECODER_CODEC_WAV)
        target_compile_definitions(${TARGET_LIB} PUBLIC MICRO_DECODER_CODEC_WAV=1)
    endif()

    # =========================================================================
    # External dependencies
    # =========================================================================

    # micro-flac
    if(MICRO_DECODER_CODEC_FLAC)
        # GIT_SHALLOW omitted: micro-flac uses submodules, which are unreliable with shallow clones
        FetchContent_Declare(
            micro_flac
            GIT_REPOSITORY https://github.com/esphome-libs/micro-flac.git
            GIT_TAG        v0.1.1
            GIT_SUBMODULES "lib/micro-ogg-demuxer"
        )
        FetchContent_MakeAvailable(micro_flac)
        target_link_libraries(${TARGET_LIB} PRIVATE micro_flac)
    endif()

    # micro-mp3
    if(MICRO_DECODER_CODEC_MP3)
        FetchContent_Declare(
            micro_mp3
            GIT_REPOSITORY https://github.com/esphome-libs/micro-mp3.git
            GIT_TAG        v0.1.0
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(micro_mp3)
        target_link_libraries(${TARGET_LIB} PRIVATE micro_mp3)
    endif()

    # micro-opus
    if(MICRO_DECODER_CODEC_OPUS)
        # GIT_SHALLOW omitted: micro-opus uses submodules, which are unreliable with shallow clones
        FetchContent_Declare(
            micro_opus
            GIT_REPOSITORY https://github.com/esphome-libs/micro-opus.git
            GIT_TAG        v0.3.5
            GIT_SUBMODULES "lib/opus" "lib/micro-ogg-demuxer"
        )
        FetchContent_MakeAvailable(micro_opus)
        target_link_libraries(${TARGET_LIB} PRIVATE micro_opus)
    endif()

    # micro-wav
    if(MICRO_DECODER_CODEC_WAV)
        FetchContent_Declare(
            micro_wav
            GIT_REPOSITORY https://github.com/esphome-libs/micro-wav.git
            GIT_TAG        v0.1.0
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(micro_wav)
        target_link_libraries(${TARGET_LIB} PRIVATE micro_wav)
    endif()

    # libcurl (HTTP streaming)
    find_package(CURL REQUIRED)
    target_link_libraries(${TARGET_LIB} PRIVATE CURL::libcurl)

    # Threading support
    find_package(Threads REQUIRED)
    target_link_libraries(${TARGET_LIB} PRIVATE Threads::Threads)

    # =========================================================================
    # clang-tidy integration (opt-in via -DENABLE_CLANG_TIDY=ON)
    # Inside the function for the same reason as codec options above.
    # =========================================================================
    option(ENABLE_CLANG_TIDY "Enable clang-tidy static analysis" OFF)
    if(ENABLE_CLANG_TIDY)
        find_program(CLANG_TIDY_EXE
            NAMES clang-tidy clang-tidy-18 clang-tidy-17 clang-tidy-16 clang-tidy-15
            HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
            REQUIRED
        )
        set_target_properties(${TARGET_LIB} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
    endif()

endfunction()
