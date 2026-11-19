cmake_minimum_required(VERSION 3.20)
project(THUG1WholeSourceEngine LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(THUG_SOURCE_SANITIZERS "Host ASan/UBSan" ON)
if(NINTENDO_3DS)
    set(MEDIA_BUILD "${CMAKE_BINARY_DIR}/media")
else()
    set(MEDIA_BUILD "${CMAKE_CURRENT_SOURCE_DIR}/build/media-host")
endif()
add_library(thug_media_codecs INTERFACE)
target_include_directories(thug_media_codecs INTERFACE "${MEDIA_BUILD}" "${CMAKE_CURRENT_SOURCE_DIR}/private/ffmpeg")
target_link_libraries(thug_media_codecs INTERFACE
    "${MEDIA_BUILD}/libavformat/libavformat.a" "${MEDIA_BUILD}/libavcodec/libavcodec.a" "${MEDIA_BUILD}/libavutil/libavutil.a")
set(ORIGINAL "${CMAKE_CURRENT_SOURCE_DIR}/private/original")
if(NOT EXISTS "${ORIGINAL}/gel/mainloop/mainloop.cpp")
    message(FATAL_ERROR "Run python3 tools/prepare_engine.py CHECKOUT first. Original game source stays private.")
endif()
set(WHOLE_SCHEDULER
    core/task/task.cpp core/task/tlist.cpp core/task/tstack.cpp
    gel/module/module.cpp gel/module/modman.cpp gel/mainloop/mainloop.cpp
    core/crc.cpp core/compress.cpp core/list/list.cpp core/lookuptable.cpp
    core/math/math.cpp core/math/vector.cpp core/math/matrix.cpp
    core/math/geometry.cpp core/math/rot90.cpp core/math/slerp.cpp
    core/string/cstring.cpp core/string/stringutils.cpp
    sys/mem/compactpool.cpp sys/mem/poolmanager.cpp
    core/support/class.cpp sys/config/config.cpp
    sys/mem/memman.cpp sys/mem/alloc.cpp sys/mem/heap.cpp sys/mem/region.cpp
    sys/mem/pile.cpp sys/mem/pool.cpp sys/mem/poolable.cpp
    sys/sio/win32/p_sioman.cpp gel/input/inpman.cpp gel/input/inpserv.cpp
    sys/file/pre.cpp sys/file/asyncfilesys.cpp sys/file/pip.cpp
    gel/soundfx/soundfx.cpp gel/music/music.cpp gel/soundfx/sdl/ima.cpp gfx/nx.cpp gfx/nxtexture.cpp gfx/nxtexman.cpp gel/scripting/checksum.cpp
    gfx/nxfont.cpp gfx/nxwin2d.cpp gfx/nxsprite.cpp gfx/nxmesh.cpp
    gfx/nxgeom.cpp gfx/nxmodel.cpp gfx/nxlight.cpp gfx/nxlightman.cpp gfx/nxnewparticle.cpp gfx/nxnewparticlemgr.cpp gfx/nxmiscfx.cpp
    gfx/nxscene.cpp gfx/nxsector.cpp sk/engine/supersector.cpp gel/collision/collision.cpp gel/collision/colltridata.cpp
    gfx/nxviewman.cpp gfx/nxviewport.cpp gfx/camera.cpp gfx/nxweather.cpp gfx/nxloadscreen.cpp)
list(TRANSFORM WHOLE_SCHEDULER PREPEND "${ORIGINAL}/")
# Active, whole console shape owners. Upstream PC versions disable rendering.
# Their hardware-only adaptation is authenticated in original_source.py.
set(THUG_CONSOLE_PARTICLES)
foreach(shape line flat shaded smooth glow star smoothstar ribbon smoothribbon ribbontrail glowribbontrail)
    list(APPEND THUG_CONSOLE_PARTICLES "${ORIGINAL}/gfx/ngc/p_nxparticle${shape}.cpp")
endforeach()
list(APPEND WHOLE_SCHEDULER ${THUG_CONSOLE_PARTICLES})
set(THUG_ORIGINAL_OCCLUSION "${ORIGINAL}/gfx/dx9/nx/occlude.cpp")
list(APPEND WHOLE_SCHEDULER ${THUG_ORIGINAL_OCCLUSION})
set(THUG_NATIVE_PLATFORM engine/platform/allocation.cpp engine/platform/memory.cpp engine/platform/config.cpp
    engine/platform/thread.cpp engine/platform/thread_os.c engine/platform/socket.cpp engine/platform/socket_os.c
    engine/platform/diagnostics.cpp engine/platform/logic_timing.cpp engine/platform/application.cpp engine/platform/application_os.c
    engine/platform/timer.cpp engine/platform/timer_state.cpp engine/platform/clock_os.c
    engine/platform/input.cpp engine/platform/input_device.cpp engine/platform/input_os.c
    engine/platform/files.cpp engine/platform/pre_validation.cpp engine/platform/async_files.cpp
    engine/platform/save.cpp engine/platform/media.cpp engine/platform/media_os.c
    engine/platform/movie_decode.cpp engine/platform/movies.cpp
    engine/platform/audio.cpp engine/platform/audio_os.c engine/platform/audio_sfx.cpp engine/platform/audio_stream.cpp engine/platform/audio_file_stream.cpp
    engine/platform/graphics.cpp engine/platform/graphics_os.c engine/platform/graphics_viewport.cpp engine/platform/graphics_nx.cpp)
list(APPEND THUG_NATIVE_PLATFORM engine/platform/render_assets.cpp engine/platform/scene_assets.cpp engine/platform/static_world_lod.cpp engine/platform/world_texture_bank.cpp engine/platform/texture_cache.cpp engine/platform/native_texture.cpp
    engine/platform/native_font.cpp engine/platform/native_font_factory.cpp engine/platform/native_ui.cpp
    engine/platform/mesh_assets.cpp engine/platform/native_mesh.cpp engine/platform/native_ui_gpu.cpp engine/platform/material_animation.cpp
    engine/platform/native_lighting.cpp engine/platform/native_geom.cpp engine/platform/native_scene.cpp engine/platform/native_model.cpp engine/platform/native_world_gpu.cpp engine/platform/native_particles.cpp engine/platform/native_renderer.cpp)
list(APPEND THUG_NATIVE_PLATFORM engine/platform/legacy_particle_draw.cpp engine/platform/legacy_particle_factory.cpp)
list(APPEND THUG_NATIVE_PLATFORM engine/platform/native_effects.cpp engine/platform/weather_field.cpp engine/platform/native_weather.cpp)
list(APPEND THUG_NATIVE_PLATFORM engine/platform/native_engine.cpp)
list(APPEND THUG_NATIVE_PLATFORM engine/platform/native_occlusion.cpp)
list(APPEND THUG_NATIVE_PLATFORM engine/platform/native_fog.cpp)
list(APPEND THUG_NATIVE_PLATFORM engine/platform/native_loadscreen.cpp)
list(APPEND THUG_NATIVE_PLATFORM engine/platform/native_projection.cpp)
list(APPEND THUG_NATIVE_PLATFORM engine/platform/native_diagnostics.cpp engine/platform/native_capabilities.cpp)
list(APPEND THUG_NATIVE_PLATFORM engine/platform/launch_paths.cpp)
list(APPEND THUG_NATIVE_PLATFORM engine/platform/render_options.c engine/platform/native_profile.cpp)
list(APPEND THUG_NATIVE_PLATFORM engine/third_party/meshoptimizer/allocator.cpp engine/third_party/meshoptimizer/simplifier.cpp)
add_library(thug_whole_scheduler STATIC ${WHOLE_SCHEDULER} ${THUG_NATIVE_PLATFORM})
target_include_directories(thug_whole_scheduler PUBLIC engine/platform "${ORIGINAL}" "${ORIGINAL}/sk")
target_compile_definitions(thug_whole_scheduler PUBLIC __PLAT_N3DS__ KISAK_ORIGINAL_ALLOCATOR __NOPT_ASSERT__)
target_link_libraries(thug_whole_scheduler PUBLIC thug_media_codecs)
target_compile_options(thug_whole_scheduler PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-register)
# Isolated lifecycle tests compile whole original owners. Discard unrelated
# unused functions at final link; the full game still links every real owner.
target_compile_options(thug_whole_scheduler PRIVATE -ffunction-sections -fdata-sections)
if(NINTENDO_3DS)
    # Keep debug symbols and original assertions while optimising ARM code.
    # The original engine type-puns math/packed data; preserve that behaviour.
    # These CPU-bound loops run for both eyes on ARM11. Preserve the original
    # FP semantics, assertions, vertex format and the game's global -O2.
    set_property(SOURCE engine/platform/native_geom.cpp engine/platform/native_world_gpu.cpp
        engine/platform/graphics_os.c engine/platform/audio.cpp engine/platform/mesh_assets.cpp
        APPEND PROPERTY COMPILE_OPTIONS -O3)
    target_compile_options(thug_whole_scheduler PRIVATE -O2 -fno-strict-aliasing)
endif()
if(APPLE)
    target_link_options(thug_whole_scheduler INTERFACE -Wl,-dead_strip)
else()
    target_link_options(thug_whole_scheduler INTERFACE -Wl,--gc-sections)
endif()
if(THUG_SOURCE_SANITIZERS)
    target_compile_options(thug_whole_scheduler PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(thug_whole_scheduler PUBLIC -fsanitize=address,undefined)
endif()
if(NOT NINTENDO_3DS)
    add_executable(source_bake_world_lods EXCLUDE_FROM_ALL
        engine/test_tools/bake_world_lods.cpp)
    target_include_directories(source_bake_world_lods PRIVATE engine/third_party/meshoptimizer)
    target_link_libraries(source_bake_world_lods PRIVATE thug_whole_scheduler)
    add_executable(source_bake_texture_cache EXCLUDE_FROM_ALL
        engine/test_tools/bake_texture_cache.cpp engine/third_party/rg_etc1/rg_etc1.cpp)
    target_include_directories(source_bake_texture_cache PRIVATE engine/third_party/rg_etc1)
    target_compile_options(source_bake_texture_cache PRIVATE -O3)
    target_link_libraries(source_bake_texture_cache PRIVATE thug_whole_scheduler)
    add_executable(source_dump_world_mips EXCLUDE_FROM_ALL engine/test_tools/dump_world_mips.cpp)
    target_link_libraries(source_dump_world_mips PRIVATE thug_whole_scheduler)
    add_executable(source_prepare_cas_resources EXCLUDE_FROM_ALL engine/test_tools/prepare_cas_resources.cpp)
    target_link_libraries(source_prepare_cas_resources PRIVATE thug_whole_scheduler)
    find_package(Threads REQUIRED)
    target_link_libraries(thug_whole_scheduler PUBLIC Threads::Threads)
endif()
if(NINTENDO_3DS)
    set_source_files_properties(engine/platform/media_os.c engine/platform/movie_decode.cpp PROPERTIES COMPILE_OPTIONS -fno-short-enums)
    ctr_add_shader_library(source_world_shader OUTPUT world.shbin engine/platform/shaders/world.v.pica)
    dkp_add_embedded_binary_library(source_world_shader_bin source_world_shader)
    ctr_add_shader_library(source_skin_shader OUTPUT skin.shbin engine/platform/shaders/skin.v.pica)
    dkp_add_embedded_binary_library(source_skin_shader_bin source_skin_shader)
    target_link_libraries(thug_whole_scheduler PUBLIC citro3d source_world_shader_bin)
    target_link_libraries(thug_whole_scheduler PUBLIC source_skin_shader_bin)
    target_link_options(thug_whole_scheduler INTERFACE -Wl,--wrap=gspSubmitGxCommand -Wl,--wrap=GX_ProcessCommandList -Wl,--wrap=GPUCMD_Add)
endif()
include(engine/whole_engine.cmake)
