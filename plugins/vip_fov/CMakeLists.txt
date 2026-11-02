# vip_fov — Plugify C++ plugin: port of the Pisex VIP module "[VIP] FOV".
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no guard library. The map gate, the
# client/handle checks and the map-cached schema offset live in the plugin's own src/guards.cpp;
# the native s2sdk menu lifecycle lives in src/menus.cpp; the engine-free decisions are shared with
# the test through src/fov_logic.hpp.
#
#   plugify-vip_fov  the plugin shared library (entry "bin/vip_fov" -> bin/libvip_fov.so)
#   vip_fov_tests    engine-free unit + runtime regression tests (build/tests/vip_fov_tests)

set(VIP_FOV_VERSION "1.1.0")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_*       (s2sdk_bindings)
#   __vip_*         (vip consumer bindings, generated into plugins/vip/include/external/plugify/include)
#   __hudmenu_*     (hudmenu_bindings; the optional per-player "!menu" type choice)
# In the monorepo plugins/vip is added first ("vip" < "vip_fov"), so vip_bindings already exists;
# a standalone exported repo only carries plugins/vip/include/ headers + vip.cpp and has no
# plugins/vip/CMakeLists.txt - define a local fallback then. The fallback target names are unique
# to this plugin, so several ported plugins can fall back in the same build without colliding.
if(TARGET vip_bindings)
    set(VIP_FOV_VIP_BINDINGS $<TARGET_OBJECTS:vip_bindings>)
else()
    add_library(vip_fov_vip_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include/vip/vip.cpp")
    target_include_directories(vip_fov_vip_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(vip_fov_vip_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(VIP_FOV_VIP_BINDINGS $<TARGET_OBJECTS:vip_fov_vip_bindings>)
endif()
if(TARGET hudmenu_bindings)
    set(VIP_FOV_HUDMENU_BINDINGS $<TARGET_OBJECTS:hudmenu_bindings>)
else()
    add_library(vip_fov_hudmenu_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/sdk/include/hudmenu/hudmenu.cpp")
    target_include_directories(vip_fov_hudmenu_bindings SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(vip_fov_hudmenu_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(VIP_FOV_HUDMENU_BINDINGS $<TARGET_OBJECTS:vip_fov_hudmenu_bindings>)
endif()

# The engine-facing half of the plugin: the map gate, the client/handle checks, the map-cached
# schema offset and the native menu lifecycle. Compiled once into the plugin and once into the
# test; the __s2sdk_* binding globals are linked by the final targets, never by this library.
add_library(vip_fov_support STATIC
        src/guards.cpp
        src/menus.cpp
)
target_include_directories(vip_fov_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/plg/hudmenu headers must not fail -Werror.
target_include_directories(vip_fov_support SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(vip_fov_support PUBLIC plugify_sdk)
target_compile_options(vip_fov_support PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_fov_support PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

add_library(plugify-vip_fov SHARED
        src/plugin.cpp
        # The binding globals the plugify module fills from the manifest's dependencies; compiled
        # in, never linked as a library.
        $<TARGET_OBJECTS:s2sdk_bindings>
        ${VIP_FOV_VIP_BINDINGS}
        ${VIP_FOV_HUDMENU_BINDINGS}
)
target_include_directories(plugify-vip_fov PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated vip/s2sdk/plg/hudmenu headers must not fail -Werror.
target_include_directories(plugify-vip_fov SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-vip_fov PRIVATE
        vip_fov_support
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-vip_fov PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-vip_fov PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_fov"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-vip_fov PROPERTIES OUTPUT_NAME vip_fov)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-vip_fov PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/vip_fov.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/vip_fov.pplugin"
)

# --- tests --------------------------------------------------------------------------------------
# vip_fov_tests: no engine and no Plugify host. The plugin TU is compiled into the test (like
# vip_test's test), so the whole lifecycle - OnPluginStart/OnPluginUpdate/OnPluginEnd, the VIP
# callbacks and the native menu handler - runs exactly as on a server while every __s2sdk_* /
# __vip_* / __hudmenu_* binding slot is a recording fake. Covered: the pure logic (list parsing,
# guarded stoi, item dispatch, page mapping, menu type choice), the map gate (B1/B2), the
# controller/handle chain (C1/C2, including valid signed handles), the spawn apply (default 90,
# stored cookie, malformed cookie), the full menu flow (create/select/re-open, back -> VIP menu,
# exit/timeout, page guard), a map change, menu reuse without leaks, missing bindings and a clean
# teardown + reload (A1/A7/C8).
add_executable(vip_fov_tests
        tests/vip_fov_tests.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        ${VIP_FOV_VIP_BINDINGS}
        ${VIP_FOV_HUDMENU_BINDINGS}
)
target_include_directories(vip_fov_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(vip_fov_tests SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(vip_fov_tests PRIVATE
        vip_fov_support
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(vip_fov_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_fov_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_fov"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(vip_fov_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
