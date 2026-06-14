# Source file definitions for micro-decoder

function(micro_decoder_get_sources BASE_DIR)
    # Common sources: build on both ESP-IDF and host
    set(MICRO_DECODER_COMMON_SOURCES
        ${BASE_DIR}/src/types.cpp
        ${BASE_DIR}/src/decoder_source.cpp
        ${BASE_DIR}/src/audio_decoder.cpp
        ${BASE_DIR}/src/audio_reader.cpp
        ${BASE_DIR}/src/ring_buffer.cpp
        ${BASE_DIR}/src/md_transfer_buffer.cpp

        PARENT_SCOPE
    )

    # ESP-IDF only sources
    set(MICRO_DECODER_ESP_SOURCES
        ${BASE_DIR}/src/esp/http_client.cpp

        PARENT_SCOPE
    )

    # Host only sources
    set(MICRO_DECODER_HOST_SOURCES
        ${BASE_DIR}/src/host/http_client.cpp

        PARENT_SCOPE
    )
endfunction()
