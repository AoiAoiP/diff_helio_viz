cmake_minimum_required(VERSION 3.25)
project(heliostat_viz
    VERSION 0.1.0
    DESCRIPTION "Real-time visualization front-end for the heliostat surface optimizer"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /W3)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# Dependencies: Vulkan SDK only.
#   * fmt / glm are deliberately NOT used (std::format / raw arrays instead)
#   * Slang is used as a *compiler* only (slangc -> SPIR-V); the C++ side never
#     links libslang, so no slang.dll is needed at runtime.
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED)
message(STATUS "Vulkan headers: ${Vulkan_INCLUDE_DIRS}")

if(NOT VULKAN_SDK)
    set(VULKAN_SDK "$ENV{VULKAN_SDK}")
endif()
find_program(SLANGC_EXECUTABLE
    NAMES slangc slangc.exe
    HINTS "${VULKAN_SDK}/Bin" "${VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin"
    DOC "Slang shader compiler shipped with the Vulkan SDK"
)
if(NOT SLANGC_EXECUTABLE)
    message(FATAL_ERROR
        "slangc not found. Install the Vulkan SDK (bundles Slang) and/or set VULKAN_SDK.\n"
        "  tried: $ENV{VULKAN_SDK}/Bin")
endif()
message(STATUS "slangc: ${SLANGC_EXECUTABLE}")

