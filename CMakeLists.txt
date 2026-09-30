cmake_minimum_required(VERSION 3.12)
project(rehlds_netdiag CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REAPI_ROOT "${CMAKE_SOURCE_DIR}/deps/ReAPI/reapi")
set(METAMOD_ROOT "${CMAKE_SOURCE_DIR}/deps/Metamod-R/metamod")

if(NOT EXISTS "${REAPI_ROOT}/include/cssdk/engine/rehlds_api.h")
    message(FATAL_ERROR "ReAPI dependency is missing. Run scripts/fetch_deps.sh first.")
endif()

if(NOT EXISTS "${METAMOD_ROOT}/src/meta_api.h")
    message(FATAL_ERROR "Metamod-R dependency is missing. Run scripts/fetch_deps.sh first.")
endif()

add_library(rehlds_netdiag SHARED
    src/rehlds_netdiag.cpp
)

target_include_directories(rehlds_netdiag PRIVATE
    "${REAPI_ROOT}/include"
    "${REAPI_ROOT}/include/cssdk/common"
    "${REAPI_ROOT}/include/cssdk/dlls"
    "${REAPI_ROOT}/include/cssdk/engine"
    "${REAPI_ROOT}/include/cssdk/game_shared"
    "${REAPI_ROOT}/include/cssdk/public"
    "${REAPI_ROOT}/include/cssdk/pm_shared"
    "${METAMOD_ROOT}/src"
)

target_compile_definitions(rehlds_netdiag PRIVATE
    _LINUX
    LINUX
    NDEBUG
    _GLIBCXX_USE_CXX11_ABI=0
    HAVE_STRONG_TYPEDEF
    _stricmp=strcasecmp
    _strnicmp=strncasecmp
    _vsnprintf=vsnprintf
    _snprintf=snprintf
)

target_compile_options(rehlds_netdiag PRIVATE
    -m32
    -O2
    -fPIC
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fno-exceptions
    -fno-rtti
    -fcf-protection=none
)

target_link_options(rehlds_netdiag PRIVATE
    -m32
    -Wl,--version-script=${CMAKE_SOURCE_DIR}/version_script.lds
)

target_link_libraries(rehlds_netdiag PRIVATE dl)

set_target_properties(rehlds_netdiag PROPERTIES
    PREFIX ""
    OUTPUT_NAME "rehlds_netdiag_mm_i386"
)
