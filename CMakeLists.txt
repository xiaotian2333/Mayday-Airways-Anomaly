cmake_minimum_required(VERSION 3.22)

set(ANOMALY_RELEASE_VERSION "1.0.0" CACHE STRING
    "Release version used to configure Anomaly")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/anomaly_version.cmake")
anomaly_resolve_version("${ANOMALY_RELEASE_VERSION}"
    ANOMALY_PROJECT_VERSION ANOMALY_VERSION_STRING)
project(Anomaly VERSION "${ANOMALY_PROJECT_VERSION}" LANGUAGES C CXX)

set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(ANOMALY_GENERATED_INCLUDE_DIR "${GENERATED_DIR}/sdk-build/include")
set(ANOMALY_GENERATED_INSTALL_INCLUDE_DIR "${GENERATED_DIR}/sdk-install/include")
file(MAKE_DIRECTORY "${ANOMALY_GENERATED_INCLUDE_DIR}/anomaly/sdk")
file(MAKE_DIRECTORY "${ANOMALY_GENERATED_INSTALL_INCLUDE_DIR}/anomaly/sdk")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.h.in"
    "${ANOMALY_GENERATED_INCLUDE_DIR}/anomaly/sdk/configured_version.h"
    @ONLY)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.h.in"
    "${ANOMALY_GENERATED_INSTALL_INCLUDE_DIR}/anomaly/sdk/version.h"
    @ONLY)
# In-tree SDK consumers include headers through several internal targets; keep
# the configured release data available to the source-tree version header.
include_directories(BEFORE "${ANOMALY_GENERATED_INCLUDE_DIR}")
add_compile_definitions(ANOMALY_USE_CONFIGURED_VERSION=1)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Anomaly currently targets Windows x64 only")
endif()

