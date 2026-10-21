bogodroid_add_unityloader_plugin(brickgamepro
    brickgamepro.cpp
    gameover_icons.cpp
    casing_art.cpp
    exit_icons.cpp
)

# Keep the user's transparent black icon in the title plugin, so deployment
# needs only the .so and cannot accidentally miss a separate UI resource.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS assets/gameover-icon.png)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/assets/gameover-icon.png" finish_icons_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," finish_icons_bytes "${finish_icons_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gameover_icons_png.h"
    "static const unsigned char kFinishIconsPng[] = {${finish_icons_bytes}};\n")
target_include_directories(bogodroid_unityloader_plugin_brickgamepro PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS assets/exit-game.png)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/assets/exit-game.png" exit_icon_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," exit_icon_bytes "${exit_icon_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/exit_icons_png.h"
    "static const unsigned char kExitIconPng[] = {${exit_icon_bytes}};\n")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS assets/casing-girl.png)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/assets/casing-girl.png" casing_art_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," casing_art_bytes "${casing_art_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/casing_art_png.h"
    "static const unsigned char kCasingArtPng[] = {${casing_art_bytes}};\n")
