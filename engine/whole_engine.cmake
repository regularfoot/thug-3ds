# Full portable source selection, NOT a mission or gameplay-method whitelist.
# Platform implementations are explicit remaining 3DS obligations. Unknown
# engine symbols must fail compilation/link; no generated link stubs.
file(GLOB_RECURSE THUG_ORIGINAL_CPP CONFIGURE_DEPENDS "${ORIGINAL}/*.cpp")
include("${CMAKE_CURRENT_SOURCE_DIR}/private/original-build-sources.cmake")
set(THUG_WHOLE_GAME_SOURCES)
set(THUG_PLATFORM_SOURCE_OBLIGATIONS)
set(THUG_NOT_BUILT_UPSTREAM)
foreach(source IN LISTS THUG_ORIGINAL_CPP)
    file(RELATIVE_PATH relative "${ORIGINAL}" "${source}")
    if(relative MATCHES "/(wn32|win32|xbox|ngc|ngps|dx9|sdl)/")
        list(APPEND THUG_PLATFORM_SOURCE_OBLIGATIONS "${relative}")
    elseif(relative MATCHES "^sys/sio/(sioman|siodev)\\.cpp$")
        # Despite their generic path these directly implement PS2 libpad/SIF,
        # with platform alternatives in sibling dirs. Native SIO remains an
        # explicit link obligation; never fake Sony SDK calls to compile them.
        list(APPEND THUG_PLATFORM_SOURCE_OBLIGATIONS "${relative}")
    elseif(relative MATCHES "^sk/gamenet/gamespy/")
        # Third-party GameSpy SDK is a platform dependency, not an offline
        # player manager. Keep ALL other GameNet sources, including Lobby.
        list(APPEND THUG_PLATFORM_SOURCE_OBLIGATIONS "${relative}")
    elseif(relative MATCHES "^(standard|template)\\.cpp$")
        # Coding-style documents contain deliberate non-C++ pseudocode.
    elseif(relative MATCHES "/gunslinger[^/]*\\.cpp$")
        # Duplicate implementations of ordinary components, not another feature.
    elseif(relative MATCHES "^core/debug/")
        # Platform diagnostic hooks, implemented by our fatal/log contract.
        list(APPEND THUG_PLATFORM_SOURCE_OBLIGATIONS "${relative}")
    elseif(NOT relative IN_LIST THUG_ORIGINAL_PC_BUILD_SOURCES)
        # The actual PC build disables prototypes such as HorseCamera. Its
        # literal file list is hash-authenticated by original_source.py. This
        # is not a mission/function whitelist: keep every real game subsystem.
        list(APPEND THUG_NOT_BUILT_UPSTREAM "${relative}")
    else()
        list(APPEND THUG_WHOLE_GAME_SOURCES "${source}")
    endif()
endforeach()
# Reuse the whole original PC manager's portable list/task lifecycle, selecting
# actual native hardware acquisition through the checked portability ledger.
list(APPEND THUG_WHOLE_GAME_SOURCES "${ORIGINAL}/sys/sio/win32/p_sioman.cpp")
list(APPEND THUG_WHOLE_GAME_SOURCES "${ORIGINAL}/gel/soundfx/sdl/ima.cpp")
list(APPEND THUG_WHOLE_GAME_SOURCES ${THUG_CONSOLE_PARTICLES})
list(APPEND THUG_WHOLE_GAME_SOURCES ${THUG_ORIGINAL_OCCLUSION})
list(LENGTH THUG_WHOLE_GAME_SOURCES THUG_WHOLE_GAME_COUNT)
message(STATUS "Whole original portable engine: ${THUG_WHOLE_GAME_COUNT} translation units, no per-mission selection")
message(STATUS "Original build disables prototypes: ${THUG_NOT_BUILT_UPSTREAM}")
add_library(thug_whole_game_objects OBJECT EXCLUDE_FROM_ALL ${THUG_WHOLE_GAME_SOURCES})
target_include_directories(thug_whole_game_objects PRIVATE engine/platform "${ORIGINAL}" "${ORIGINAL}/sk")
target_compile_definitions(thug_whole_game_objects PRIVATE __PLAT_N3DS__ KISAK_ORIGINAL_ALLOCATOR __NOPT_ASSERT__)
target_compile_options(thug_whole_game_objects PRIVATE -Wno-register)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(thug_whole_game_objects PRIVATE -ferror-limit=3 -Wno-writable-strings)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(thug_whole_game_objects PRIVATE -fmax-errors=3 -Wno-write-strings)
endif()
# The application includes the REAL Sk/Main.cpp, not the unit-test main or a
# custom external gameplay loop. A full link must close every engine owner.
add_library(thug_application_entry OBJECT EXCLUDE_FROM_ALL engine/platform/application_entry.cpp)
target_include_directories(thug_application_entry PRIVATE engine/platform)
add_executable(thug1_full_engine EXCLUDE_FROM_ALL ${THUG_NATIVE_PLATFORM}
    engine/platform/startup_save.cpp
    $<TARGET_OBJECTS:thug_application_entry> $<TARGET_OBJECTS:thug_whole_game_objects>)