enable_language(ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_property(ANOMALY_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(ANOMALY_IS_MULTI_CONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
else()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# Development fixtures: the offline platform preview, the render fixtures and the
# plugin-side test harnesses. They belong to no release component, so the default
# build only produces the Runtime, Tools, SDK targets; enable them explicitly
# (`build.cmd fixtures`) when working on them.
option(ANOMALY_BUILD_TEST_FIXTURES
    "Build development fixtures (platform preview, render and plugin test harnesses)" OFF)

# Linker PDBs exist for the Symbols component alone, and no release publishes that
# component, so the default build does not produce them. `build.cmd symbols`
# (or -DANOMALY_BUILD_SYMBOLS=ON) enables both generation and packaging.
option(ANOMALY_BUILD_SYMBOLS
    "Generate and package linker PDBs for the release components" OFF)
if(NOT ANOMALY_BUILD_SYMBOLS)
    # RelWithDebInfo is the shipped configuration, so its debug information only
    # fed the Symbols component; Debug keeps /Zi for local debugging.
    foreach(anomaly_debug_flag_variable IN ITEMS
            CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        string(REPLACE "/Zi" "" ${anomaly_debug_flag_variable}
            "${${anomaly_debug_flag_variable}}")
        string(STRIP "${${anomaly_debug_flag_variable}}"
            ${anomaly_debug_flag_variable})
    endforeach()
    foreach(anomaly_debug_link_variable IN ITEMS
            CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO
            CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO
            CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO)
        string(REPLACE "/debug" "" ${anomaly_debug_link_variable}
            "${${anomaly_debug_link_variable}}")
        string(STRIP "${${anomaly_debug_link_variable}}"
            ${anomaly_debug_link_variable})
    endforeach()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(anomaly_sdk INTERFACE)
add_library(Anomaly::sdk ALIAS anomaly_sdk)
set_target_properties(anomaly_sdk PROPERTIES EXPORT_NAME sdk)
target_compile_features(anomaly_sdk INTERFACE c_std_11 cxx_std_20)
target_compile_definitions(anomaly_sdk INTERFACE
    $<BUILD_INTERFACE:ANOMALY_USE_CONFIGURED_VERSION=1>)
target_include_directories(anomaly_sdk INTERFACE
    $<BUILD_INTERFACE:${ANOMALY_GENERATED_INCLUDE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk/AnomalyPlugin.cmake")

# Populate dependencies directly instead of starting an MSBuild sub-build for each.
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()
include(FetchContent)
FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(minhook)

# Portable BLAKE3 produces the FIoHash metadata for generated compatibility containers.
FetchContent_Declare(
    blake3
    URL https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/1.3.1.zip
    URL_HASH SHA256=7a417e3571cde3199cb709d4f732709172ceb35f1ff740e4564faf72a79330c4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
if(ANOMALY_BUILD_OPTIONAL_LIBS)
    FetchContent_MakeAvailable(blake3)
    add_library(anomaly_blake3 STATIC
        "${blake3_SOURCE_DIR}/c/blake3.c"
        "${blake3_SOURCE_DIR}/c/blake3_dispatch.c"
        "${blake3_SOURCE_DIR}/c/blake3_portable.c")
    target_include_directories(anomaly_blake3 PUBLIC "${blake3_SOURCE_DIR}/c")
    target_compile_definitions(anomaly_blake3 PRIVATE
        BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512 BLAKE3_USE_NEON=0)
endif()

set(JSON_BuildTests OFF)
set(JSON_Install OFF)
set(JSON_MultipleHeaders OFF)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)
unset(JSON_BuildTests)
unset(JSON_Install)
unset(JSON_MultipleHeaders)

set(JSON_VALIDATOR_INSTALL OFF)
set(JSON_VALIDATOR_BUILD_TESTS OFF)
set(JSON_VALIDATOR_BUILD_EXAMPLES OFF)
set(JSON_VALIDATOR_SHARED_LIBS OFF)
FetchContent_Declare(
    json_schema_validator
    URL https://github.com/pboettch/json-schema-validator/archive/refs/tags/2.3.0.tar.gz
    URL_HASH SHA256=2c00b50023c7d557cdaa71c0777f5bcff996c4efd7a539e58beaa4219fa2a5e1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json_schema_validator)
unset(JSON_VALIDATOR_INSTALL)
unset(JSON_VALIDATOR_BUILD_TESTS)
unset(JSON_VALIDATOR_BUILD_EXAMPLES)
unset(JSON_VALIDATOR_SHARED_LIBS)
# validator 链接进手工映射的 Core，其函数级静态守卫同样要禁用 TLS 托管。
if(TARGET nlohmann_json_schema_validator)
    target_compile_options(nlohmann_json_schema_validator PRIVATE /Zc:threadSafeInit-)
endif()

# miniz provides the ZIP inflate used to install Dalamud-style plugin packages.
FetchContent_Declare(
    miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(miniz)
add_library(anomaly_miniz STATIC "${miniz_SOURCE_DIR}/miniz.c")
target_include_directories(anomaly_miniz PUBLIC "${miniz_SOURCE_DIR}")
if(MSVC)
    target_compile_options(anomaly_miniz PRIVATE /w)
endif()

# miniaudio decodes the HiFiVehicleMusic library and drives WASAPI/DirectSound;
# RtAudio supplies the ASIO host (built from source, its CMake project is skipped).
FetchContent_Declare(
    miniaudio
    URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.25.tar.gz
    URL_HASH SHA256=b900edcffe979816e2560a0580b9b1216d674b4f17fbadeca8f777a7f8ab0274
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR anomaly-no-cmake
)
FetchContent_Declare(
    rtaudio
    URL https://github.com/thestk/rtaudio/archive/refs/tags/6.0.1.tar.gz
    URL_HASH SHA256=7206c8b6cee43b474f43d64988fefaadfdcfc4264ed38d8de5f5d0e6ddb0a123
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR anomaly-no-cmake
)
if(ANOMALY_BUILD_OPTIONAL_LIBS)
    FetchContent_MakeAvailable(miniaudio rtaudio)
    add_library(anomaly_miniaudio STATIC "${miniaudio_SOURCE_DIR}/miniaudio.c")
    target_include_directories(anomaly_miniaudio PUBLIC "${miniaudio_SOURCE_DIR}")
    add_library(anomaly_rtaudio_asio STATIC
        "${rtaudio_SOURCE_DIR}/RtAudio.cpp"
        "${rtaudio_SOURCE_DIR}/include/asio.cpp"
        "${rtaudio_SOURCE_DIR}/include/asiodrivers.cpp"
        "${rtaudio_SOURCE_DIR}/include/asiolist.cpp"
        "${rtaudio_SOURCE_DIR}/include/iasiothiscallresolver.cpp")
    target_include_directories(anomaly_rtaudio_asio PUBLIC
        "${rtaudio_SOURCE_DIR}" "${rtaudio_SOURCE_DIR}/include")
    target_compile_definitions(anomaly_rtaudio_asio PUBLIC __WINDOWS_ASIO__)
    target_link_libraries(anomaly_rtaudio_asio PUBLIC ole32 winmm user32 advapi32)
    if(MSVC)
        target_compile_options(anomaly_miniaudio PRIVATE /w)
        target_compile_options(anomaly_rtaudio_asio PRIVATE /w)
    endif()
endif()
if(ANOMALY_BUILD_OPTIONAL_LIBS)
    # Monkey's Audio SDK decodes .ape for HiFiVehicleMusic (official SDK archive,
    # built as a static library from its library sources; its CMake project is skipped).
    FetchContent_Declare(
        monkeys_audio
        URL https://monkeysaudio.com/files/MAC_1327_SDK.zip
        URL_HASH SHA256=c47c6b36f6a7bd50d990f2eb36a70915c0074a7b9634be396c94464905e76686
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR anomaly-no-cmake
    )
    FetchContent_MakeAvailable(monkeys_audio)
    set(ANOMALY_MONKEYS_AUDIO_INCLUDE_DIRS
        "${monkeys_audio_SOURCE_DIR}/Source/Shared" "${monkeys_audio_SOURCE_DIR}/Source/MACLib")
    set(anomaly_monkeys_audio_sources)
    foreach(anomaly_mac_source IN ITEMS
            Shared/BufferIO Shared/CharacterHelper Shared/CircleBuffer Shared/CPUFeatures Shared/CRC
            Shared/GlobalFunctions Shared/MemoryIO Shared/Semaphore Shared/Thread Shared/WholeFileIO
            Shared/WinFileIO
            MACLib/APECompress MACLib/APECompressCore MACLib/APECompressCreate MACLib/APEDecompress
            MACLib/APEDecompressCore MACLib/APEHeader MACLib/APEInfo MACLib/APELink MACLib/APETag
            MACLib/BitArray MACLib/FloatTransform MACLib/MACLib MACLib/MACProgressHelper MACLib/MD5
            MACLib/NewPredictor MACLib/NNFilter MACLib/NNFilterGeneric MACLib/NNFilterSSE2
            MACLib/NNFilterSSE4.1 MACLib/NNFilterAVX2 MACLib/NNFilterAVX512 MACLib/Prepare
            MACLib/UnBitArray MACLib/UnBitArrayBase MACLib/WAVInputSource MACLib/Interim
            MACLib/Old/NewPredictorOld MACLib/Old/AntiPredictorOld MACLib/Old/AntiPredictorExtraHighOld
            MACLib/Old/AntiPredictorFastOld MACLib/Old/AntiPredictorHighOld MACLib/Old/AntiPredictorNormalOld
            MACLib/Old/APEDecompressCoreOld MACLib/Old/APEDecompressOld MACLib/Old/UnBitArrayOld
            MACLib/Old/UnMACOld)
        list(APPEND anomaly_monkeys_audio_sources "${monkeys_audio_SOURCE_DIR}/Source/${anomaly_mac_source}.cpp")
    endforeach()
    add_library(anomaly_monkeys_audio STATIC ${anomaly_monkeys_audio_sources})
    # Kept private: the SDK headers (All.h, Thread.h, Version.h, ...) would shadow others.
    target_include_directories(anomaly_monkeys_audio PRIVATE ${ANOMALY_MONKEYS_AUDIO_INCLUDE_DIRS})
    target_compile_definitions(anomaly_monkeys_audio PRIVATE PLATFORM_WINDOWS UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(anomaly_monkeys_audio PRIVATE /w)
    endif()
endif()
if(ANOMALY_BUILD_OPTIONAL_LIBS)
    # dsd2pcm converts DSD to PCM when neither native DSD nor DoP reaches the DAC.
    FetchContent_Declare(
        dsd2pcm
        URL https://github.com/GiangLH/dsd2pcm/archive/354fcb539d11920980ecd88249954f883a3d16f4.tar.gz
        URL_HASH SHA256=bd01b056e8e82ddb6c6859cce4f359925cdee4459c8b2bc11d0ebe8a5599f2ba
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR anomaly-no-cmake
    )
    FetchContent_MakeAvailable(dsd2pcm)
    add_library(anomaly_dsd2pcm STATIC "${dsd2pcm_SOURCE_DIR}/dsd2pcm.c")
    target_include_directories(anomaly_dsd2pcm PUBLIC "${dsd2pcm_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(anomaly_dsd2pcm PRIVATE /w)
    endif()
endif()

# Liquid glass shaders live in src/ui/shaders and are compiled at build time into
# bytecode headers, so the renderer never compiles HLSL at run time.
file(GLOB ANOMALY_GLASS_FXC_CANDIDATES
    "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64/fxc.exe")
list(SORT ANOMALY_GLASS_FXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
set(ANOMALY_GLASS_FXC_HINTS "")
foreach(candidate IN LISTS ANOMALY_GLASS_FXC_CANDIDATES)
    get_filename_component(candidate_directory "${candidate}" DIRECTORY)
    list(APPEND ANOMALY_GLASS_FXC_HINTS "${candidate_directory}")
endforeach()
find_program(ANOMALY_GLASS_FXC fxc
    HINTS
        "${CMAKE_WINDOWS_KITS_10_DIR}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        ${ANOMALY_GLASS_FXC_HINTS}
    REQUIRED)
set(ANOMALY_GLASS_SHADER_DIR "${GENERATED_DIR}/anomaly/shaders")
file(MAKE_DIRECTORY "${ANOMALY_GLASS_SHADER_DIR}")
set(ANOMALY_GLASS_SHADER_HEADERS "")
function(anomaly_compile_glass_shader source entry profile variable)
    set(output "${ANOMALY_GLASS_SHADER_DIR}/${variable}.h")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${ANOMALY_GLASS_FXC}" /nologo /O3 /Ges /T ${profile} /E ${entry}
                /Vn ${variable} /Fh "${output}" "${CMAKE_SOURCE_DIR}/${source}"
        DEPENDS "${CMAKE_SOURCE_DIR}/${source}"
        COMMENT "Compiling ${source} (${entry})"
        VERBATIM)
    set(ANOMALY_GLASS_SHADER_HEADERS ${ANOMALY_GLASS_SHADER_HEADERS} "${output}" PARENT_SCOPE)
endfunction()
anomaly_compile_glass_shader(src/ui/shaders/glass_pass.hlsl vs_full vs_4_0 kGlassFullScreenVs)
anomaly_compile_glass_shader(src/ui/shaders/glass_pass.hlsl ps_down ps_4_0 kGlassDownsamplePs)
anomaly_compile_glass_shader(src/ui/shaders/glass_pass.hlsl ps_blur ps_4_0 kGlassBlurPs)
anomaly_compile_glass_shader(src/ui/shaders/glass_surface.hlsl vs_quad vs_4_0 kGlassSurfaceVs)
anomaly_compile_glass_shader(src/ui/shaders/glass_surface.hlsl ps_glass ps_4_0 kGlassSurfacePs)

add_library(anomaly_imgui STATIC
    ${ANOMALY_GLASS_SHADER_HEADERS}
    src/ui/shaders/glass_pass.hlsl
    src/ui/shaders/glass_surface.hlsl
    src/ui/host_ui_service.cpp
    src/ui/liquid_glass.cpp
    src/ui/platform_ui_theme.cpp
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp"
)
target_include_directories(anomaly_imgui PUBLIC "${imgui_SOURCE_DIR}"
    PRIVATE include "${GENERATED_DIR}")
target_compile_definitions(anomaly_imgui PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_imgui PRIVATE /W3 /EHsc /utf-8)  # 消费含中文注释的 SDK 头
target_link_libraries(anomaly_imgui PUBLIC d3d11 d3d12 dxgi dxguid)
# The .hlsl files are listed for the IDE only; fxc builds them above.
set_source_files_properties(
    src/ui/shaders/glass_pass.hlsl src/ui/shaders/glass_surface.hlsl
    PROPERTIES HEADER_FILE_ONLY TRUE)

add_executable(proxy_generator tools/proxy_generator.cpp)
target_compile_definitions(proxy_generator PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

set(PROXY_DEF "${GENERATED_DIR}/dwmapi.def")
set(PROXY_ASM "${GENERATED_DIR}/dwmapi.asm")
set(PROXY_CPP "${GENERATED_DIR}/proxy_exports.cpp")
# This prefiltered 32px derivative keeps the one-level header texture close
# to its display size without imposing mip generation on generic UI assets.
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/logo-header.png" ANOMALY_PLATFORM_LOGO_PATH)
file(TO_CMAKE_PATH
    "${CMAKE_SOURCE_DIR}/apps/launcher/AnomalyLauncher.ico"
    ANOMALY_LAUNCHER_ICON_PATH)
set(PLATFORM_UI_RESOURCES_RC "${GENERATED_DIR}/platform_ui_resources.rc")
# The same artwork is compiled into the image's read-only data, because a core that is mapped without a
# loader cannot read its own resource directory once its headers are hidden. Generating it here keeps one
# source of truth: the PNGs above.
file(READ "${ANOMALY_PLATFORM_LOGO_PATH}" ANOMALY_PLATFORM_LOGO_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1,"
    ANOMALY_PLATFORM_LOGO_BYTES "${ANOMALY_PLATFORM_LOGO_HEX}")
string(LENGTH "${ANOMALY_PLATFORM_LOGO_HEX}" ANOMALY_PLATFORM_LOGO_HEX_LENGTH)
math(EXPR ANOMALY_PLATFORM_LOGO_COUNT "${ANOMALY_PLATFORM_LOGO_HEX_LENGTH} / 2")
file(MAKE_DIRECTORY "${GENERATED_DIR}/anomaly")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/embedded_artwork.hpp.in"
    "${GENERATED_DIR}/anomaly/embedded_artwork.hpp" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ANOMALY_PLATFORM_LOGO_PATH}")
set(LAUNCHER_RESOURCES_RC "${GENERATED_DIR}/AnomalyLauncher.rc")

file(MAKE_DIRECTORY "${GENERATED_DIR}")
set(ANOMALY_HOST_EN_US_CATALOG_FILE
    "${CMAKE_SOURCE_DIR}/locales/host/en-US.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ANOMALY_HOST_EN_US_CATALOG_FILE}")
file(READ "${ANOMALY_HOST_EN_US_CATALOG_FILE}" ANOMALY_HOST_EN_US_CATALOG_JSON)
set(ANOMALY_HOST_EN_US_CATALOG_LITERALS "")
string(LENGTH "${ANOMALY_HOST_EN_US_CATALOG_JSON}"
    ANOMALY_HOST_EN_US_CATALOG_LENGTH)
set(ANOMALY_HOST_EN_US_CATALOG_OFFSET 0)
while(ANOMALY_HOST_EN_US_CATALOG_OFFSET LESS ANOMALY_HOST_EN_US_CATALOG_LENGTH)
    math(EXPR ANOMALY_HOST_EN_US_CATALOG_REMAINING
        "${ANOMALY_HOST_EN_US_CATALOG_LENGTH} - ${ANOMALY_HOST_EN_US_CATALOG_OFFSET}")
    if(ANOMALY_HOST_EN_US_CATALOG_REMAINING GREATER 8000)
        set(ANOMALY_HOST_EN_US_CATALOG_CHUNK_LENGTH 8000)
    else()
        set(ANOMALY_HOST_EN_US_CATALOG_CHUNK_LENGTH
            ${ANOMALY_HOST_EN_US_CATALOG_REMAINING})
    endif()
    string(SUBSTRING "${ANOMALY_HOST_EN_US_CATALOG_JSON}"
        ${ANOMALY_HOST_EN_US_CATALOG_OFFSET}
        ${ANOMALY_HOST_EN_US_CATALOG_CHUNK_LENGTH}
        ANOMALY_HOST_EN_US_CATALOG_CHUNK)
    string(APPEND ANOMALY_HOST_EN_US_CATALOG_LITERALS
        "    R\"anomaly_catalog(${ANOMALY_HOST_EN_US_CATALOG_CHUNK})anomaly_catalog\"\n")
    math(EXPR ANOMALY_HOST_EN_US_CATALOG_OFFSET
        "${ANOMALY_HOST_EN_US_CATALOG_OFFSET} + ${ANOMALY_HOST_EN_US_CATALOG_CHUNK_LENGTH}")
endwhile()
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/i18n_embedded_catalog.hpp.in"
    "${GENERATED_DIR}/anomaly/i18n_embedded_catalog.hpp"
    @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/locales/host/en-US.json"
    "${CMAKE_SOURCE_DIR}/schemas/plugin-manifest.schema.json"
    "${CMAKE_SOURCE_DIR}/schemas/repository-cache.schema.json"
    "${CMAKE_SOURCE_DIR}/schemas/repository-config.schema.json"
    "${CMAKE_SOURCE_DIR}/schemas/repository-index.schema.json"
    "${CMAKE_SOURCE_DIR}/logo-header.png"
    "${CMAKE_SOURCE_DIR}/logo.png"
    "${CMAKE_SOURCE_DIR}/apps/launcher/assets/afdian-sponsor.png"
    "${CMAKE_SOURCE_DIR}/apps/launcher/assets/kofi.png"
    "${CMAKE_SOURCE_DIR}/apps/launcher/AnomalyLauncher.ico")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/platform_ui_resources.rc.in"
    "${PLATFORM_UI_RESOURCES_RC}"
    @ONLY)
configure_file(
    "${CMAKE_SOURCE_DIR}/apps/launcher/AnomalyLauncher.rc.in"
    "${LAUNCHER_RESOURCES_RC}"
    @ONLY)
file(READ
    "${CMAKE_SOURCE_DIR}/schemas/plugin-manifest.schema.json"
    ANOMALY_PLUGIN_MANIFEST_SCHEMA_JSON)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/plugin_manifest_schema.hpp.in"
    "${GENERATED_DIR}/plugin_manifest_schema.hpp"
    @ONLY)
file(READ
    "${CMAKE_SOURCE_DIR}/schemas/plugin-profile.schema.json"
    ANOMALY_PLUGIN_PROFILE_SCHEMA_JSON)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/plugin_profile_schema.hpp.in"
    "${GENERATED_DIR}/plugin_profile_schema.hpp"
    @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/schemas/plugin-profile.schema.json")
file(READ
    "${CMAKE_SOURCE_DIR}/schemas/repository-index.schema.json"
    ANOMALY_REPOSITORY_INDEX_SCHEMA_JSON)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/repository_index_schema.hpp.in"
    "${GENERATED_DIR}/repository_index_schema.hpp"
    @ONLY)
file(READ
    "${CMAKE_SOURCE_DIR}/schemas/repository-config.schema.json"
    ANOMALY_REPOSITORY_CONFIG_SCHEMA_JSON)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/repository_config_schema.hpp.in"
    "${GENERATED_DIR}/repository_config_schema.hpp"
    @ONLY)
file(READ
    "${CMAKE_SOURCE_DIR}/schemas/repository-cache.schema.json"
    ANOMALY_REPOSITORY_CACHE_SCHEMA_JSON)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/repository_cache_schema.hpp.in"
    "${GENERATED_DIR}/repository_cache_schema.hpp"
    @ONLY)
set(ANOMALY_SOURCE_COMMIT "")
set(ANOMALY_SOURCE_DIRTY true)
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE ANOMALY_GIT_REVISION_RESULT
        OUTPUT_VARIABLE ANOMALY_SOURCE_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE ANOMALY_GIT_STATUS_RESULT
        OUTPUT_VARIABLE ANOMALY_GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(ANOMALY_GIT_REVISION_RESULT EQUAL 0 AND
       ANOMALY_GIT_STATUS_RESULT EQUAL 0 AND
       ANOMALY_GIT_STATUS STREQUAL "")
        set(ANOMALY_SOURCE_DIRTY false)
    endif()
endif()
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/build_provenance.hpp.in"
    "${GENERATED_DIR}/build_provenance.hpp"
    @ONLY)

add_custom_command(
    OUTPUT "${PROXY_DEF}" "${PROXY_ASM}" "${PROXY_CPP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND $<TARGET_FILE:proxy_generator>
            "${CMAKE_SOURCE_DIR}/config/dwmapi.exports" "${GENERATED_DIR}"
    DEPENDS proxy_generator "${CMAKE_SOURCE_DIR}/config/dwmapi.exports"
    VERBATIM
)

add_library(anomaly_memory STATIC
    src/platform/windows/memory.cpp
    src/platform/windows/pattern.cpp)
target_include_directories(anomaly_memory PUBLIC include)
target_compile_definitions(
    anomaly_memory PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_memory PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释
target_link_libraries(anomaly_memory PUBLIC Psapi)

add_library(anomaly_i18n STATIC src/i18n/i18n.cpp)
target_include_directories(
    anomaly_i18n PUBLIC include PRIVATE "${GENERATED_DIR}")
target_compile_definitions(
    anomaly_i18n PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# The embedded catalog is a raw string literal holding UTF-8; without /utf-8 MSVC decodes it
# with the local code page and the bytes become ill-formed at parse time.
target_compile_options(anomaly_i18n PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(anomaly_i18n PRIVATE nlohmann_json::nlohmann_json)

add_library(anomaly_config STATIC src/platform/windows/config.cpp)
target_include_directories(anomaly_config PUBLIC include)
target_compile_definitions(
    anomaly_config PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_config PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释
target_link_libraries(anomaly_config PUBLIC anomaly_i18n)

add_library(anomaly_telemetry STATIC src/services/telemetry.cpp)
target_include_directories(anomaly_telemetry PUBLIC include)
target_compile_definitions(anomaly_telemetry PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_telemetry PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释
target_link_libraries(anomaly_telemetry PRIVATE winhttp bcrypt shell32 ole32 nlohmann_json::nlohmann_json)

add_library(anomaly_bootstrap_client STATIC src/bootstrap/bootstrap_client.cpp)
target_include_directories(anomaly_bootstrap_client PUBLIC include)
target_compile_definitions(
    anomaly_bootstrap_client PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_bootstrap_client PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_internal_memory_services STATIC
    src/platform/windows/module_memory_service.cpp
    src/platform/windows/pattern_service.cpp)
target_include_directories(anomaly_internal_memory_services PUBLIC include)
target_compile_definitions(
    anomaly_internal_memory_services PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_internal_memory_services PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_internal_memory_services PUBLIC anomaly_memory)


# AdapterServiceRegistry：插件可见的服务表注册中心（通用设施）
add_library(anomaly_adapter_services STATIC
    src/services/adapter_service_registry.cpp)
target_include_directories(anomaly_adapter_services PUBLIC include)
target_compile_definitions(
    anomaly_adapter_services PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_adapter_services PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_adapter_services PUBLIC nlohmann_json::nlohmann_json)

if(ANOMALY_BUILD_OPTIONAL_LIBS)
    add_library(anomaly_udp_packet_parser STATIC src/diagnostics/udp_packet_parser.cpp)
    target_include_directories(anomaly_udp_packet_parser PUBLIC include)
    target_compile_definitions(
        anomaly_udp_packet_parser PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(anomaly_udp_packet_parser PRIVATE /W4 /permissive- /EHsc)

    add_library(anomaly_npcap_capture STATIC src/diagnostics/npcap_capture.cpp)
    target_include_directories(anomaly_npcap_capture PUBLIC include)
    target_compile_definitions(
        anomaly_npcap_capture PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(anomaly_npcap_capture PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(anomaly_npcap_capture PUBLIC Ws2_32)
endif()



add_library(anomaly_signature_updater STATIC src/services/signature_updater.cpp)
target_include_directories(anomaly_signature_updater PUBLIC include
    PRIVATE "${minhook_SOURCE_DIR}/src/hde")
target_compile_definitions(anomaly_signature_updater PUBLIC
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_signature_updater PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释
target_link_libraries(anomaly_signature_updater PUBLIC anomaly_memory
    PRIVATE anomaly_reliable_storage minhook nlohmann_json::nlohmann_json)






add_library(anomaly_websocket_server STATIC src/platform/windows/websocket_server.cpp)
target_include_directories(anomaly_websocket_server PUBLIC include)
target_compile_definitions(
    anomaly_websocket_server PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_websocket_server PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释
target_link_libraries(anomaly_websocket_server PUBLIC Ws2_32 bcrypt)


add_library(anomaly_diagnostics STATIC
    src/diagnostics/analyzer.cpp
    src/diagnostics/diagnostic_pipe_service.cpp
    src/diagnostics/json.cpp
    src/diagnostics/pipe_server.cpp
    src/runtime/service_graph_diagnostics.cpp)
target_include_directories(anomaly_diagnostics PUBLIC include)
target_compile_definitions(anomaly_diagnostics PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_diagnostics PRIVATE /W4 /permissive- /EHsc /utf-8)  # 依赖的公共头含中文注释
target_link_libraries(
    anomaly_diagnostics
    PUBLIC anomaly_config anomaly_internal_memory_services anomaly_memory Advapi32
    PRIVATE nlohmann_json::nlohmann_json)

add_library(anomaly_runtime_kernel STATIC src/runtime/runtime_session.cpp)
target_include_directories(anomaly_runtime_kernel PUBLIC include)
target_compile_definitions(
    anomaly_runtime_kernel PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_runtime_kernel PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释

add_library(anomaly_dispatcher STATIC src/runtime/dispatcher.cpp)
target_include_directories(anomaly_dispatcher PUBLIC include)
target_compile_definitions(
    anomaly_dispatcher PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_dispatcher PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_runtime_dispatchers STATIC src/runtime/runtime_dispatchers.cpp)
target_include_directories(anomaly_runtime_dispatchers PUBLIC include)
target_compile_definitions(
    anomaly_runtime_dispatchers PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_runtime_dispatchers PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_runtime_dispatchers PUBLIC anomaly_dispatcher)

add_library(anomaly_structured_logger STATIC
    src/services/log_retention.cpp
    src/services/structured_logger.cpp)
target_include_directories(anomaly_structured_logger PUBLIC include)
target_compile_definitions(
    anomaly_structured_logger PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_structured_logger PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_crash_reporter STATIC src/diagnostics/crash_reporter.cpp)
target_include_directories(anomaly_crash_reporter PUBLIC include)
target_compile_definitions(
    anomaly_crash_reporter PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_crash_reporter PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_crash_reporter PUBLIC Dbghelp)

add_library(anomaly_reliable_storage STATIC src/services/reliable_storage.cpp)
target_include_directories(anomaly_reliable_storage PUBLIC include)
target_compile_definitions(
    anomaly_reliable_storage PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_reliable_storage PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释

add_library(anomaly_artifact_crypto STATIC src/repository/artifact_crypto.cpp)
target_include_directories(anomaly_artifact_crypto PUBLIC include)
target_compile_definitions(
    anomaly_artifact_crypto PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_artifact_crypto PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_artifact_crypto PUBLIC bcrypt)

add_library(anomaly_launcher_proxy STATIC src/launcher/proxy_installation.cpp)
target_include_directories(anomaly_launcher_proxy PUBLIC include)
target_compile_definitions(
    anomaly_launcher_proxy PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_launcher_proxy PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释
target_link_libraries(anomaly_launcher_proxy PUBLIC anomaly_artifact_crypto)

add_library(anomaly_launcher_configuration STATIC src/launcher/configuration.cpp)
target_include_directories(anomaly_launcher_configuration PUBLIC include)
target_compile_definitions(
    anomaly_launcher_configuration PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_launcher_configuration PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释
target_link_libraries(anomaly_launcher_configuration
    PRIVATE nlohmann_json::nlohmann_json Advapi32)

add_library(anomaly_manual_map STATIC
    src/launcher/import_procedure.cpp
    src/launcher/manual_map.cpp
    src/launcher/manual_map_remote.asm
    src/launcher/manual_map_hijack.asm
    src/launcher/manual_map_load_library.asm)
target_include_directories(anomaly_manual_map PUBLIC include)
target_compile_definitions(
    anomaly_manual_map PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_manual_map PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /EHsc /utf-8>)  # 源码与公共头含中文注释
target_link_libraries(anomaly_manual_map PUBLIC Advapi32 Psapi wintrust crypt32)

# The launcher's own injection and proxy paths are self-contained. The old official-launcher
# game-hook DLL is not part of this build.

add_library(anomaly_semver STATIC src/plugin/semver.cpp)
target_include_directories(anomaly_semver PUBLIC include)
target_compile_definitions(
    anomaly_semver PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_semver PRIVATE /W4 /permissive- /EHsc)

# The launcher's own update check. It reads a GitHub release, decides whether that release is newer
# than this build, and publishes the downloaded package over the installation. It sits beside the
# proxy installer because it reuses that installer's list of files an update must not overwrite.
add_library(anomaly_launcher_release STATIC src/launcher/release_update.cpp)
target_include_directories(anomaly_launcher_release PUBLIC include)
target_compile_definitions(
    anomaly_launcher_release PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_launcher_release PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_launcher_release
    PUBLIC anomaly_launcher_proxy anomaly_semver
    PRIVATE nlohmann_json::nlohmann_json)

add_library(anomaly_plugin_manifest STATIC
    src/plugin/plugin_manifest.cpp
    src/plugin/plugin_capability_policy.cpp)
target_include_directories(
    anomaly_plugin_manifest PUBLIC include PRIVATE "${GENERATED_DIR}")
target_compile_definitions(
    anomaly_plugin_manifest PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_manifest PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_plugin_manifest
    PUBLIC anomaly_semver
    PRIVATE nlohmann_json::nlohmann_json nlohmann_json_schema_validator::validator)

add_library(anomaly_plugin_profile STATIC src/plugin/plugin_profile.cpp)
target_include_directories(anomaly_plugin_profile PUBLIC include PRIVATE "${GENERATED_DIR}")
target_compile_definitions(anomaly_plugin_profile PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_profile PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_plugin_profile
    PUBLIC anomaly_reliable_storage nlohmann_json::nlohmann_json
    PRIVATE nlohmann_json_schema_validator::validator)

add_library(anomaly_plugin_compatibility STATIC src/plugin/plugin_compatibility.cpp)
target_include_directories(anomaly_plugin_compatibility PUBLIC include)
target_compile_definitions(
    anomaly_plugin_compatibility PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_compatibility PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_plugin_compatibility PUBLIC anomaly_plugin_manifest)

add_library(anomaly_plugin_package STATIC src/plugin/plugin_package.cpp)
target_include_directories(anomaly_plugin_package PUBLIC include)
target_compile_definitions(
    anomaly_plugin_package PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_package PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释

add_library(anomaly_artifact_bundle STATIC src/repository/artifact_bundle.cpp)
target_include_directories(anomaly_artifact_bundle PUBLIC include)
target_compile_definitions(
    anomaly_artifact_bundle PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_artifact_bundle PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码含中文注释
target_link_libraries(anomaly_artifact_bundle PUBLIC anomaly_artifact_crypto anomaly_plugin_package)

add_library(anomaly_repository_index STATIC src/repository/repository_index.cpp)
target_include_directories(
    anomaly_repository_index PUBLIC include PRIVATE "${GENERATED_DIR}")
target_compile_definitions(
    anomaly_repository_index PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_repository_index PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_repository_index
    PUBLIC anomaly_semver anomaly_artifact_crypto
    PRIVATE nlohmann_json::nlohmann_json nlohmann_json_schema_validator::validator)

add_library(anomaly_repository_configuration STATIC
    src/repository/repository_configuration.cpp)
target_include_directories(
    anomaly_repository_configuration PUBLIC include PRIVATE "${GENERATED_DIR}")
target_compile_definitions(
    anomaly_repository_configuration PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_repository_configuration PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_repository_configuration
    PUBLIC anomaly_repository_index
    PRIVATE nlohmann_json::nlohmann_json nlohmann_json_schema_validator::validator)

if(ANOMALY_BUILD_OPTIONAL_LIBS)
    add_library(anomaly_repository_policy STATIC
        src/repository/repository_policy.cpp)
    target_include_directories(
        anomaly_repository_policy PUBLIC include PRIVATE "${GENERATED_DIR}")
    target_compile_definitions(
        anomaly_repository_policy PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(anomaly_repository_policy PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(anomaly_repository_policy
        PUBLIC anomaly_repository_configuration
        PRIVATE nlohmann_json::nlohmann_json nlohmann_json_schema_validator::validator)
endif()

add_library(anomaly_runtime_launch STATIC
    src/repository/runtime_launch.cpp)
target_include_directories(anomaly_runtime_launch PUBLIC include)
target_compile_definitions(
    anomaly_runtime_launch PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_runtime_launch PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释

if(ANOMALY_BUILD_OPTIONAL_LIBS)
    add_library(anomaly_update_transaction STATIC src/repository/update_transaction.cpp)
    target_include_directories(anomaly_update_transaction PUBLIC include)
    target_compile_definitions(
        anomaly_update_transaction PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(anomaly_update_transaction PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码含中文注释
    target_link_libraries(anomaly_update_transaction PUBLIC
        anomaly_repository_index anomaly_artifact_bundle anomaly_plugin_manifest
        PRIVATE nlohmann_json::nlohmann_json)
endif()

add_library(anomaly_repository_network STATIC src/repository/repository_network.cpp)
target_include_directories(anomaly_repository_network PUBLIC include)
target_compile_definitions(
    anomaly_repository_network PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_repository_network PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释
target_link_libraries(anomaly_repository_network PUBLIC winhttp Shlwapi)

add_library(anomaly_repository_coordinator STATIC
    src/repository/repository_coordinator.cpp
    src/repository/plugin_repository_config.cpp
    src/repository/plugin_list.cpp
    src/repository/safe_zip.cpp)
target_include_directories(anomaly_repository_coordinator PUBLIC include)
target_compile_definitions(
    anomaly_repository_coordinator PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_repository_coordinator PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_repository_coordinator
    PUBLIC anomaly_repository_network anomaly_semver anomaly_miniz
        nlohmann_json::nlohmann_json
    PRIVATE anomaly_artifact_crypto anomaly_plugin_manifest anomaly_plugin_package)

add_library(anomaly_plugin_catalog STATIC src/plugin/plugin_catalog.cpp)
target_include_directories(anomaly_plugin_catalog PUBLIC include)
target_compile_definitions(
    anomaly_plugin_catalog PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_catalog PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释
target_link_libraries(
    anomaly_plugin_catalog PUBLIC anomaly_plugin_compatibility anomaly_plugin_package)

add_library(anomaly_plugin_dependency_resolver STATIC src/plugin/plugin_dependency_resolver.cpp)
target_include_directories(anomaly_plugin_dependency_resolver PUBLIC include)
target_compile_definitions(
    anomaly_plugin_dependency_resolver PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_dependency_resolver PRIVATE /W4 /permissive- /EHsc /utf-8)  # 依赖的公共头含中文注释
target_link_libraries(
    anomaly_plugin_dependency_resolver PUBLIC anomaly_plugin_catalog)

add_library(anomaly_plugin_enablement STATIC src/plugin/plugin_enablement.cpp)
target_include_directories(anomaly_plugin_enablement PUBLIC include)
target_compile_definitions(
    anomaly_plugin_enablement PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_enablement PRIVATE /W4 /permissive- /EHsc /utf-8)  # 依赖的公共头含中文注释
target_link_libraries(anomaly_plugin_enablement
    PUBLIC anomaly_plugin_catalog PRIVATE nlohmann_json::nlohmann_json)

add_library(anomaly_plugin_native_dependency STATIC src/plugin/plugin_native_dependency.cpp)
target_include_directories(anomaly_plugin_native_dependency PUBLIC include)
target_compile_definitions(
    anomaly_plugin_native_dependency PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_native_dependency PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_plugin_native_dependency PUBLIC Psapi)

add_library(anomaly_plugin_shadow STATIC
    src/plugin/plugin_shadow_store.cpp
    src/plugin/plugin_file_watcher.cpp
    src/plugin/plugin_config_watcher.cpp)
target_include_directories(anomaly_plugin_shadow PUBLIC include)
target_compile_definitions(
    anomaly_plugin_shadow PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_shadow PRIVATE /W4 /permissive- /EHsc /utf-8)  # 依赖的公共头含中文注释
target_link_libraries(
    anomaly_plugin_shadow PUBLIC anomaly_plugin_catalog anomaly_plugin_native_dependency)

add_library(anomaly_plugin_runtime STATIC
    src/plugin/ipc_registry.cpp
    src/plugin/plugin_scope.cpp
    src/plugin/plugin_runtime.cpp)
target_include_directories(anomaly_plugin_runtime PUBLIC include)
target_compile_definitions(
    anomaly_plugin_runtime PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_runtime PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释

add_library(anomaly_ui_resources STATIC
    src/ui/input_service.cpp
    src/ui/ui_resource_decoder.cpp
    src/ui/ui_resource_registry.cpp)
target_include_directories(anomaly_ui_resources PUBLIC include)
target_compile_definitions(
    anomaly_ui_resources PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_ui_resources PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_ui_resources PUBLIC anomaly_plugin_runtime ole32 shell32)

add_library(anomaly_hook_manager STATIC src/hook/hook_manager.cpp)
target_include_directories(anomaly_hook_manager PUBLIC include)
target_compile_definitions(
    anomaly_hook_manager PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_hook_manager PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释
target_link_libraries(anomaly_hook_manager PUBLIC anomaly_plugin_runtime)

add_library(anomaly_ui_service STATIC src/ui/ui_service_registry.cpp)
target_include_directories(anomaly_ui_service PUBLIC include)
target_compile_definitions(
    anomaly_ui_service PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_ui_service PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释

add_library(anomaly_platform_ui_model STATIC src/ui/platform_ui_model.cpp)
target_include_directories(anomaly_platform_ui_model PUBLIC include)
target_compile_definitions(
    anomaly_platform_ui_model PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_platform_ui_model PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释
target_link_libraries(
    anomaly_platform_ui_model PUBLIC anomaly_plugin_dependency_resolver
    anomaly_plugin_enablement anomaly_repository_coordinator)

add_library(anomaly_platform_settings STATIC src/ui/platform_settings.cpp)
target_include_directories(anomaly_platform_settings PUBLIC include)
target_compile_definitions(
    anomaly_platform_settings PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_platform_settings PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_platform_settings PUBLIC anomaly_reliable_storage
    PRIVATE nlohmann_json::nlohmann_json)

# Theme layout geometry and parsing. It has no ImGui or D3D dependency so the
# launcher, the in-game shell and the standalone verification can share it.
add_library(anomaly_platform_ui_layout STATIC src/ui/platform_ui_layout.cpp)
target_include_directories(anomaly_platform_ui_layout PUBLIC include)
target_compile_definitions(
    anomaly_platform_ui_layout PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_platform_ui_layout PRIVATE /W4 /permissive- /EHsc)

# First-launch notice: Markdown parser shared by the host and the preview.
add_library(anomaly_platform_notice STATIC src/ui/platform_notice.cpp)
target_include_directories(anomaly_platform_notice PUBLIC include)
target_compile_definitions(
    anomaly_platform_notice PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_platform_notice PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_platform_notice PRIVATE winhttp)
target_link_libraries(
    anomaly_platform_ui_layout PRIVATE nlohmann_json::nlohmann_json)

add_library(anomaly_minhook_backend STATIC src/hook/minhook_backend.cpp)
target_include_directories(anomaly_minhook_backend PUBLIC include)
target_compile_definitions(
    anomaly_minhook_backend PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_minhook_backend PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_minhook_backend PUBLIC anomaly_hook_manager minhook)

add_library(anomaly_scoped_platform STATIC src/plugin/scoped_platform_services.cpp)
target_include_directories(anomaly_scoped_platform PUBLIC include)
target_compile_definitions(
    anomaly_scoped_platform PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_scoped_platform PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_scoped_platform
    PUBLIC anomaly_internal_memory_services anomaly_reliable_storage anomaly_minhook_backend
    anomaly_signature_updater
    PRIVATE nlohmann_json::nlohmann_json nlohmann_json_schema_validator::validator)

add_library(anomaly_service_graph STATIC src/runtime/service_graph.cpp)
target_include_directories(anomaly_service_graph PUBLIC include)
target_compile_definitions(
    anomaly_service_graph PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_service_graph PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释
target_link_libraries(
    anomaly_runtime_kernel PUBLIC anomaly_service_graph anomaly_runtime_dispatchers)


add_library(anomaly_core SHARED
    src/runtime/core_main.cpp
    src/runtime/manual_map_exception.cpp
    src/ui/platform_host.cpp
    "${PLATFORM_UI_RESOURCES_RC}"
    src/render/dx12/embedded_bridge.cpp
    src/render/dx12/embedded_discovery.cpp
    src/render/dx12/embedded_input.cpp
    src/render/dx12/embedded_renderer.cpp
    src/render/dx12/embedded_ui_resource_render_backend.cpp
    src/ui/embedded_ui.cpp
    src/plugin/plugin_manager.cpp
    src/game/mayday/mayday_profile_runtime.cpp
)
set_target_properties(anomaly_core PROPERTIES
    OUTPUT_NAME "Anomaly.Core"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Anomaly"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/Anomaly")
target_include_directories(anomaly_core PRIVATE include src "${GENERATED_DIR}")
target_compile_definitions(anomaly_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_core PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码含中文注释
target_link_libraries(
    anomaly_core PRIVATE
    bcrypt
    d3dcompiler
    anomaly_telemetry
    anomaly_diagnostics
    anomaly_runtime_kernel
    anomaly_imgui
    anomaly_minhook_backend
    anomaly_ui_service
    anomaly_platform_ui_model
    anomaly_platform_settings
    anomaly_platform_ui_layout
    anomaly_platform_notice
    anomaly_scoped_platform
    anomaly_plugin_shadow
    anomaly_plugin_dependency_resolver
    anomaly_plugin_enablement
    anomaly_adapter_services
    anomaly_websocket_server
    anomaly_repository_coordinator
    anomaly_crash_reporter
    anomaly_structured_logger
    anomaly_ui_resources
    anomaly_i18n
    anomaly_plugin_profile
    nlohmann_json::nlohmann_json)

# The Core is manually mapped and cannot use MSVC's loader-managed TLS slot for
# function-local static guards. These targets initialize their process-scoped
# local statics on the lifecycle thread before publishing concurrent services.
# 名单必须覆盖最终链接进 anomaly_core 的全部本地静态库闭包（新 target 登记）。
set(ANOMALY_MANUAL_MAP_TLS_FREE_TARGETS
    anomaly_core
    anomaly_telemetry
    anomaly_diagnostics
    anomaly_config
    anomaly_i18n
    anomaly_internal_memory_services
    anomaly_memory
    anomaly_runtime_kernel
    anomaly_service_graph
    anomaly_runtime_dispatchers
    anomaly_dispatcher
    anomaly_imgui
    anomaly_minhook_backend
    anomaly_hook_manager
    anomaly_plugin_runtime
    anomaly_ui_service
    anomaly_platform_ui_model
    anomaly_plugin_dependency_resolver
    anomaly_plugin_catalog
    anomaly_plugin_compatibility
    anomaly_plugin_manifest
    anomaly_semver
    anomaly_plugin_package
    anomaly_plugin_enablement
    anomaly_repository_coordinator
    anomaly_repository_network
    anomaly_miniz
    anomaly_artifact_crypto
    anomaly_platform_settings
    anomaly_reliable_storage
    anomaly_platform_ui_layout
    anomaly_platform_notice
    anomaly_scoped_platform
    anomaly_signature_updater
    anomaly_plugin_shadow
    anomaly_plugin_native_dependency
    anomaly_adapter_services
    anomaly_websocket_server
    anomaly_crash_reporter
    anomaly_structured_logger
    anomaly_ui_resources
    anomaly_plugin_profile)
foreach(target IN LISTS ANOMALY_MANUAL_MAP_TLS_FREE_TARGETS)
    target_compile_options(${target} PRIVATE /Zc:threadSafeInit-)
endforeach()
add_custom_command(TARGET anomaly_core POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/CONTACT"
            "$<TARGET_FILE_DIR:anomaly_core>/CONTACT"
    COMMAND "${CMAKE_COMMAND}"
            "-DANOMALY_RUNTIME_ROOT=$<TARGET_FILE_DIR:anomaly_core>"
            -P "${CMAKE_SOURCE_DIR}/cmake/initialize_runtime_config.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/repository.json"
            "$<TARGET_FILE_DIR:anomaly_core>/repository.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/plugin-repositories.json"
            "$<TARGET_FILE_DIR:anomaly_core>/plugin-repositories.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/locales/host"
            "$<TARGET_FILE_DIR:anomaly_core>/locales/host"
    # The notice text ships as content, not as a compiled string.
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/notices"
            "$<TARGET_FILE_DIR:anomaly_core>/notices"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/fonts"
            "$<TARGET_FILE_DIR:anomaly_core>/assets/fonts"
    # Theme artwork and presets ship with the runtime too: the preview fixture
    # deploys them and the injected host resolves the same entry points.
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/images"
            "$<TARGET_FILE_DIR:anomaly_core>/assets/images"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/config/themes"
            "$<TARGET_FILE_DIR:anomaly_core>/config/themes")

add_library(dwmapi SHARED
    src/bootstrap/dwmapi/proxy_main.cpp
    "${PROXY_CPP}"
    "${PROXY_ASM}"
    "${PROXY_DEF}"
)
set_target_properties(dwmapi PROPERTIES
    OUTPUT_NAME dwmapi
    ARCHIVE_OUTPUT_NAME anomaly_dwmapi
    PREFIX "")
target_include_directories(dwmapi PRIVATE include "${GENERATED_DIR}")
target_compile_definitions(dwmapi PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(dwmapi PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /EHsc /utf-8>)  # 源码含中文注释
target_link_libraries(dwmapi PRIVATE anomaly_bootstrap_client anomaly_runtime_launch)
add_dependencies(dwmapi anomaly_core)

add_executable(anomaly_cli apps/cli/main.cpp)
set_target_properties(anomaly_cli PROPERTIES OUTPUT_NAME "anomaly-cli")
target_compile_definitions(anomaly_cli PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_cli PRIVATE /W4 /permissive- /EHsc)

add_executable(anomaly_inspect apps/cli/inspect_main.cpp)
set_target_properties(anomaly_inspect PROPERTIES OUTPUT_NAME "anomaly-inspect")
target_compile_definitions(anomaly_inspect PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_inspect PRIVATE /W4 /permissive- /EHsc)

add_executable(anomaly-plugin apps/plugin_tool/main.cpp)
set_target_properties(anomaly-plugin PROPERTIES OUTPUT_NAME "anomaly-plugin")
target_include_directories(anomaly-plugin PRIVATE include)
target_compile_definitions(anomaly-plugin PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly-plugin PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly-plugin PRIVATE
    anomaly_plugin_manifest anomaly_plugin_package anomaly_artifact_bundle bcrypt)

add_executable(anomaly_abi_snapshot apps/abi_snapshot/main.cpp)
set_target_properties(anomaly_abi_snapshot PROPERTIES OUTPUT_NAME "anomaly-abi-snapshot")
target_link_libraries(anomaly_abi_snapshot PRIVATE Anomaly::sdk)
target_compile_options(anomaly_abi_snapshot PRIVATE /W4 /permissive- /EHsc /utf-8)  # 源码与公共头含中文注释

# Attaches the runtime core to a game that is already running. It shares the mapper the launcher uses,
# so the attach it performs is the same one the launcher's live-attach mode performs.
add_executable(anomaly_injector apps/injector/main.cpp)
set_target_properties(anomaly_injector PROPERTIES OUTPUT_NAME "anomaly-inject")
target_include_directories(anomaly_injector PRIVATE include)
target_compile_definitions(anomaly_injector PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_injector PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_injector PRIVATE anomaly_manual_map)

add_executable(anomaly_test_host apps/test_host/main.cpp src/plugin/plugin_manager.cpp)
set_target_properties(anomaly_test_host PROPERTIES OUTPUT_NAME "anomaly-test-host")
target_include_directories(anomaly_test_host PRIVATE include)
target_compile_definitions(anomaly_test_host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_test_host PRIVATE /W4 /permissive- /EHsc /utf-8)  # 依赖的公共头含中文注释
target_link_libraries(anomaly_test_host PRIVATE
    anomaly_diagnostics anomaly_plugin_shadow anomaly_plugin_dependency_resolver anomaly_plugin_enablement
    anomaly_adapter_services anomaly_plugin_runtime
    anomaly_structured_logger anomaly_scoped_platform anomaly_ui_resources
    anomaly_i18n anomaly_plugin_profile nlohmann_json::nlohmann_json Psapi)

if(ANOMALY_BUILD_TEST_FIXTURES)
    enable_testing()
    anomaly_add_plugin(anomaly_test_hook_lifecycle_plugin NO_RELEASE
        SOURCES apps/test_host/hook_lifecycle_fixture_plugin.cpp
        MANIFEST apps/test_host/hook_lifecycle_fixture_manifest.json
        PACKAGE_NAME HookLifecycleFixture
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fixtures/HookLifecycleFixture")
    add_dependencies(anomaly_test_host anomaly_test_hook_lifecycle_plugin)
    add_test(NAME plugin_hook_stop_order
        COMMAND anomaly_test_host --hook-stop-order-fixture --plugin
            "$<TARGET_FILE_DIR:anomaly_test_hook_lifecycle_plugin>")
    add_test(NAME plugin_enablement_drain
        COMMAND anomaly_test_host --enablement-drain-fixture --plugin
            "$<TARGET_FILE_DIR:anomaly_test_hook_lifecycle_plugin>")
    add_executable(anomaly_d3d12_interop_fixture
        apps/render_fixture/d3d12_interop_fixture.cpp)
    target_include_directories(anomaly_d3d12_interop_fixture PRIVATE include src)
    target_compile_definitions(anomaly_d3d12_interop_fixture PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(anomaly_d3d12_interop_fixture PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(anomaly_d3d12_interop_fixture PRIVATE d3d12 dxgi user32)
    add_test(NAME d3d12_interop COMMAND anomaly_d3d12_interop_fixture)
    add_executable(anomaly-platform-preview WIN32        apps/platform_preview/main.cpp
        src/ui/platform_host.cpp
        src/game/mayday/mayday_profile_runtime.cpp
        "${PLATFORM_UI_RESOURCES_RC}"
        src/plugin/plugin_manager.cpp)
    set_target_properties(anomaly-platform-preview PROPERTIES
        OUTPUT_NAME "anomaly-platform-preview")
    target_include_directories(anomaly-platform-preview PRIVATE include src "${GENERATED_DIR}" "")
    target_compile_definitions(anomaly-platform-preview PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(anomaly-platform-preview PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(
        anomaly-platform-preview PRIVATE
        anomaly_diagnostics anomaly_imgui anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_platform_ui_model anomaly_plugin_enablement anomaly_adapter_services
        anomaly_platform_settings anomaly_platform_ui_layout anomaly_platform_notice
        anomaly_plugin_runtime
        anomaly_structured_logger anomaly_scoped_platform anomaly_ui_resources
        anomaly_i18n anomaly_plugin_profile nlohmann_json::nlohmann_json ole32 shell32 comdlg32)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/launcher_archive.cmake")

add_executable(anomaly_launcher WIN32
    apps/launcher/main.cpp
    apps/launcher/AnomalyLauncher.manifest
    "${LAUNCHER_RESOURCES_RC}")
set_target_properties(anomaly_launcher PROPERTIES OUTPUT_NAME "AnomalyLauncher")
target_link_libraries(anomaly_launcher PRIVATE winhttp anomaly_launcher_archive)
target_link_libraries(anomaly_launcher PRIVATE nlohmann_json::nlohmann_json anomaly_adapter_services)
target_link_libraries(anomaly_launcher PRIVATE advapi32)
# The launcher carries a few user-facing strings inline; /utf-8 keeps them valid.
target_compile_options(anomaly_launcher PRIVATE /utf-8)
target_include_directories(anomaly_launcher PRIVATE include)
target_compile_definitions(
    anomaly_launcher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_launcher PRIVATE /W4 /permissive- /EHsc)
target_link_options(anomaly_launcher PRIVATE /MANIFESTUAC:NO /DELAYLOAD:dwmapi.dll)
target_link_libraries(anomaly_launcher PRIVATE
    anomaly_telemetry
    anomaly_config anomaly_imgui anomaly_launcher_configuration anomaly_launcher_proxy
    anomaly_launcher_release anomaly_manual_map
    anomaly_platform_notice anomaly_platform_settings anomaly_platform_ui_layout
    anomaly_runtime_launch anomaly_ui_resources
    ole32 shell32 delayimp dcomp)
# The runtime payload the launcher is installed with is complete without a plugin hook, so the launcher
# depends only on the pieces it actually loads.
add_dependencies(anomaly_launcher dwmapi anomaly_core)

option(ANOMALY_BUILD_SDK_EXAMPLES "Build the Anomaly 1.0 SDK example plugins" ON)
option(ANOMALY_BUILD_BUILTIN_PLUGINS "Build Anomaly bundled plugins" ON)
# 暂无发布产物消费者的可选静态库（网络抓包、音频解码等实验组件）。默认关闭
# 以跳过它们的编译；需要单独验证或接入这些库时置 ON，产物集合不变。
option(ANOMALY_BUILD_OPTIONAL_LIBS
    "Build optional static libraries that no shipped target consumes" OFF)
# Developer test plugin packages are neither built nor installed unless asked for. They
# install through the TestPlugins component alone, which no release component stages
# (`tools/package_release.ps1` installs GameRuntime, SDK and Tools), and building them
# would cost time on every default build. `build.cmd testplugins` turns both on.
option(ANOMALY_BUILD_TEST_PLUGINS
    "Build and install the developer test plugin packages" OFF)
set(ANOMALY_PLATFORM_PREVIEW_STAGE_COMMANDS
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/locales/host"
            "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly/locales/host"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/notices"
            "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly/notices"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/fonts"
            "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly/assets/fonts"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/images"
            "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly/assets/images")

# Theme presets are shipped content, so the whole directory is deployed exactly
# like locales and artwork. `layout.json` is deliberately absent from the source
# directory: it is the shared document the running build creates the first time a
# background is changed, and it lives in the deployed tree only.
list(APPEND ANOMALY_PLATFORM_PREVIEW_STAGE_COMMANDS
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/config/themes"
            "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly/config/themes")

if(ANOMALY_BUILD_SDK_EXAMPLES)
    # The SDK examples ship as sources in the SDK component; they are built without
    # installing a runtime package.
    anomaly_add_plugin(anomaly_example_hello_ui C_ONLY NO_RELEASE
        SOURCES examples/hello_ui/plugin.c
        MANIFEST examples/hello_ui/manifest.json
        PACKAGE_NAME HelloUi
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sdk-examples/HelloUi")
    target_compile_options(anomaly_example_hello_ui PRIVATE /utf-8)  # 示例源码含中文注释
    anomaly_add_plugin(anomaly_example_tick_counter NO_RELEASE
        SOURCES examples/tick_counter/plugin.cpp
        MANIFEST examples/tick_counter/manifest.json
        PACKAGE_NAME TickCounter
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sdk-examples/TickCounter")
    target_compile_options(anomaly_example_tick_counter PRIVATE /utf-8)  # 示例源码含中文注释
    anomaly_add_plugin(anomaly_example_reliable_config NO_RELEASE
        SOURCES examples/reliable_config/plugin.cpp
        MANIFEST examples/reliable_config/manifest.json
        PACKAGE_NAME ReliableConfig
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sdk-examples/ReliableConfig")
    target_compile_options(anomaly_example_reliable_config PRIVATE /utf-8)  # 示例源码含中文注释
endif()

if(ANOMALY_BUILD_BUILTIN_PLUGINS)
    # Mayday 内建插件：金币/债务修改。il2cpp 细节由宿主 mayday.money 服务封装。
    anomaly_add_plugin(anomaly_builtin_money
        SOURCES plugins/money/plugin.cpp
        MANIFEST plugins/money/manifest.json
        PACKAGE_NAME Money
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/builtin-plugins/Money")
    target_compile_options(anomaly_builtin_money PRIVATE /utf-8)

    # Mayday 内建插件：自由镜头。WASD/空格/Shift + 鼠标，热键可绑定。
    anomaly_add_plugin(anomaly_builtin_freecam
        SOURCES plugins/freecam/plugin.cpp
        MANIFEST plugins/freecam/manifest.json
        PACKAGE_NAME Freecam
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/builtin-plugins/Freecam")
    target_compile_options(anomaly_builtin_freecam PRIVATE /utf-8)

    # Mayday 内建插件：气象调节。时间/天气调节与锁定，il2cpp 细节由宿主
    # mayday.weather 服务封装。
    anomaly_add_plugin(anomaly_builtin_weather
        SOURCES plugins/weather/plugin.cpp
        MANIFEST plugins/weather/manifest.json
        PACKAGE_NAME Weather
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/builtin-plugins/Weather")
    target_compile_options(anomaly_builtin_weather PRIVATE /utf-8)

    # Mayday 内建插件：自动棒球棍。手持棒球棍时自动锁定攻击范围内最近的
    # 乘客，以 Mirror Command 形式发送使用请求；服务端命中判定期间临时朝向
    # 目标，画面视角不动。il2cpp/钩子细节由宿主 mayday.combat 服务封装。
    anomaly_add_plugin(anomaly_builtin_autobat
        SOURCES plugins/autobat/plugin.cpp
        MANIFEST plugins/autobat/manifest.json
        PACKAGE_NAME Autobat
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/builtin-plugins/Autobat")
    target_compile_options(anomaly_builtin_autobat PRIVATE /utf-8)

    if(ANOMALY_BUILD_TEST_FIXTURES)
        add_executable(anomaly_ui_overlay_fixture
            apps/test_host/ui_overlay_fixture.cpp src/plugin/plugin_manager.cpp)
        target_include_directories(anomaly_ui_overlay_fixture PRIVATE include)
        target_compile_definitions(anomaly_ui_overlay_fixture PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_options(anomaly_ui_overlay_fixture PRIVATE /utf-8 /W4 /permissive- /EHsc)
        target_link_libraries(anomaly_ui_overlay_fixture PRIVATE
            anomaly_diagnostics anomaly_plugin_shadow anomaly_plugin_dependency_resolver anomaly_plugin_enablement
            anomaly_adapter_services anomaly_plugin_runtime
            anomaly_structured_logger anomaly_scoped_platform anomaly_ui_resources
            anomaly_i18n anomaly_plugin_profile nlohmann_json::nlohmann_json)
    endif()

    # Test packages exist only when ANOMALY_BUILD_TEST_PLUGINS is on, so every list
    # that reaches their targets is built from the targets that actually exist.
    # 内建插件不再维护额外的 target 列表：anomaly_add_plugin 已在 CMake 层统一
    # 完成 include/宏/打包/安装注册（ANOMALY_RELEASE_PLUGIN_TARGETS）。
endif()

if(TARGET anomaly-platform-preview)
    # Use the package metadata owned by the installation rules. NO_RELEASE examples
    # and TestPlugins do not belong to the GameRuntime preview.
    get_property(preview_candidates GLOBAL PROPERTY ANOMALY_RELEASE_PLUGIN_TARGETS)
    set(ANOMALY_PREVIEW_PACKAGE_NAMES)
    set(ANOMALY_PREVIEW_PACKAGE_COMMANDS "")
    set(preview_plugin_targets)
    foreach(preview_plugin IN LISTS preview_candidates)
        get_target_property(preview_component ${preview_plugin} ANOMALY_PACKAGE_INSTALL_COMPONENT)
        if(NOT preview_component STREQUAL "GameRuntime")
            continue()
        endif()
        list(APPEND preview_plugin_targets ${preview_plugin})
        get_target_property(preview_name ${preview_plugin} ANOMALY_PACKAGE_NAME)
        get_target_property(preview_manifest ${preview_plugin} ANOMALY_PACKAGE_MANIFEST)
        get_target_property(preview_resources ${preview_plugin} ANOMALY_PACKAGE_RESOURCE_DIRECTORIES)
        get_target_property(preview_bundled_targets ${preview_plugin} ANOMALY_PACKAGE_BUNDLED_TARGETS)
        get_filename_component(preview_source "${preview_manifest}" DIRECTORY)
        list(APPEND ANOMALY_PREVIEW_PACKAGE_NAMES "${preview_name}")
        string(APPEND ANOMALY_PREVIEW_PACKAGE_COMMANDS
            "file(COPY_FILE [==[$<TARGET_FILE:${preview_plugin}>]==] \"\${preview_plugins}/${preview_name}/plugin.dll\" ONLY_IF_DIFFERENT)\n"
            "file(COPY_FILE [==[${preview_manifest}]==] \"\${preview_plugins}/${preview_name}/manifest.json\" ONLY_IF_DIFFERENT)\n")
        foreach(preview_resource IN LISTS preview_resources)
            string(APPEND ANOMALY_PREVIEW_PACKAGE_COMMANDS
                "file(COPY [==[${preview_source}/${preview_resource}]==] DESTINATION \"\${preview_plugins}/${preview_name}\")\n")
        endforeach()
        foreach(preview_bundled_target IN LISTS preview_bundled_targets)
            string(APPEND ANOMALY_PREVIEW_PACKAGE_COMMANDS
                "file(COPY_FILE [==[$<TARGET_FILE:${preview_bundled_target}>]==] \"\${preview_plugins}/${preview_name}/$<TARGET_FILE_NAME:${preview_bundled_target}>\" ONLY_IF_DIFFERENT)\n")
        endforeach()
    endforeach()
    configure_file(cmake/stage_platform_preview.cmake.in
        "${GENERATED_DIR}/stage_platform_preview.cmake.in" @ONLY)
    file(GENERATE OUTPUT "${GENERATED_DIR}/stage_platform_preview-$<CONFIG>.cmake"
        INPUT "${GENERATED_DIR}/stage_platform_preview.cmake.in")
    # A custom target also runs when only JSON/assets changed or the executable is
    # already current; POST_BUILD alone leaves those resources stale.
    add_custom_target(anomaly_stage_platform_preview
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly"
        COMMAND "${CMAKE_COMMAND}"
                "-DANOMALY_PREVIEW_ROOT=$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly"
                -P "${GENERATED_DIR}/stage_platform_preview-$<CONFIG>.cmake"
        ${ANOMALY_PLATFORM_PREVIEW_STAGE_COMMANDS}
        DEPENDS ${preview_plugin_targets}
        VERBATIM)
    add_dependencies(anomaly-platform-preview anomaly_stage_platform_preview)
endif()

set(ANOMALY_RELEASE_PE_TARGETS
    dwmapi anomaly_core anomaly_launcher
    anomaly-plugin anomaly_abi_snapshot anomaly_test_host
    anomaly_cli anomaly_inspect anomaly_injector)
get_property(ANOMALY_RELEASE_PLUGIN_TARGETS GLOBAL PROPERTY ANOMALY_RELEASE_PLUGIN_TARGETS)
list(REMOVE_DUPLICATES ANOMALY_RELEASE_PLUGIN_TARGETS)
foreach(runtime_plugin_target IN LISTS ANOMALY_RELEASE_PLUGIN_TARGETS)
    if(TARGET ${runtime_plugin_target})
        list(APPEND ANOMALY_RELEASE_PE_TARGETS ${runtime_plugin_target})
    endif()
endforeach()
if(MSVC AND ANOMALY_BUILD_SYMBOLS)
    # Keep the CodeView record embedded in each published PE independent of the
    # build-machine directory. The full PDB remains in the Symbols component.
    foreach(release_target IN LISTS ANOMALY_RELEASE_PE_TARGETS)
        target_link_options(${release_target} PRIVATE
            "/PDBALTPATH:$<TARGET_PDB_FILE_NAME:${release_target}>")
    endforeach()
endif()

install(TARGETS dwmapi
    RUNTIME DESTINATION . COMPONENT GameRuntime
    LIBRARY DESTINATION . COMPONENT GameRuntime)
install(TARGETS anomaly_launcher
    RUNTIME DESTINATION . COMPONENT GameRuntime)
install(TARGETS anomaly_core
    RUNTIME DESTINATION Anomaly COMPONENT GameRuntime
    LIBRARY DESTINATION Anomaly COMPONENT GameRuntime)
# Plugin packages install themselves: `anomaly_add_plugin` emits the package rules for
# `Anomaly/plugins/<PackageName>` in the GameRuntime component, or in the opt-in
# TestPlugins component for TEST_PLUGIN packages, so a registered plugin cannot be
# missing from the runtime tree.
install(FILES CONTACT config/anomaly.ini config/repository.json config/plugin-repositories.json
    DESTINATION Anomaly COMPONENT GameRuntime)
install(DIRECTORY locales/host
    DESTINATION Anomaly/locales COMPONENT GameRuntime)
install(DIRECTORY notices
    DESTINATION Anomaly COMPONENT GameRuntime)
install(FILES assets/fonts/NotoSansCJKsc-Regular.ttf
    DESTINATION Anomaly/assets/fonts COMPONENT GameRuntime)
# Theme artwork and presets ship with the package: the game runtime and the
# launcher both resolve `assets/images/...` and `config/themes/...` against the
# same `Anomaly/` root. Users replace or extend these with their own files.
install(DIRECTORY assets/images/
    DESTINATION Anomaly/assets/images COMPONENT GameRuntime)
install(DIRECTORY config/themes/
    DESTINATION Anomaly/config/themes COMPONENT GameRuntime)
install(DIRECTORY profiles/
    DESTINATION Anomaly/profiles COMPONENT GameRuntime)
install(TARGETS anomaly_cli anomaly_inspect anomaly-plugin anomaly_abi_snapshot anomaly_test_host
    anomaly_injector
    RUNTIME DESTINATION . COMPONENT Tools)
# The layout editor is one offline HTML file: it needs no runtime, no network and
# no build step, so it ships as data next to the command line tools.
install(FILES tools/theme_layout_editor/index.html
    DESTINATION theme-editor COMPONENT Tools)
install(TARGETS anomaly_sdk EXPORT AnomalySDKTargets COMPONENT SDK)
install(EXPORT AnomalySDKTargets
    FILE AnomalySDKTargets.cmake
    NAMESPACE Anomaly::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AnomalySDK
    COMPONENT SDK)
install(DIRECTORY include/anomaly/sdk/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/anomaly/sdk COMPONENT SDK
    PATTERN "version.h" EXCLUDE)
install(FILES "${ANOMALY_GENERATED_INSTALL_INCLUDE_DIR}/anomaly/sdk/version.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/anomaly/sdk COMPONENT SDK)
install(FILES schemas/plugin-manifest.schema.json schemas/plugin-profile.schema.json
    schemas/repository-config.schema.json
    schemas/repository-index.schema.json schemas/plugin-enablement.schema.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/anomaly/schemas COMPONENT SDK)
install(FILES abi/anomaly-sdk-v1-windows-x64.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/anomaly/abi COMPONENT SDK)
set(ANOMALY_SDK_EXAMPLES_DESTINATION "${CMAKE_INSTALL_DATADIR}/anomaly/examples")
install(FILES examples/CMakeLists.txt examples/README.md
    DESTINATION ${ANOMALY_SDK_EXAMPLES_DESTINATION} COMPONENT SDK)
install(FILES examples/hello_ui/plugin.c examples/hello_ui/manifest.json
    DESTINATION ${ANOMALY_SDK_EXAMPLES_DESTINATION}/hello_ui COMPONENT SDK)
install(DIRECTORY examples/hello_ui/locales
    DESTINATION ${ANOMALY_SDK_EXAMPLES_DESTINATION}/hello_ui COMPONENT SDK)
install(FILES examples/tick_counter/plugin.cpp examples/tick_counter/manifest.json
    DESTINATION ${ANOMALY_SDK_EXAMPLES_DESTINATION}/tick_counter COMPONENT SDK)
install(FILES examples/reliable_config/plugin.cpp examples/reliable_config/manifest.json
    DESTINATION ${ANOMALY_SDK_EXAMPLES_DESTINATION}/reliable_config COMPONENT SDK)
configure_package_config_file(
    cmake/sdk/AnomalySDKConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/AnomalySDKConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AnomalySDK)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/AnomalySDKConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/AnomalySDKConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/AnomalySDKConfigVersion.cmake"
    cmake/sdk/AnomalyPlugin.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AnomalySDK COMPONENT SDK)
set(ANOMALY_THIRD_PARTY_LICENSE_FILES
    third_party/licenses/imgui-LICENSE.txt
    third_party/licenses/minhook-LICENSE.txt
    third_party/licenses/nlohmann-json-LICENSE.txt
    third_party/licenses/json-schema-validator-LICENSE.txt
    third_party/licenses/noto-sans-cjk-LICENSE.txt
    third_party/licenses/miniaudio-LICENSE.txt
    third_party/licenses/rtaudio-LICENSE.txt
    third_party/licenses/monkeys-audio-LICENSE.txt
    third_party/licenses/dsd2pcm-LICENSE.txt
    third_party/licenses/libarchive-LICENSE.txt
    third_party/licenses/liblzma-LICENSE.txt
    third_party/licenses/zlib-LICENSE.txt)
set(ANOMALY_RELEASE_COMPONENTS GameRuntime SDK Tools)
if(ANOMALY_BUILD_SYMBOLS)
    list(APPEND ANOMALY_RELEASE_COMPONENTS Symbols)
endif()
foreach(anomaly_release_component IN LISTS ANOMALY_RELEASE_COMPONENTS)
    install(FILES LICENSE NOTICE
        DESTINATION . COMPONENT ${anomaly_release_component})
    install(FILES ${ANOMALY_THIRD_PARTY_LICENSE_FILES}
        DESTINATION third_party/licenses COMPONENT ${anomaly_release_component})
endforeach()
if(ANOMALY_BUILD_SYMBOLS)
    foreach(symbol_target IN LISTS ANOMALY_RELEASE_PE_TARGETS)
        install(FILES "$<TARGET_PDB_FILE:${symbol_target}>"
            DESTINATION symbols COMPONENT Symbols)
    endforeach()
endif()