# ---------------------------------------------------------------------------
# Shader compilation (each entry point -> its own .spv)
# ---------------------------------------------------------------------------
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(VIZ_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/viz/shaders")
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")

# Everything the shaders #include (used for rebuild dependency tracking).
set(SHADER_SOURCES
    ${SHADER_DIR}/common.slang
    ${SHADER_DIR}/sunshape.slang
    ${SHADER_DIR}/bolt_common.slang
    ${SHADER_DIR}/bolt_forward.slang
    ${SHADER_DIR}/forward.slang
    ${SHADER_DIR}/s95_gpu.slang
)
# NOTE: shaders/reference/*.slang are the upstream inverse-rendering (bwd_diff)
# shaders. They are kept for reference and are NOT compiled here: their C++
# driver (Adam loop, gradient projection) intentionally stays in the research
# repository. See PLAN.md §3 and NOTES_provenance.md.

set(SHADER_ENTRIES
    "clearFlux:forward.slang"
    "finalizeFlux:forward.slang"
    "renderForward:forward.slang"
    "renderForwardBuie:forward.slang"
    "renderForwardPillbox:forward.slang"
    "renderForwardGaussian:forward.slang"
    "computeBoltSurface:bolt_forward.slang"
    "computeS95FindLevel:s95_gpu.slang"
)

foreach(entry_def ${SHADER_ENTRIES})
    string(REPLACE ":" ";" entry_parts "${entry_def}")
    list(GET entry_parts 0 ENTRY_NAME)
    list(GET entry_parts 1 SOURCE_FILE)

    set(SPV_OUTPUT "${SHADER_OUTPUT_DIR}/${ENTRY_NAME}.spv")
    add_custom_command(
        OUTPUT ${SPV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND ${SLANGC_EXECUTABLE}
            -std 2026 -target spirv -O2
            -entry ${ENTRY_NAME}
            -o ${SPV_OUTPUT}
            "${SHADER_DIR}/${SOURCE_FILE}"
        DEPENDS ${SHADER_SOURCES} ${SLANGC_EXECUTABLE}
        COMMENT "slangc: ${ENTRY_NAME} <- ${SOURCE_FILE}"
        VERBATIM
    )
    list(APPEND ALL_SPV_FILES ${SPV_OUTPUT})
endforeach()

add_custom_target(shaders ALL DEPENDS ${ALL_SPV_FILES})

# ---------------------------------------------------------------------------
# Viewer shaders (viz/shaders/*.slang). Graphics stages are inferred from the
# [shader("vertex"|"fragment")] attribute; the SPIR-V entry point is always
# "main". They import the physics core (common.slang, sunshape.slang), so the
# core shader directory is on the include path.
# ---------------------------------------------------------------------------
set(VIZ_SHADER_SOURCES
    ${VIZ_SHADER_DIR}/flux_lite.slang
    ${VIZ_SHADER_DIR}/scene.slang
    ${VIZ_SHADER_DIR}/post.slang
    ${VIZ_SHADER_DIR}/hud.slang
)

set(VIZ_SHADER_ENTRIES
    "clearFluxLite:flux_lite.slang"
    "fluxLite:flux_lite.slang"
    "finalizeFluxLite:flux_lite.slang"
    "vsSky:scene.slang"
    "fsSky:scene.slang"
    "vsScene:scene.slang"
    "fsScene:scene.slang"
    "vsFluxMap:scene.slang"
    "fsFluxMap:scene.slang"
    "vsFullscreen:post.slang"
    "fsBloomPre:post.slang"
    "fsBloomDown:post.slang"
    "fsBloomUp:post.slang"
    "fsComposite:post.slang"
    "fsCopy:post.slang"
    "vsHud:hud.slang"
    "fsHud:hud.slang"
)

foreach(entry_def ${VIZ_SHADER_ENTRIES})
    string(REPLACE ":" ";" entry_parts "${entry_def}")
    list(GET entry_parts 0 ENTRY_NAME)
    list(GET entry_parts 1 SOURCE_FILE)

    set(SPV_OUTPUT "${SHADER_OUTPUT_DIR}/${ENTRY_NAME}.spv")
    add_custom_command(
        OUTPUT ${SPV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND ${SLANGC_EXECUTABLE}
            -std 2026 -target spirv -O2
            -I "${SHADER_DIR}"
            -entry ${ENTRY_NAME}
            -o ${SPV_OUTPUT}
            "${VIZ_SHADER_DIR}/${SOURCE_FILE}"
        DEPENDS ${VIZ_SHADER_SOURCES} ${SHADER_SOURCES} ${SLANGC_EXECUTABLE}
        COMMENT "slangc: ${ENTRY_NAME} <- viz/shaders/${SOURCE_FILE}"
        VERBATIM
    )
    list(APPEND ALL_VIZ_SPV_FILES ${SPV_OUTPUT})
endforeach()

add_custom_target(viz_shaders ALL DEPENDS ${ALL_VIZ_SPV_FILES})

# ---------------------------------------------------------------------------
# Core engine CLI (headless forward renderer; the viewer links the same sources)
# ---------------------------------------------------------------------------
add_executable(heliostat_core
    src/main.cpp
    src/vk.cpp
    src/config.cpp
    src/data.cpp
    src/engine.cpp
)
target_include_directories(heliostat_core PRIVATE src ${Vulkan_INCLUDE_DIRS})
target_link_libraries(heliostat_core PRIVATE Vulkan::Vulkan)
add_dependencies(heliostat_core shaders)

# Make the freshly compiled SPIR-V reachable from the executable directory, so
# the binary can be launched with any working directory.
add_custom_command(TARGET heliostat_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:heliostat_core>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${SHADER_OUTPUT_DIR}" "$<TARGET_FILE_DIR:heliostat_core>/shaders"
    COMMENT "Copying SPIR-V next to heliostat_core"
)

# ---------------------------------------------------------------------------
# Visualization target — Win32 + Vulkan 1.4, hand-written (no GLFW/SDL/ImGui).
#   * links the same verified core sources (vk/config/data/engine)
#   * src/vk.cpp gains an optional win32 surface path (headless CLI unchanged)
# ---------------------------------------------------------------------------
add_executable(heliostat_viz WIN32
    viz/src/viz_main.cpp
    viz/src/platform_win32.cpp
    viz/src/vk_context.cpp
    viz/src/vk_gfx.cpp
    viz/src/vk_flux.cpp
    viz/src/vk_scene.cpp
    viz/src/vk_post.cpp
    viz/src/hud.cpp
    viz/src/gpu_timer.cpp
    viz/src/camera.cpp
    src/vk.cpp
    src/config.cpp
    src/data.cpp
    src/engine.cpp
)
target_include_directories(heliostat_viz PRIVATE src viz/src ${Vulkan_INCLUDE_DIRS})
target_link_libraries(heliostat_viz PRIVATE Vulkan::Vulkan)
# UNICODE/_UNICODE so the resource macros (IDC_ARROW, IDI_APPLICATION) match the
# explicit ...W entry points this layer calls.
target_compile_definitions(heliostat_viz PRIVATE UNICODE _UNICODE)
add_dependencies(heliostat_viz shaders viz_shaders)

add_custom_command(TARGET heliostat_viz POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:heliostat_viz>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${SHADER_OUTPUT_DIR}" "$<TARGET_FILE_DIR:heliostat_viz>/shaders"
    COMMENT "Copying SPIR-V next to heliostat_viz"
)
