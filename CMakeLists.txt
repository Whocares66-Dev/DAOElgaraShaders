cmake_minimum_required(VERSION 3.21)

project(DAOElgaraShaders VERSION 0.4.1 LANGUAGES CXX)

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

# Our shaders formatted as the C++ is, by clang-format, which takes HLSL as C++
# (Quality.cmake's targets are DAOLib's and know only src, tests and
# examples; these hang from them). shaders/smaa/SMAA.hlsl is its authors' and
# kept as published.
file(GLOB ELGARA_HLSL CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/shaders/*.hlsl")
if(DAO_CLANG_FORMAT AND TARGET format)
    add_custom_target(
        format-hlsl
        COMMAND "${DAO_CLANG_FORMAT}" -i --style=file ${ELGARA_HLSL}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Formatting the shaders"
        VERBATIM
    )
    add_custom_target(
        format-check-hlsl
        COMMAND
            "${DAO_CLANG_FORMAT}" --dry-run -Werror --style=file ${ELGARA_HLSL}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Checking the formatting of the shaders"
        VERBATIM
    )
    add_dependencies(format format-hlsl)
    add_dependencies(format-check format-check-hlsl)
endif()

# Which texels of a material are metal, from its textures (issue 19): the
# table tool's rule in C++, touching neither the game nor the process, for
# the plugin to work out a material the table does not know.
add_library(
    elgara_materials
    STATIC
    src/materials/TextureRule.cpp
    src/materials/Answers.cpp
    src/materials/Names.cpp
    src/materials/Said.cpp
    src/materials/Learn.cpp
    src/materials/Kind.cpp
    src/materials/Islands.cpp
    src/materials/Parts.cpp
    src/materials/Items.cpp
)
target_include_directories(elgara_materials PUBLIC src)
# The names' tables are Python's regular expressions (lookbehind among them),
# which PCRE2 reads as Python does and std::regex does not.
find_package(pcre2 CONFIG REQUIRED)
target_link_libraries(elgara_materials PRIVATE PCRE2::8BIT)
# A texture read and a map written as the game keeps them (Learn.cpp).
target_link_libraries(elgara_materials PUBLIC DAOLib::core)
dao_apply_warnings(elgara_materials)

# ---------------------------------------------------------------------------
# elgarashaders.dll -- the plugin, a folder of its own:
# build\<preset>\daolib\plugins\elgarashaders\ has the dll, its pdb and,
# in override\, the game's shaders rebuilt, which the game reads as it reads
# its own override folder. tools/deploy.ps1 copies the
# folder as it is.
# ---------------------------------------------------------------------------
add_daolib_plugin(
    elgarashaders
    SOURCES
        src/Graphics.cpp
        src/Glowing.cpp
        src/GpuTimer.cpp
        src/MaterialPatch.cpp
        src/MaterialLearn.cpp
        src/ResourceDump.cpp
        src/Outdoors.cpp
        src/DayNight.cpp
        src/DrawDistance.cpp
        src/NightCubes.cpp
        src/NightLightmaps.cpp
        src/SkyBodies.cpp
        src/ProbeBaker.cpp
        src/Probes.cpp
        src/EffectLights.cpp
        src/LevelShadows.cpp
        src/KeptState.cpp
        src/Cascades.cpp
        src/Reflected.cpp
        src/ShaderCode.cpp
)
target_link_libraries(elgarashaders PRIVATE elgara_materials)
dao_apply_warnings(elgarashaders)
# The plugin's tables, read as it starts: data/effects.ini (tools/effect_list.py),
# the lights it gives fires that have none and the effects it dims at night;
# data/glowing.ini, the terrain palettes' layers that glow (lava);
# data/areas, a file for each area the day and night runs in, how its sun and
# moon go; data/clouds, the clouds an area's sky may take by day (an interior's
# night lightmaps are baked into the plugin's folder by toolsake-night.ps1). Steps with the tables for their input: copied after the DLL's
# link, a change to a table alone never reached the build (2026-10-23).
file(
    GLOB ELGARA_AREAS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/data/areas/*.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/clouds/*.ini"
)
set(ELGARA_TABLES
    "${CMAKE_CURRENT_SOURCE_DIR}/data/effects.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/glowing.ini"
    ${ELGARA_AREAS}
)
set(ELGARA_TABLES_OUT)
foreach(table IN LISTS ELGARA_TABLES)
    file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}/data" "${table}")
    set(out "${CMAKE_BINARY_DIR}/daolib/plugins/elgarashaders/${relative}")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${table}" "${out}"
        DEPENDS "${table}"
        VERBATIM
    )
    list(APPEND ELGARA_TABLES_OUT "${out}")
endforeach()
add_custom_target(elgarashaders_effects ALL DEPENDS ${ELGARA_TABLES_OUT})

# The plugin's baker of light probes as a command of its own: a level baked
# with no game running, to look at what it makes and how long it takes.
add_executable(elgarashaders_bake src/BakeTool.cpp src/ProbeBaker.cpp)
target_link_libraries(elgarashaders_bake PRIVATE DAOLib::daolib)
dao_apply_warnings(elgarashaders_bake)

# A lightmap of the relighter's packed as the game keeps its own (DXT1 or
# DXT5): the night lightmaps tools/bake-night.ps1 bakes (src/DdsPackTool.cpp).
add_executable(elgarashaders_ddspack src/DdsPackTool.cpp)
target_link_libraries(elgarashaders_ddspack PRIVATE DAOLib::core)
dao_apply_warnings(elgarashaders_ddspack)

# The materials' table (data/materials.ini) made from the game, and given to
# the plugin as answers and maps (src/materials/Table.h, issue 19), over
# DAOLib's install index: elgarashaders_materials table | objects | why.
add_library(elgara_materials_table STATIC src/materials/Table.cpp)
target_link_libraries(
    elgara_materials_table
    PUBLIC elgara_materials DAOLib::core
)
dao_apply_warnings(elgara_materials_table)
add_executable(elgarashaders_materials src/materials/TableTool.cpp)
target_link_libraries(
    elgarashaders_materials
    PRIVATE elgara_materials_table shell32 ole32
)
dao_apply_warnings(elgarashaders_materials)

# The shaders, a chain and a material are the game's own, changed: made from
# the install's shaders.erf into the plugin's override folder
# (shaders/build_shaders.py, which writes nothing and says why where the game
# or the Windows SDK's fxc.exe is not found). They were made on every build,
# minutes of fxc a time on a busy machine, the 0.2.2 release's build among
# them (the user, 2026-10-20: "we shouldn't have to recompile things that
# didn't change"). Now when a file the script says it reads changes: the
# game's archives, fxc, the HLSL and SMAA files, the Python. A new HLSL file
# configures again (ELGARA_HLSL's glob), and so asks the script again.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    file(
        GLOB ELGARA_SMAA
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/shaders/smaa/*"
    )
    set(ELGARA_SHADERS_RUN
        "${CMAKE_COMMAND}"
        -E
        env
        "DAOLIB_TOOLS=${ELGARA_DAOLIB}/tools"
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/build_shaders.py"
    )
    execute_process(
        COMMAND ${ELGARA_SHADERS_RUN} inputs
        OUTPUT_VARIABLE ELGARA_SHADER_INPUTS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE ELGARA_SHADER_INPUTS_RESULT
    )
    if(NOT ELGARA_SHADER_INPUTS_RESULT EQUAL 0)
        message(
            FATAL_ERROR
            "shaders/build_shaders.py inputs failed (${ELGARA_SHADER_INPUTS_RESULT})"
        )
    endif()
    # The game's archives are read through DAOLib's dao_files (its one
    # reader of them, issue 106 of DAOLib's), built first: in a consumer's
    # tree it is built only for what names it.
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/shaders.stamp"
        COMMAND
            "${CMAKE_COMMAND}" -E env "DAO_FILES=$<TARGET_FILE:dao_files>"
            ${ELGARA_SHADERS_RUN}
            "${CMAKE_BINARY_DIR}/daolib/plugins/elgarashaders/override"
        COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/shaders.stamp"
        DEPENDS ${ELGARA_SHADER_INPUTS} ${ELGARA_HLSL} ${ELGARA_SMAA} dao_files
        COMMENT "elgarashaders: the game's shaders, rebuilt"
        VERBATIM
    )
    add_custom_target(
        elgarashaders_shaders
        ALL
        DEPENDS "${CMAKE_BINARY_DIR}/shaders.stamp"
    )
endif()

# What each material is, as the game reads it: data/materials.ini made into
# answers and metalness maps in the plugin's folder (elgarashaders_materials
# objects), when the table, the names' tables, the add-ins' dump or the tool
# changes. The dump (build/addins, tools/dump_addins.py) is outside git, and
# put in or changed after a build it was not read again until the stamp was
# taken away by hand (2026-10-16): its files are the step's inputs too.
file(
    GLOB ELGARA_ADDINS_DUMP
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/build/addins/*"
)
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/materials.stamp"
    COMMAND
        "$<TARGET_FILE:elgarashaders_materials>" objects
        "${CMAKE_BINARY_DIR}/daolib/plugins/elgarashaders" --ini
        "${CMAKE_CURRENT_SOURCE_DIR}/data/materials.ini" --names
        "${CMAKE_CURRENT_SOURCE_DIR}/data/material_names.txt" --addins
        "${CMAKE_CURRENT_SOURCE_DIR}/build/addins"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/materials.stamp"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/data/materials.ini"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/material_names.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/material_parts.ini"
        ${ELGARA_ADDINS_DUMP}
        elgarashaders_materials
    COMMENT "elgarashaders: the materials' table, as the game reads it"
    VERBATIM
)
# Not part of a debug build's own: the tool unoptimised takes fifteen minutes
# and more over the game's textures where the release build's takes one
# (2026-10-15), and a debug build is for the linters and a debugger, not
# what is deployed. `--target elgarashaders_answers` makes them there too.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ELGARA_ANSWERS_IN_ALL "")
else()
    set(ELGARA_ANSWERS_IN_ALL ALL)
endif()
add_custom_target(
    elgarashaders_answers
    ${ELGARA_ANSWERS_IN_ALL}
    DEPENDS "${CMAKE_BINARY_DIR}/materials.stamp"
)

enable_testing()

# The materials' rule, names, lines and table (tests/test_materials.cpp); its
# [game] cases read the installed game, and are skipped where there is none.
find_package(Catch2 3 CONFIG REQUIRED)
add_executable(
    elgarashaders_tests
    tests/test_materials.cpp
    tests/test_cascades.cpp
    tests/test_reflected.cpp
    tests/test_shadercode.cpp
    src/Cascades.cpp
    src/Reflected.cpp
    src/ShaderCode.cpp
)
target_link_libraries(
    elgarashaders_tests
    PRIVATE elgara_materials_table DAOLib::core Catch2::Catch2WithMain
)
target_compile_definitions(
    elgarashaders_tests
    PRIVATE
        ELGARA_NAMES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/data/material_names.txt"
)
dao_apply_warnings(elgarashaders_tests)
include(Catch)
catch_discover_tests(elgarashaders_tests)

# The Python tools' own tests (tools/test_*.py), where there is a Python.
if(Python3_Interpreter_FOUND)
    add_test(
        NAME python_tools
        COMMAND
            "${Python3_EXECUTABLE}" -m unittest discover -s
            "${PROJECT_SOURCE_DIR}/tools" -p "test_*.py"
    )
endif()
