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
    message(FATAL_ERROR "the bake tool is built for 64 bits; use tools/build-bake.ps1")
endif()

set(ELGARA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(DAOLIB_DIR "" CACHE PATH "DAOLib's source tree (default extern/DAOLib, else ../DAOLib)")
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

find_package(spdlog CONFIG REQUIRED)
find_package(embree CONFIG REQUIRED)

add_executable(
    elgarashaders_relight
    "${ELGARA_ROOT}/src/RelightTool.cpp"
    "${ELGARA_ROOT}/src/Relighter.cpp"
    "${ELGARA_ROOT}/src/ProbeBaker.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/Dds.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/Erf.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/Gff4.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/Inflate.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/Level.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/LightProbes.cpp"
    "${ELGARA_DAOLIB}/src/dao/core/RayCast.cpp"
)
target_compile_features(elgarashaders_relight PRIVATE cxx_std_20)
target_include_directories(elgarashaders_relight PRIVATE "${ELGARA_ROOT}/src" "${ELGARA_DAOLIB}/src")
target_compile_definitions(elgarashaders_relight PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX ELGARA_EMBREE)
target_compile_options(elgarashaders_relight PRIVATE /W4 /EHsc /utf-8)
target_link_libraries(elgarashaders_relight PRIVATE spdlog::spdlog embree)
