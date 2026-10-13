cmake_minimum_required(VERSION 3.21)

project(DAOElgaraShaders VERSION 0.1.0 LANGUAGES CXX)

# Dragon Age: Origins is a 32-bit x86 executable, so the plugin the loader
# loads into it must be too. CMakePresets.json selects the x86 toolset and
# the x86-windows-static-md vcpkg triplet; this catches a configure from a
# 64-bit shell with a sentence rather than a DLL the game cannot load.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(
        FATAL_ERROR
        "elgarashaders.dll must be built for 32-bit x86 (the game's architecture); "
        "configure with a preset from CMakePresets.json, through tools/build.ps1"
    )
endif()

# ---------------------------------------------------------------------------
# DAOLib, built with us the way Skyrim plugins build CommonLibSSE: a source
# tree added as a subdirectory, which gives DAOLib::daolib, add_daolib_plugin
# and dao_apply_warnings. Its dependencies (detours, spdlog) come through our
# vcpkg.json, since a manifest build reads only the top-level manifest.
#
# The tree is the one DAOLIB_DIR names if it names one, else extern/DAOLib,
# a git submodule, else the sibling checkout ..\DAOLib.
# ---------------------------------------------------------------------------
set(DAOLIB_DIR
    ""
    CACHE PATH
    "DAOLib's source tree (default extern/DAOLib, else ../DAOLib)"
)
if(DAOLIB_DIR)
    set(ELGARA_DAOLIB "${DAOLIB_DIR}")
elseif(DEFINED ENV{DAOLIB_DIR})
    set(ELGARA_DAOLIB "$ENV{DAOLIB_DIR}")
elseif(EXISTS "${PROJECT_SOURCE_DIR}/extern/DAOLib/CMakeLists.txt")
    set(ELGARA_DAOLIB "${PROJECT_SOURCE_DIR}/extern/DAOLib")
else()
    set(ELGARA_DAOLIB "${PROJECT_SOURCE_DIR}/../DAOLib")
endif()
if(NOT EXISTS "${ELGARA_DAOLIB}/cmake/DAOLib.cmake")
    message(
        FATAL_ERROR
        "No DAOLib with add_daolib_plugin at ${ELGARA_DAOLIB}. Add the submodule "
        "(git submodule update --init) or set DAOLIB_DIR."
    )
endif()
message(STATUS "DAOLib: ${ELGARA_DAOLIB}")
add_subdirectory(
    "${ELGARA_DAOLIB}"
    "${CMAKE_BINARY_DIR}/extern/DAOLib"
    EXCLUDE_FROM_ALL
)

# The format and lint targets, DAOLib's own module run over this tree: it
# reads the top-level source directory, our .clang-format and .clang-tidy,
# and tools/check-powershell.ps1.
include("${ELGARA_DAOLIB}/cmake/Quality.cmake")

# ---------------------------------------------------------------------------
# elgarashaders.dll -- the plugin, a folder of its own:
# build\<preset>\daolib\plugins\elgarashaders\ has the dll, its pdb, the
# looks' file and, in override\, the game's shaders rebuilt, which the game
# reads as it reads its own override folder. tools/deploy.ps1 copies the
# folder as it is.
# ---------------------------------------------------------------------------
add_daolib_plugin(
    elgarashaders
    SOURCES src/Graphics.cpp src/ProbeBaker.cpp src/ResourceDump.cpp
    FILES data/graphics.ini data/graphics.tuned
)
dao_apply_warnings(elgarashaders)

# The shaders, a chain and a material are the game's own, changed: made on
# every build from the install's shaders.erf into the plugin's override
# folder (shaders/build_shaders.py, which writes nothing and says why where
# the game or the Windows SDK's fxc.exe is not found). The map size is
# Graphics.cpp's kMapSize.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(
        elgarashaders_shaders
        ALL
        COMMAND
            "${CMAKE_COMMAND}" -E env "DAOLIB_TOOLS=${ELGARA_DAOLIB}/tools"
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/build_shaders.py"
            "${CMAKE_BINARY_DIR}/daolib/plugins/elgarashaders/override" 2048
        COMMENT "elgarashaders: the game's shaders, rebuilt"
        VERBATIM
    )
endif()

# What each material is, as the game reads it: data/materials.ini made into
# material objects' overrides and metalness maps beside the shaders
# (tools/material_table.py --objects), when the table or the tool changes.
if(Python3_Interpreter_FOUND)
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/materials.stamp"
        COMMAND
            "${CMAKE_COMMAND}" -E env "DAOLIB_TOOLS=${ELGARA_DAOLIB}/tools"
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/material_table.py" --objects
            "${CMAKE_BINARY_DIR}/daolib/plugins/elgarashaders/override"
        COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/materials.stamp"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/data/materials.ini"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/material_table.py"
        COMMENT "elgarashaders: the materials' table, as the game reads it"
        VERBATIM
    )
    add_custom_target(elgarashaders_materials ALL DEPENDS "${CMAKE_BINARY_DIR}/materials.stamp")
endif()

# The plugin's baker of light probes as a command of its own: a level baked
# with no game running, to look at what it makes and how long it takes.
add_executable(elgarashaders_bake src/BakeTool.cpp src/ProbeBaker.cpp)
target_link_libraries(elgarashaders_bake PRIVATE DAOLib::daolib)
dao_apply_warnings(elgarashaders_bake)

# The relighter as a command: a level's lightmaps made again from its own lamps
# and what glows in it, written as files for the override folder.
add_executable(elgarashaders_relight src/RelightTool.cpp src/Relighter.cpp src/ProbeBaker.cpp)
target_link_libraries(elgarashaders_relight PRIVATE DAOLib::daolib)
dao_apply_warnings(elgarashaders_relight)

# The Python tools' own tests (tools/test_*.py), where there is a Python.
enable_testing()
if(Python3_Interpreter_FOUND)
    add_test(
        NAME python_tools
        COMMAND
            "${Python3_EXECUTABLE}" -m unittest discover -s
            "${PROJECT_SOURCE_DIR}/tools" -p "test_*.py"
    )
endif()
