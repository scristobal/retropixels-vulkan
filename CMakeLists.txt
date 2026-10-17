cmake_minimum_required(VERSION 3.16)
project(RetroPixelsVulkan VERSION 1.0 LANGUAGES C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)
find_package(glfw3 3.3 REQUIRED)
find_program(RETROPIXELS_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/bin")
find_program(RETROPIXELS_GLSLANG NAMES glslangValidator HINTS "$ENV{VULKAN_SDK}/bin")
if(NOT RETROPIXELS_GLSLC AND NOT RETROPIXELS_GLSLANG)
    message(FATAL_ERROR "Install glslc or glslangValidator to compile the Vulkan shaders.")
endif()

set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")
foreach(STAGE vert frag)
    set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sprite.${STAGE}")
    set(OUTPUT "${SHADER_OUTPUT_DIR}/sprite.${STAGE}.spv")
    if(RETROPIXELS_GLSLC)
        set(SHADER_COMMAND "${RETROPIXELS_GLSLC}" --target-env=vulkan1.0 "${SOURCE}" -o "${OUTPUT}")
    else()
        set(SHADER_COMMAND "${RETROPIXELS_GLSLANG}" -V --target-env vulkan1.0 "${SOURCE}" -o "${OUTPUT}")
    endif()
    add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND ${SHADER_COMMAND}
        DEPENDS "${SOURCE}"
        COMMENT "Compiling sprite.${STAGE} to SPIR-V"
        VERBATIM)
    list(APPEND SHADER_OUTPUTS "${OUTPUT}")
endforeach()
add_custom_target(RetroPixelsShaders DEPENDS ${SHADER_OUTPUTS})

add_executable(RetroPixels src/main.c src/renderer.c src/image.c)
add_dependencies(RetroPixels RetroPixelsShaders)
target_compile_definitions(RetroPixels PRIVATE
    RETROPIXELS_SHADERS_DIR="${SHADER_OUTPUT_DIR}"
    RETROPIXELS_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_include_directories(RetroPixels SYSTEM PRIVATE external/stb)
target_link_libraries(RetroPixels PRIVATE Vulkan::Vulkan glfw)
if(UNIX)
    target_link_libraries(RetroPixels PRIVATE m)
endif()
if(MSVC)
    target_compile_options(RetroPixels PRIVATE /W4)
else()
    target_compile_options(RetroPixels PRIVATE -Wall -Wextra -Wpedantic)
endif()
