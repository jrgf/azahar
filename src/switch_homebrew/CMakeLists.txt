if (NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "ENABLE_SWITCH_HOMEBREW_CORE requires the devkitPro Switch CMake toolchain")
endif()

add_executable(azahar-switch-core
    source/core_bridge/core_bridge.cpp
    source/core_bridge/core_bridge.h
    source/core_bridge/emu_window_switch.cpp
    source/core_bridge/emu_window_switch.h
    source/emulation_session.cpp
    source/emulation_session.h
    source/emulator_bootstrap.cpp
    source/emulator_bootstrap.h
    source/main.cpp
    source/switch_input.cpp
    source/switch_input.h
    source/switch_input_libnx.cpp
    source/switch_jit_probe.cpp
    source/switch_jit_probe.h
    source/switch_runtime.cpp
    source/switch_runtime.h
)

target_compile_definitions(azahar-switch-core PRIVATE AZAHAR_SWITCH_ENABLE_CORE_BRIDGE)
target_include_directories(azahar-switch-core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source
    ${NX_STANDARD_INCLUDE_DIRECTORIES}
)

target_link_libraries(azahar-switch-core PRIVATE
    audio_core
    citra_common
    citra_core
    deko3d
    input_common
    ${NX_STANDARD_LIBRARIES}
)

if (AZAHAR_SWITCH_OPENGL_SPIKE)
    if (NOT ENABLE_OPENGL)
        message(FATAL_ERROR "AZAHAR_SWITCH_OPENGL_SPIKE requires ENABLE_OPENGL=ON")
    endif()

    target_compile_definitions(azahar-switch-core PRIVATE AZAHAR_SWITCH_OPENGL_SPIKE)
    target_include_directories(azahar-switch-core PRIVATE
        $ENV{DEVKITPRO}/portlibs/switch/include
    )
    target_link_directories(azahar-switch-core PRIVATE
        $ENV{DEVKITPRO}/portlibs/switch/lib
    )
    if (TARGET glad)
        target_link_libraries(azahar-switch-core PRIVATE glad)
    endif()
    target_link_libraries(azahar-switch-core PRIVATE EGL glapi drm_nouveau)
endif()

nx_generate_nacp(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/azahar-switch-core.nacp
    NAME "Azahar"
    AUTHOR "Azahar Emulator Project"
    VERSION "0.1.0"
)

nx_create_nro(
    azahar-switch-core-nro
    TARGET azahar-switch-core
    OUTPUT ${CMAKE_BINARY_DIR}/bin/azahar-switch-core.nro
    NACP ${CMAKE_CURRENT_BINARY_DIR}/azahar-switch-core.nacp
)

if (ENABLE_LIBRETRO)
    add_executable(azahar-switch-libretro
        source/emulator_bootstrap.cpp
        source/emulator_bootstrap.h
        source/heap_tracker.cpp
        source/libretro_host_main.cpp
        source/switch_input.cpp
        source/switch_input.h
        source/switch_input_libnx.cpp
        source/switch_runtime.cpp
        source/switch_runtime.h
    )

    target_include_directories(azahar-switch-libretro PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/source
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/externals/libretro-common/libretro-common/include
        ${NX_STANDARD_INCLUDE_DIRECTORIES}
    )
    target_link_libraries(azahar-switch-libretro PRIVATE
        azahar_libretro
        audio_core
        citra_common
        citra_core
        deko3d
        input_common
        libretro
        network
        video_core
        ${NX_STANDARD_LIBRARIES}
    )
    # libuam.a internal symbols are localised by externals/CMakeLists.txt via
    # objcopy --keep-global-symbols, so we don't need any link-time multiple-
    # definition hacks here.
    if (ENABLE_OPENGL)
        target_link_directories(azahar-switch-libretro PRIVATE
            $ENV{DEVKITPRO}/portlibs/switch/lib
        )
        target_link_libraries(azahar-switch-libretro PRIVATE EGL glapi drm_nouveau)
        if (TARGET glad)
            target_link_libraries(azahar-switch-libretro PRIVATE glad)
        endif()
    endif()
    if (AZAHAR_SWITCH_OPENGL_SPIKE)
        target_compile_definitions(azahar-switch-libretro PRIVATE AZAHAR_SWITCH_OPENGL_SPIKE)
        target_include_directories(azahar-switch-libretro PRIVATE
            $ENV{DEVKITPRO}/portlibs/switch/include
        )
    endif()

    nx_generate_nacp(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/azahar-switch-libretro.nacp
        NAME "Azahar LR"
        AUTHOR "Azahar Emulator Project"
        VERSION "0.1.0"
    )

    nx_create_nro(
        azahar-switch-libretro-nro
        TARGET azahar-switch-libretro
        OUTPUT ${CMAKE_BINARY_DIR}/bin/azahar-switch-libretro.nro
        NACP ${CMAKE_CURRENT_BINARY_DIR}/azahar-switch-libretro.nacp
    )
endif()
