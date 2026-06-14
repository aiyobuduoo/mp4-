# CMakeLists.txt for micro-decoder
# Supports both ESP-IDF component builds and standalone host builds
#
# Structure:
#   cmake/sources.cmake   - Source file definitions
#   cmake/esp-idf.cmake   - ESP-IDF specific configuration
#   cmake/host.cmake      - Host platform configuration

cmake_minimum_required(VERSION 3.16)

# ==============================================================================
# Build type detection
# ==============================================================================

if(DEFINED IDF_TARGET)
    set(ESP_IDF_BUILD TRUE)
else()
    set(ESP_IDF_BUILD FALSE)
endif()

# Set source directory variable for consistent use
if(ESP_IDF_BUILD)
    set(MICRO_DECODER_COMPONENT_DIR ${COMPONENT_DIR})
else()
    set(MICRO_DECODER_COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ==============================================================================
# Version (single source of truth: idf_component.yml)
# ==============================================================================

file(READ ${MICRO_DECODER_COMPONENT_DIR}/idf_component.yml _micro_decoder_manifest)
string(REGEX MATCH "version:[ \t]*[\"']?([0-9]+\\.[0-9]+\\.[0-9]+)[\"']?"
       _ ${_micro_decoder_manifest})
set(MICRO_DECODER_VERSION ${CMAKE_MATCH_1})
if(NOT MICRO_DECODER_VERSION)
    message(FATAL_ERROR "Failed to parse version from idf_component.yml")
endif()

# ==============================================================================
# Include CMake modules
# ==============================================================================

include(${MICRO_DECODER_COMPONENT_DIR}/cmake/sources.cmake)

# ==============================================================================
# ESP-IDF Build
# ==============================================================================

if(ESP_IDF_BUILD)
    include(${MICRO_DECODER_COMPONENT_DIR}/cmake/esp-idf.cmake)

    # Collect sources
    micro_decoder_get_sources(${MICRO_DECODER_COMPONENT_DIR})

    # Register the component: uses both common and ESP-specific sources
    idf_component_register(
        SRCS ${MICRO_DECODER_COMMON_SOURCES} ${MICRO_DECODER_ESP_SOURCES}
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "src" "src/esp"
        REQUIRES esp_http_client pthread esp_ringbuf
        PRIV_REQUIRES mbedtls
    )

    # Apply ESP-IDF configuration
    micro_decoder_configure_esp_idf(${COMPONENT_LIB} ${COMPONENT_DIR})

    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        MICRO_DECODER_VERSION="${MICRO_DECODER_VERSION}")

# ==============================================================================
# Host Build
# ==============================================================================

else()
    project(micro_decoder VERSION 0.3.0 LANGUAGES CXX)

    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/host.cmake)

    # Get sources
    micro_decoder_get_sources(${CMAKE_CURRENT_SOURCE_DIR})

    # Create the library: common sources + host networking
    add_library(micro_decoder STATIC ${MICRO_DECODER_COMMON_SOURCES})

    # Host build options
    option(ENABLE_WERROR "Treat warnings as errors" OFF)
    option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

    # Apply host configuration
    micro_decoder_configure_host(micro_decoder ${CMAKE_CURRENT_SOURCE_DIR})

    target_compile_definitions(micro_decoder PUBLIC
        MICRO_DECODER_VERSION="${MICRO_DECODER_VERSION}")

    # Examples
    add_subdirectory(host_examples/basic_player)

endif()
