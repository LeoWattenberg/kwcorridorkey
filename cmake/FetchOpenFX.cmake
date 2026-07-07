include(FetchContent)

FetchContent_Declare(
    openfx
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openfx.git
    GIT_TAG OFX_Release_1.5.1
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(openfx)
if(NOT openfx_POPULATED)
    FetchContent_Populate(openfx)
endif()

set(OPENFX_INCLUDE_DIR "${openfx_SOURCE_DIR}/include" CACHE PATH "OpenFX include directory")
