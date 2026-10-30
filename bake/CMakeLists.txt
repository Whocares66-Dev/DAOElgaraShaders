cmake_minimum_required(VERSION 3.21)

project(DAOElgaraShadersBake VERSION 0.1.0 LANGUAGES CXX)

# The relighter as a 64-bit program of its own (issue 5). The plugin is 32-bit
# because the game is, and its build makes a 32-bit elgarashaders_relight
# beside it; but a bake reads no game and is held to nothing of the game's.
# At 32 bits Orzammar's Commons at 512 a side ran out of address space on a
# machine with 128 GiB, and Embree, which casts its rays here, is made for 64.
#
# It is the same sources: src/RelightTool.cpp, Relighter.cpp and
# ProbeBaker.cpp, and of DAOLib only the pure parts they stand on, compiled
# here from DAOLib's tree as they are (DAOLib's own build is 32-bit and is not
# asked to be otherwise). tools/build-bake.ps1 configures and builds it into
# build\bake64.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(
        FATAL_ERROR
        "the bake tool is built for 64 bits; use tools/build-bake.ps1"
    )
endif()

# The compile database, which clang-tidy reads (tools/build-bake.ps1 -Lint):
# the Embree half of Relighter.cpp is compiled here alone, so it is linted
# here or nowhere.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(ELGARA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(DAOLIB_DIR
    ""
    CACHE PATH
    "DAOLib's source tree (default extern/DAOLib, else ../DAOLib)"
)
if(DAOLIB_DIR)
    set(ELGARA_DAOLIB "${DAOLIB_DIR}")
elseif(DEFINED ENV{DAOLIB_DIR})
    set(ELGARA_DAOLIB "$ENV{DAOLIB_DIR}")
elseif(EXISTS "${ELGARA_ROOT}/extern/DAOLib/CMakeLists.txt")
    set(ELGARA_DAOLIB "${ELGARA_ROOT}/extern/DAOLib")
else()
    set(ELGARA_DAOLIB "${ELGARA_ROOT}/../DAOLib")
endif()
message(STATUS "DAOLib: ${ELGARA_DAOLIB}")

# AddressSanitizer (tools/build-bake.ps1 -Sanitize, into build\bake64-asan),
# as DAOLib's DAO_SANITIZE has it: over every translation unit built here,
# so set before any target. vcpkg's spdlog, fmt, Embree and TBB are built
# without it, so the STL's container annotations, on under
# /fsanitize=address, would differ between them and us (LNK2038 on
# annotate_string and annotate_vector), and are turned off, which is
# Microsoft's advice for linking such libraries.
option(ELGARA_SANITIZE "Build the bake tool with AddressSanitizer" OFF)
if(ELGARA_SANITIZE)
    add_compile_options(/fsanitize=address)
    add_compile_definitions(
        _DISABLE_STRING_ANNOTATION
        _DISABLE_VECTOR_ANNOTATION
    )
    add_link_options(/INCREMENTAL:NO)
    message(STATUS "ELGARA_SANITIZE: AddressSanitizer enabled")
endif()

find_package(spdlog CONFIG REQUIRED)
find_package(embree CONFIG REQUIRED)

# DAOLib's pure parts, a library of their own: they are DAOLib's to keep
# clean, in DAOLib's own build, which is 32-bit. Here they are compiled for
# 64 bits at DAOLib's warning level, and a warning that only 64 bits brings
# out of them is said and does not fail this build.
add_library(
    elgara_daolib_core
    STATIC
    "${ELGARA_DAOLIB}/src/dao/core/Dds.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/Erf.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/Gff4.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/Inflate.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/Level.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/LightProbes.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/Model.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/ReadAt.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/RayCast.cpp"
)
target_compile_features(elgara_daolib_core PUBLIC cxx_std_20)
target_include_directories(elgara_daolib_core PUBLIC "${ELGARA_DAOLIB}/src")
target_compile_definitions(
    elgara_daolib_core
    PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX
)
target_compile_options(elgara_daolib_core PRIVATE /W4 /EHsc /permissive- /utf-8)

# Our own three sources, held to what the plugin's are (DAOLib's
# dao_apply_warnings): every warning, and a warning fails the build;
# third-party headers (spdlog, Embree, the STL) are kept quiet.
add_executable(
    elgarashaders_relight
    "${ELGARA_ROOT}/src/RelightTool.cpp"
    "${ELGARA_ROOT}/src/Relighter.cpp"
    "${ELGARA_ROOT}/src/ProbeBaker.cpp"
)
target_include_directories(elgarashaders_relight PRIVATE "${ELGARA_ROOT}/src")
target_compile_definitions(elgarashaders_relight PRIVATE ELGARA_EMBREE)
target_compile_options(
    elgarashaders_relight
    PRIVATE
        /W4
        /WX
        /EHsc
        /permissive-
        /utf-8
        /Zc:preprocessor
        /external:anglebrackets
        /external:W0
)
target_link_libraries(
    elgarashaders_relight
    PRIVATE elgara_daolib_core spdlog::spdlog embree
)