target_include_directories(thug1_full_engine PRIVATE engine/platform "${ORIGINAL}" "${ORIGINAL}/sk")
target_compile_definitions(thug1_full_engine PRIVATE __PLAT_N3DS__ KISAK_ORIGINAL_ALLOCATOR __NOPT_ASSERT__)
target_link_libraries(thug1_full_engine PRIVATE thug_media_codecs)
if(NINTENDO_3DS)
    option(THUG_ARM_LTO "Enable link-time optimization for the full ARM game" OFF)
    if(THUG_ARM_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT thug_ipo_supported OUTPUT thug_ipo_error LANGUAGES C CXX)
        if(NOT thug_ipo_supported)
            message(FATAL_ERROR "ARM LTO unavailable: ${thug_ipo_error}")
        endif()
        set_property(TARGET thug1_full_engine thug_whole_game_objects thug_application_entry
            PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        # FFmpeg and its C adapter deliberately use 32-bit enums. Keep that
        # boundary out of the game's short-enum LTO unit.
        set_property(SOURCE engine/platform/media_os.c engine/platform/movie_decode.cpp APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
        target_compile_definitions(thug1_full_engine PRIVATE THUG_ARM_LTO=1)
    endif()
    target_sources(thug1_full_engine PRIVATE engine/platform/startup_memory.c)
    target_compile_options(thug_whole_game_objects PRIVATE -O2 -fno-strict-aliasing)
    target_compile_options(thug1_full_engine PRIVATE -O2 -fno-strict-aliasing)
    target_link_libraries(thug1_full_engine PRIVATE citro3d source_world_shader_bin source_skin_shader_bin ctru m)
    target_link_options(thug1_full_engine PRIVATE
        -Wl,--wrap=gspSubmitGxCommand -Wl,--wrap=GX_ProcessCommandList -Wl,--wrap=GPUCMD_Add -Wl,--wrap=C3D_UpdateUniforms -Wl,--wrap=C3Di_EffectBind
        -Wl,--wrap=__system_allocateHeaps)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/engine/platform/source_port/build_id.h" build_id_line REGEX "^#define THUG_SOURCE_BUILD ")
    string(REGEX REPLACE "^#define THUG_SOURCE_BUILD \"([^\"]+)\".*" "\\1" source_build_id "${build_id_line}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/engine/platform/source_port/build_id.h")
    ctr_generate_smdh(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/thug1_full_engine.smdh"
        NAME "THUG1 ${source_build_id}" DESCRIPTION "THUG1 ${source_build_id} - Source Engine" AUTHOR "3DS port")
    ctr_create_3dsx(thug1_full_engine SMDH "${CMAKE_CURRENT_BINARY_DIR}/thug1_full_engine.smdh")
    set_target_properties(thug1_full_engine_3dsx PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
# A genuine compile AND link gate. It intentionally fails for real missing
# platform/ABI owners. No unresolved-symbol suppression or generated stubs.
add_custom_target(whole_engine_compile DEPENDS thug1_full_engine)
