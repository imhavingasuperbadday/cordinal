# The native Linux build (64-bit). Windows builds with build.bat.
#
#   cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release && cmake --build build-linux
#
# Needs SDL3, SDL3_ttf, PipeWire (libpipewire-0.3), libsystemd (sd-bus), fontconfig and OpenSSL 3
# (the web server's HTTPS away from home).
cmake_minimum_required(VERSION 3.20)
project(cordinal VERSION 0.1.1 LANGUAGES CXX)

if(WIN32)
    message(FATAL_ERROR "On Windows, build with build.bat")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
find_package(SDL3 REQUIRED CONFIG)
find_package(SDL3_ttf REQUIRED CONFIG)
find_package(Threads REQUIRED)
find_package(OpenSSL 3 REQUIRED)

# The same native decoders and emulators as Windows. Only the SIMD unit needs AVX2/FMA flags;
# runtime dispatch keeps the rest of the player usable on baseline x64 machines.
file(GLOB CHIPS CONFIGURE_DEPENDS src/chiptune/*.cpp)
file(GLOB AUDIO CONFIGURE_DEPENDS src/audio_*.cpp)
file(GLOB ENCODE CONFIGURE_DEPENDS src/encode/*.cpp)
set_source_files_properties(${ENCODE} PROPERTIES COMPILE_DEFINITIONS "GLINT_NO_THREADS;_USE_MATH_DEFINES")
set_source_files_properties(src/audio_avx.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma;-mpclmul")

# The portable core, compiled against the Windows API subset in src/linux/compat.
add_library(cordinal_core STATIC
    src/album_editions.cpp src/archive.cpp src/autoplaylist.cpp src/cue.cpp src/decoder.cpp src/dsp.cpp src/engine.cpp src/game_music.cpp src/inflate.cpp src/musicbrainz.cpp
    src/library.cpp src/librarydb.cpp src/listenbrainz.cpp src/migration.cpp src/backup.cpp src/playlog.cpp src/points.cpp src/radio.cpp src/radio_browser.cpp src/radio_stream.cpp src/radio_hls.cpp src/scanner.cpp src/sfx.cpp src/sort.cpp src/steam.cpp
    src/helpers.cpp src/streamfile.cpp src/tags.cpp src/tagwriter.cpp src/tagwriter_flacogg.cpp src/tagwriter_mp3mp4.cpp
    src/tagwriter_cord.cpp src/tagundo.cpp src/playlist_files.cpp src/playlist.cpp src/md5.cpp src/cord_encode.cpp src/cord_export.cpp
    src/convert.cpp src/organize.cpp src/devicecopy.cpp src/linux/recycle.cpp
    src/titleformat.cpp src/peaks.cpp src/theme_palette.cpp src/replaygain.cpp src/eq_profile.cpp
    src/packs.cpp src/web_audio.cpp src/web_catalog.cpp src/web_http.cpp src/web_library.cpp src/web_server.cpp src/web_ui.cpp
    src/web_aac.cpp src/web_acme.cpp src/web_reach.cpp src/web_remote.cpp src/qr.cpp src/linux/tls_openssl.cpp
    ${AUDIO} ${CHIPS} ${ENCODE} src/linux/compat/compat.cpp src/linux/audio_pipewire.cpp src/linux/sfx_pipewire.cpp)
target_include_directories(cordinal_core PUBLIC src src/linux/compat)
target_compile_definitions(cordinal_core PUBLIC UNICODE _UNICODE NOMINMAX)
# Releases ship Light and Dark only; ON compiles in the other themes and embeds their art (theme_palette.h).
option(CORDINAL_EXTRA_THEMES "Include the themes beyond Light and Dark" OFF)
if(CORDINAL_EXTRA_THEMES)
    target_compile_definitions(cordinal_core PUBLIC CORDINAL_EXTRA_THEMES)
endif()
target_compile_definitions(cordinal_core PUBLIC "__forceinline=inline __attribute__((always_inline))")
target_compile_options(cordinal_core PRIVATE -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-unknown-pragmas)
target_link_libraries(cordinal_core PUBLIC PkgConfig::PIPEWIRE Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

add_executable(cordinal-core-test src/linux/core_test.cpp)
target_link_libraries(cordinal-core-test PRIVATE cordinal_core)

# The player.
add_executable(cordinal
    src/linux/main.cpp src/linux/ui.cpp src/linux/radio_ui.cpp src/linux/text.cpp src/linux/settings.cpp src/linux/library_sync.cpp
    src/linux/playback.cpp src/linux/cover.cpp src/linux/mpris.cpp src/linux/web.cpp src/linux/replaygain.cpp
    src/linux/tagwrite.cpp src/linux/panels.cpp src/linux/cord_export.cpp src/linux/convert_run.cpp src/linux/expand.cpp
    src/linux/organize_run.cpp src/linux/copy_run.cpp src/linux/libview.cpp src/linux/scrobble.cpp src/linux/sounds.cpp src/linux/points_ui.cpp src/linux/lyrics.cpp src/linux/mini.cpp src/linux/packs_ui.cpp src/linux/helpers_ui.cpp src/linux/preferences_ui.cpp src/linux/games.cpp src/linux/editions_ui.cpp)
target_sources(cordinal PRIVATE src/linux/migration.cpp src/linux/fountain.cpp)

# The Windows icon file, whose images are PNGs: the window's icon, the About box's and the web player's.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/cordinal.ico" APP_ICON HEX)
string(REGEX REPLACE "(..)" "0x\\1," APP_ICON "${APP_ICON}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/app_icon.inc" "static const unsigned char kAppIcon[] = {${APP_ICON}};\n")
if(CORDINAL_EXTRA_THEMES)
    # Embed the existing source font; neither executable needs a loose asset at runtime.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/deltarune/deltarune.ttf" WEB_FONT HEX)
    string(REGEX REPLACE "(..)" "0x\\1," WEB_FONT "${WEB_FONT}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/web_font.inc" "static const unsigned char kWebFont[] = {${WEB_FONT}};\n")
    # The DELTARUNE theme's sounds are embedded the same way (the Windows build links them as resources).
    set(GAME_SOUNDS "")
    foreach(sound select menumove noise save bell error cantselect swing levelup txtq queen_bitcrushlaugh)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/deltarune/snd_${sound}.flac" SOUND_HEX HEX)
        string(REGEX REPLACE "(..)" "0x\\1," SOUND_HEX "${SOUND_HEX}")
        string(APPEND GAME_SOUNDS "static const unsigned char kSnd_${sound}[] = {${SOUND_HEX}};\n")
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_sounds.inc" "${GAME_SOUNDS}")

    set(FOUNTAIN_IMAGES "")
    foreach(sprite edge basin texture)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/deltarune/fountain_${sprite}.png" SPRITE_HEX HEX)
        string(REGEX REPLACE "(..)" "0x\\1," SPRITE_HEX "${SPRITE_HEX}")
        string(APPEND FOUNTAIN_IMAGES "static const unsigned char kFountain_${sprite}[] = {${SPRITE_HEX}};\n")
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fountain_images.inc" "${FOUNTAIN_IMAGES}")
else()
    # One zero byte in place of each asset, which nothing reads without the themes.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/web_font.inc" "static const unsigned char kWebFont[] = {0};\n")
    set(GAME_SOUNDS "")
    foreach(sound select menumove noise save bell error cantselect swing levelup txtq queen_bitcrushlaugh)
        string(APPEND GAME_SOUNDS "static const unsigned char kSnd_${sound}[] = {0};\n")
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_sounds.inc" "${GAME_SOUNDS}")
    set(FOUNTAIN_IMAGES "")
    foreach(sprite edge basin texture)
        string(APPEND FOUNTAIN_IMAGES "static const unsigned char kFountain_${sprite}[] = {0};\n")
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fountain_images.inc" "${FOUNTAIN_IMAGES}")
endif()
target_include_directories(cordinal PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(cordinal PRIVATE -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers)
target_link_libraries(cordinal PRIVATE cordinal_core SDL3::SDL3 SDL3_ttf::SDL3_ttf PkgConfig::SYSTEMD PkgConfig::FONTCONFIG)

install(TARGETS cordinal RUNTIME DESTINATION bin)
install(FILES src/encode/LICENSE.txt DESTINATION share/licenses/cordinal RENAME glint.txt)
install(FILES linux/cordinal.desktop DESTINATION share/applications)
install(FILES docs/icon.png DESTINATION share/icons/hicolor/512x512/apps RENAME cordinal.png)
