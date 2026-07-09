cmake_minimum_required(VERSION 3.24)

project(KWCorridorKeyPlugins VERSION 0.1.0 LANGUAGES CXX)

option(KWCORRIDORKEY_BUILD_OPENFX "Build DaVinci Resolve OpenFX plugin" ON)
option(KWCORRIDORKEY_BUILD_PREMIERE "Build Adobe Premiere plugin when SDK is available" ON)
option(KWCORRIDORKEY_BUILD_TESTS "Build native tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
include(GNUInstallDirs)
find_package(Python3 COMPONENTS Interpreter)

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(corridorkey_client
    native/src/FrameBufferFile.cpp
    native/src/WorkerClient.cpp
)
target_include_directories(corridorkey_client PUBLIC native/include)
target_link_libraries(corridorkey_client PUBLIC nlohmann_json::nlohmann_json)
if(CMAKE_DL_LIBS)
    target_link_libraries(corridorkey_client PRIVATE ${CMAKE_DL_LIBS})
endif()
target_compile_definitions(corridorkey_client PRIVATE
    KWCORRIDORKEY_VERSION="${PROJECT_VERSION}"
)
if(WIN32)
    target_compile_definitions(corridorkey_client PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(KWCORRIDORKEY_BUILD_OPENFX)
    include(cmake/FetchOpenFX.cmake)
    add_executable(ofx_load_probe native/tools/OfxLoadProbe.cpp)
    target_include_directories(ofx_load_probe PRIVATE ${OPENFX_INCLUDE_DIR})

    add_library(CorridorKeyResolve MODULE
        plugins/resolve/src/CorridorKeyResolve.cpp
    )
    target_include_directories(CorridorKeyResolve PRIVATE ${OPENFX_INCLUDE_DIR})
    target_link_libraries(CorridorKeyResolve PRIVATE corridorkey_client)
    set_target_properties(CorridorKeyResolve PROPERTIES
        PREFIX ""
        OUTPUT_NAME "CorridorKeyResolve"
        SUFFIX ".ofx"
    )
    if(WIN32)
        set(_ofx_platform_dir "Win64")
    elseif(APPLE)
        set(_ofx_platform_dir "MacOS")
    else()
        set(_ofx_platform_dir "Linux-x86-64")
    endif()
    set(_ofx_bundle_dir "${CMAKE_CURRENT_BINARY_DIR}/CorridorKeyResolve.ofx.bundle/Contents/${_ofx_platform_dir}")
    add_custom_command(TARGET CorridorKeyResolve POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_ofx_bundle_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:CorridorKeyResolve>" "${_ofx_bundle_dir}/$<TARGET_FILE_NAME:CorridorKeyResolve>"
        COMMENT "Creating CorridorKeyResolve.ofx.bundle"
    )
    if(KWCORRIDORKEY_BUILD_TESTS)
        add_test(
            NAME ofx_load_probe_resolve
            COMMAND ofx_load_probe "$<TARGET_FILE:CorridorKeyResolve>"
        )
    endif()
endif()

if(KWCORRIDORKEY_BUILD_PREMIERE)
    include(cmake/FindAdobeSDK.cmake)
    if(ADOBE_SDK_FOUND)
        add_library(CorridorKeyPremiere MODULE
            plugins/premiere/src/CorridorKeyPremiere.cpp
        )
        target_include_directories(CorridorKeyPremiere PRIVATE ${ADOBE_SDK_INCLUDE_DIRS})
        target_link_libraries(CorridorKeyPremiere PRIVATE corridorkey_client)
        set_target_properties(CorridorKeyPremiere PROPERTIES
            PREFIX ""
            OUTPUT_NAME "CorridorKeyPremiere"
            SUFFIX ".aex"
        )
    else()
        message(STATUS "Adobe SDK not found. Set ADOBE_AE_SDK_ROOT or ADOBE_PREMIERE_SDK_ROOT to build Premiere plugin.")
    endif()
endif()

if(Python3_Interpreter_FOUND)
    add_custom_target(package_blender_addon
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package_blender_addon.py
                --source ${CMAKE_CURRENT_SOURCE_DIR}/plugins/blender
                --output ${CMAKE_CURRENT_BINARY_DIR}/CorridorKeyBlender.zip
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/CorridorKeyBlender.zip
        COMMENT "Packaging Blender add-on"
    )
else()
    message(STATUS "Python3 interpreter not found. Blender add-on packaging target disabled.")
endif()

if(KWCORRIDORKEY_BUILD_TESTS)
    enable_testing()
    add_executable(corridorkey_client_tests native/tests/WorkerClientTests.cpp)
    target_link_libraries(corridorkey_client_tests PRIVATE corridorkey_client)
    add_test(NAME corridorkey_client_tests COMMAND corridorkey_client_tests)
endif()
