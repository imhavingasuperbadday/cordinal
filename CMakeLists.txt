# The native Linux build (64-bit). Windows builds with build.bat.
#
#   cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release && cmake --build build-linux
#
# Needs no libraries beyond glibc. Windows and input go over the Wayland and X11 protocols and
# drawing is software, in src/linux/platform/ (SDL3's API, implemented in-repo). HTTPS is BearSSL,
# compiled in from src/third_party/bearssl. Fonts are found and matched by the player's own index (src/linux/fontmatch.cpp),
# text is shaped and drawn by its own text stack (src/linux/typeface.cpp, shaper.cpp, textlayout.cpp),
# D-Bus is spoken by its own client (src/linux/dbus.cpp), and sound goes to PipeWire or PulseAudio
# through their native protocols (src/linux/pw_native.cpp, pulse_native.cpp), not a library.
cmake_minimum_required(VERSION 3.20)
project(cordinal VERSION 0.1.17 LANGUAGES C CXX)

if(WIN32)
    message(FATAL_ERROR "On Windows, build with build.bat")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# Sanitizer builds (README.md, "Fuzzing and sanitizers"). CORDINAL_SANITIZE compiles and links
# everything with AddressSanitizer and UndefinedBehaviorSanitizer, and any report ends the program,
# so the core self-tests (linux/smoke-test.sh) fail on a memory error that would not crash.
# CORDINAL_FUZZ adds the libFuzzer targets of tools/fuzz-media.cpp and needs clang. Both use the
# system's C library and allocator, which the sanitizers replace, so they turn off CORDINAL_PORTABLE
# and CORDINAL_STATIC (cordinal's own C library) and make the ordinary dynamic build:
#   cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCORDINAL_FUZZ=ON
option(CORDINAL_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(CORDINAL_FUZZ "Build the libFuzzer targets (clang; implies CORDINAL_SANITIZE)" OFF)
if(CORDINAL_FUZZ)
    set(CORDINAL_SANITIZE ON)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "CORDINAL_FUZZ needs clang for libFuzzer (-DCMAKE_CXX_COMPILER=clang++)")
    endif()
endif()
if(CORDINAL_SANITIZE)
    # Normal variables, which the option() calls below then leave alone (CMP0077).
    set(CORDINAL_PORTABLE OFF)
    set(CORDINAL_STATIC OFF)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
    if(CORDINAL_FUZZ)
        # Coverage feedback for libFuzzer in every object; only the targets link its main.
        add_compile_options(-fsanitize=fuzzer-no-link)
    endif()
endif()

# BearSSL, the TLS and certificate checks behind every HTTPS connection (THIRD_PARTY.md). Built from
# source so the program needs no TLS library from the distribution.
file(GLOB_RECURSE BEARSSL CONFIGURE_DEPENDS src/third_party/bearssl/src/*.c)
add_library(cordinal_bearssl STATIC ${BEARSSL})
target_include_directories(cordinal_bearssl PUBLIC src/third_party/bearssl/inc PRIVATE src/third_party/bearssl/src)
set_target_properties(cordinal_bearssl PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_compile_options(cordinal_bearssl PRIVATE -w)
# Two of BearSSL's designs are what UBSan reports on every connection, so those two checks are off for
# it alone; the rest of UBSan and ASan still cover it. Its classes are tables of functions taking the
# base type, which each implementation declares with its own context type and is called through a
# cast (function), and on x86 it loads and stores words at any address on purpose (BR_LE_UNALIGNED:
# alignment).
if(CORDINAL_SANITIZE)
    target_compile_options(cordinal_bearssl PRIVATE -fno-sanitize=function,alignment)
endif()

# The same native decoders and emulators as Windows. Only the SIMD unit needs AVX2/FMA flags;
# runtime dispatch keeps the rest of the player usable on baseline x64 machines.
file(GLOB CHIPS CONFIGURE_DEPENDS src/chiptune/*.cpp)
file(GLOB AUDIO CONFIGURE_DEPENDS src/audio_*.cpp)
file(GLOB ENCODE CONFIGURE_DEPENDS src/encode/*.cpp)
# Classic iPod support: the adapter and the pure C++ libgpod subset it drives.
file(GLOB IPOD CONFIGURE_DEPENDS src/ipod/*.cpp src/ipod/gpod/src/*.cpp)
set_source_files_properties(${ENCODE} PROPERTIES COMPILE_DEFINITIONS "_USE_MATH_DEFINES")
set_source_files_properties(src/audio_avx.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma;-mpclmul")

# The portable core, compiled against the Windows API subset in src/linux/compat.
add_library(cordinal_core STATIC
    src/album_editions.cpp src/album_editions_db.cpp src/album_editions_edit.cpp src/album_editions_title.cpp src/album_maker.cpp src/album_tagger.cpp src/archive.cpp src/autoplaylist.cpp src/column_layout.cpp src/cue.cpp src/decoder.cpp src/duplicates.cpp src/dsp.cpp src/upmix.cpp src/engine.cpp src/game_music.cpp src/inflate.cpp src/musicbrainz.cpp src/metadata_fetch.cpp src/coverfetch.cpp src/customcover.cpp src/cover_fade.cpp src/coverupload.cpp src/discart.cpp
    src/discord.cpp src/library.cpp src/librarydb.cpp src/listenbrainz.cpp src/migration.cpp src/backup.cpp src/playlog.cpp src/points.cpp src/taste.cpp src/radio.cpp src/radio_browser.cpp src/radio_stream.cpp src/radio_hls.cpp src/rediscover.cpp src/scanner.cpp src/sfx.cpp src/sort.cpp src/steam.cpp
    src/headsets.cpp src/helpers.cpp src/streamfile.cpp src/tags.cpp src/tags_fields.cpp src/tags_flacogg.cpp src/tags_id3.cpp src/tags_mp4.cpp src/tags_riff.cpp src/tagwriter.cpp src/tagwriter_flacogg.cpp src/tagwriter_mp3mp4.cpp
    src/tagwriter_cord.cpp src/tagundo.cpp src/playlist_files.cpp src/playlist.cpp src/md5.cpp src/cord_encode.cpp src/cord_export.cpp
    src/convert.cpp src/cdrip.cpp src/organize.cpp src/devicecopy.cpp src/collection.cpp src/linux/recycle.cpp src/linux/fontmatch.cpp
    src/linux/typeface.cpp src/linux/raster.cpp src/linux/shaper.cpp src/linux/textlayout.cpp
    src/titleformat.cpp src/peaks.cpp src/tempo.cpp src/theme_palette.cpp src/replaygain.cpp src/eq_profile.cpp
    src/tag_suggest.cpp src/keymap.cpp src/packs.cpp src/web_audio.cpp src/web_catalog.cpp src/web_http.cpp src/web_library.cpp src/web_server.cpp src/web_ui.cpp
    src/web_aac.cpp src/web_acme.cpp src/web_dlna.cpp src/web_reach.cpp src/web_remote.cpp src/qr.cpp src/speakers.cpp src/linux/tls_bearssl.cpp src/linux/tls_der.cpp
    src/soundcloud.cpp src/ytaudio.cpp src/spotifylink.cpp src/webfetch.cpp src/mp4_remux.cpp
    src/playlist_import.cpp src/release_radar.cpp src/discovery.cpp src/online_search.cpp
    ${AUDIO} ${CHIPS} ${ENCODE} ${IPOD} src/linux/compat/compat.cpp src/linux/audio_linux.cpp src/linux/sfx_linux.cpp
    src/linux/sound_stream.cpp src/linux/spa_pod.cpp src/linux/pw_native.cpp src/linux/pulse_native.cpp)
target_include_directories(cordinal_core PUBLIC src src/linux/compat src/ipod/gpod/include)
target_compile_definitions(cordinal_core PUBLIC UNICODE _UNICODE NOMINMAX)
# Releases ship Light, Dark and Apollo; ON compiles in the other themes and embeds their art (theme_palette.h).
option(CORDINAL_EXTRA_THEMES "Include the themes beyond Light and Dark" OFF)
if(CORDINAL_EXTRA_THEMES)
    target_compile_definitions(cordinal_core PUBLIC CORDINAL_EXTRA_THEMES)
endif()
target_compile_definitions(cordinal_core PUBLIC "__forceinline=inline __attribute__((always_inline))")
target_compile_options(cordinal_core PRIVATE -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-unknown-pragmas)
target_link_libraries(cordinal_core PUBLIC Threads::Threads cordinal_bearssl)

add_executable(cordinal-core-test src/linux/core_test.cpp src/linux/waybar.cpp src/linux/dbus.cpp)
target_link_libraries(cordinal-core-test PRIVATE cordinal_core)

if(CORDINAL_FUZZ)
    # One libFuzzer program per target in tools/fuzz-media.cpp; tools/fuzz-media.sh runs them.
    foreach(target flac mp3 opus vorbis aac tags art decoders)
        add_executable(fuzz-${target} tools/fuzz-media.cpp)
        target_compile_definitions(fuzz-${target} PRIVATE CORDINAL_FUZZ_TARGET="${target}")
        target_link_libraries(fuzz-${target} PRIVATE cordinal_core)
        target_link_options(fuzz-${target} PRIVATE -fsanitize=fuzzer)
    endforeach()
    target_compile_definitions(fuzz-art PRIVATE CORDINAL_FUZZ_IMAGES)
    add_custom_target(fuzzers DEPENDS fuzz-flac fuzz-mp3 fuzz-opus fuzz-vorbis fuzz-aac fuzz-tags fuzz-art fuzz-decoders)
endif()
# The official Opus conformance vectors through cordinal's decoder (tools/opus-conformance.sh). Its own
# copy of audio_opus.cpp prints each frame's range-coder state (CORDINAL_CODEC_TRACE) and stands in
# for the library's, which then is not linked.
add_executable(cordinal-opus-conformance EXCLUDE_FROM_ALL tools/opus-conformance.cpp src/audio_opus.cpp)
target_compile_definitions(cordinal-opus-conformance PRIVATE CORDINAL_CODEC_TRACE)
target_link_libraries(cordinal-opus-conformance PRIVATE cordinal_core)

# The player.
add_executable(cordinal
    src/linux/main.cpp src/linux/ui.cpp src/linux/radio_ui.cpp src/linux/text.cpp src/linux/settings.cpp src/linux/library_sync.cpp
    src/linux/playback.cpp src/linux/cover.cpp src/linux/mpris.cpp src/linux/waybar.cpp src/linux/dbus.cpp src/linux/web.cpp src/linux/replaygain.cpp
    src/linux/tagwrite.cpp src/linux/panels.cpp src/linux/panel_album.cpp src/linux/albumtag_ui.cpp src/linux/duplicates_ui.cpp src/linux/panel_eq.cpp src/linux/panel_organize.cpp
    src/linux/panel_comeback.cpp src/linux/panel_palette.cpp src/linux/panel_points.cpp src/linux/panel_prefs.cpp src/linux/panel_rules.cpp src/linux/panel_setup.cpp
    src/linux/panel_tags.cpp src/linux/panel_convert.cpp src/linux/panel_rip.cpp src/linux/convert_run.cpp src/linux/expand.cpp
    src/linux/organize_run.cpp src/linux/copy_run.cpp src/linux/collection_run.cpp src/linux/ipod_run.cpp src/linux/libview.cpp src/linux/libview_canvas.cpp src/linux/scrobble.cpp src/linux/sounds.cpp src/linux/points_ui.cpp src/linux/lyrics.cpp src/linux/mini.cpp src/linux/lightweight.cpp src/linux/keys.cpp src/linux/panel_keys.cpp src/linux/packs_ui.cpp src/linux/helpers_ui.cpp src/linux/preferences_ui.cpp src/linux/games.cpp src/linux/editions_ui.cpp src/linux/coverfetch_ui.cpp src/linux/artistinfo_ui.cpp src/linux/webfetch_ui.cpp src/linux/panel_radar.cpp
    src/linux/ui_model.cpp src/linux/ui_layout.cpp src/linux/ui_menus.cpp src/linux/ui_dialogs.cpp
    src/linux/discover_ui.cpp src/linux/search_online_ui.cpp
    src/linux/ui_draw.cpp src/linux/icons.cpp src/linux/ui_input.cpp src/linux/ui_files.cpp src/linux/ui_checks.cpp src/linux/queue_ui.cpp
    src/tests/selftest_linux_packs.cpp src/tests/selftest_linux_headsets.cpp
    src/linux/headsets.cpp src/linux/headphones_ui.cpp src/linux/output_profiles.cpp
    src/linux/platform/video.cpp src/linux/platform/render.cpp src/linux/platform/image_write.cpp src/linux/platform/keys.cpp
    src/linux/platform/desktop.cpp src/linux/platform/overlay.cpp src/linux/platform/x11.cpp src/linux/platform/x11_wire.cpp src/linux/platform/xkb_keymap.cpp src/linux/platform/wayland_wire.cpp src/linux/platform/wayland.cpp
    src/linux/helper_feed.cpp src/tests/selftest_linux_helpers.cpp)
target_sources(cordinal PRIVATE src/linux/migration.cpp src/linux/fountain.cpp src/linux/alternate_ui.cpp src/tests/selftest_linux_alternate.cpp)
target_sources(cordinal PRIVATE src/linux/big_ui.cpp src/tests/selftest_linux_big.cpp src/tests/selftest_linux_queue.cpp
    src/tests/selftest_linux_lightweight.cpp)

# The Windows icon file, whose images are PNGs: the window's icon, the About box's and the web player's.
# GENERATE preserves timestamps when bytes are unchanged, so repeated configuration does not
# recompile the text, sound and fountain modules just because their embedded assets were rewritten.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/cordinal.ico" APP_ICON HEX)
string(REGEX REPLACE "(..)" "0x\\1," APP_ICON "${APP_ICON}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/app_icon.inc" CONTENT "static const unsigned char kAppIcon[] = {${APP_ICON}};\n")
# Selawik, Microsoft's open stand-in for Segoe UI, as the UI font where neither is installed, so the
# window reads as the Windows build does. It is SIL OFL 1.1 (src/third_party/selawik/OFL.txt), which the
# FSF counts as GPL-incompatible, so it is compiled in only when asked for; otherwise an installed
# Segoe UI or Selawik is still preferred, then the desktop's sans.
option(CORDINAL_SELAWIK "Compile in the Selawik UI font (SIL OFL 1.1) for the Windows look" OFF)
set(UI_FONTS "")
foreach(face selawk selawkb)
    if(CORDINAL_SELAWIK)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/selawik/${face}.ttf" FACE_HEX HEX)
        string(REGEX REPLACE "(..)" "0x\\1," FACE_HEX "${FACE_HEX}")
    else()
        set(FACE_HEX "0")  # one zero byte: no face
    endif()
    string(APPEND UI_FONTS "static const unsigned char kFont_${face}[] = {${FACE_HEX}};\n")
endforeach()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ui_fonts.inc" CONTENT "${UI_FONTS}")
# The Apollo theme's art is part of every build.
set(APOLLO_IMAGES "")
foreach(image toolbar rebar_capacala rebar_apollo rebar_letters rebar_notes)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/apollo/${image}.png" IMAGE_HEX HEX)
    string(REGEX REPLACE "(..)" "0x\\1," IMAGE_HEX "${IMAGE_HEX}")
    string(APPEND APOLLO_IMAGES "static const unsigned char kApollo_${image}[] = {${IMAGE_HEX}};\n")
endforeach()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/apollo_images.inc" CONTENT "${APOLLO_IMAGES}")
if(CORDINAL_EXTRA_THEMES)
    # Embed the existing source font; neither executable needs a loose asset at runtime.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/deltarune/deltarune.ttf" WEB_FONT HEX)
    string(REGEX REPLACE "(..)" "0x\\1," WEB_FONT "${WEB_FONT}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/web_font.inc" CONTENT "static const unsigned char kWebFont[] = {${WEB_FONT}};\n")
    # The DELTARUNE theme's sounds are embedded the same way (the Windows build links them as resources).
    set(GAME_SOUNDS "")
    foreach(sound select menumove noise save bell error cantselect swing levelup txtq queen_bitcrushlaugh)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/deltarune/snd_${sound}.flac" SOUND_HEX HEX)
        string(REGEX REPLACE "(..)" "0x\\1," SOUND_HEX "${SOUND_HEX}")
        string(APPEND GAME_SOUNDS "static const unsigned char kSnd_${sound}[] = {${SOUND_HEX}};\n")
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/game_sounds.inc" CONTENT "${GAME_SOUNDS}")

    set(FOUNTAIN_IMAGES "")
    foreach(sprite edge basin texture)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/deltarune/fountain_${sprite}.png" SPRITE_HEX HEX)
        string(REGEX REPLACE "(..)" "0x\\1," SPRITE_HEX "${SPRITE_HEX}")
        string(APPEND FOUNTAIN_IMAGES "static const unsigned char kFountain_${sprite}[] = {${SPRITE_HEX}};\n")
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fountain_images.inc" CONTENT "${FOUNTAIN_IMAGES}")
else()
    # One zero byte in place of each asset, which nothing reads without the themes.
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/web_font.inc" CONTENT "static const unsigned char kWebFont[] = {0};\n")
    set(GAME_SOUNDS "")
    foreach(sound select menumove noise save bell error cantselect swing levelup txtq queen_bitcrushlaugh)
        string(APPEND GAME_SOUNDS "static const unsigned char kSnd_${sound}[] = {0};\n")
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/game_sounds.inc" CONTENT "${GAME_SOUNDS}")
    set(FOUNTAIN_IMAGES "")
    foreach(sprite edge basin texture)
        string(APPEND FOUNTAIN_IMAGES "static const unsigned char kFountain_${sprite}[] = {0};\n")
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fountain_images.inc" CONTENT "${FOUNTAIN_IMAGES}")
endif()
target_include_directories(cordinal PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" src/linux)
target_compile_options(cordinal PRIVATE -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers)
target_link_libraries(cordinal PRIVATE cordinal_core)

# A portable program: the C++ runtime is linked in, and on x86-64 glibc the program asks only for
# symbols glibc 2.31 has (src/linux/glibc_compat.cpp), so the build from Debian 13 runs on Debian 11,
# Ubuntu 20.04 and newer as well. Nothing else is linked: windows, sound, TLS, fonts and text are
# all in-repo. CORDINAL_PORTABLE=OFF makes an ordinary build against this system's libraries.
option(CORDINAL_PORTABLE "Link the C++ runtime statically and keep to older glibc symbols" ON)
# Stage 9 (docs/linux-libc.md): link one program with no shared library at all, so it needs nothing
# from the system, musl distributions such as Alpine included. ON by default (maintainer's decision,
# November 18, 2026): this is the shipped program, and the suites run it. CORDINAL_STATIC=OFF keeps
# the portable dynamic build with its older glibc symbol bindings, for comparisons.
option(CORDINAL_STATIC "Link a program with no shared library (glibc stage 9)" ON)
if(CORDINAL_STATIC AND (NOT CORDINAL_PORTABLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
    message(FATAL_ERROR "CORDINAL_STATIC needs the x86-64 portable build, which supplies the C library")
endif()
if(CORDINAL_PORTABLE)
    if(NOT CORDINAL_STATIC)
        target_sources(cordinal PRIVATE src/linux/glibc_compat.cpp)
    endif()
    target_link_options(cordinal PRIVATE -static-libstdc++ -static-libgcc)
    # The core test runs the same C++ runtime as the player. A shared libstdc++ would bring glibc's own
    # pthread_cond_wait into a program whose mutexes and condition variables are cordinal's (stage 8),
    # and the two protocols cannot be mixed: glibc's wait corrupts the mutex's bookkeeping and asserts
    # once another thread contends for it.
    target_link_options(cordinal-core-test PRIVATE -static-libstdc++ -static-libgcc)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_definitions(cordinal PRIVATE CORDINAL_GLIBC_COMPAT)
        # cordinal's own kernel calls, string functions, clocks, files, streams, formatting and math
        # in place of glibc's (src/linux/libc). The core test program uses them too, so its checks run
        # on them. The string functions, the allocator, stdio, printf, scanf and the math functions
        # must not be compiled into calls to themselves. CORDINAL_OWN_MALLOC=OFF keeps glibc's
        # allocator, to compare the two.
        option(CORDINAL_OWN_MALLOC "Use cordinal's allocator rather than glibc's" ON)
        set(OWN_LIBC src/linux/libc/kernel.cpp src/linux/libc/string.cpp src/linux/libc/time.cpp
            src/linux/libc/files.cpp src/linux/libc/stdio.cpp src/linux/libc/printf.cpp
            src/linux/libc/scanf.cpp src/linux/libc/numbers.cpp src/linux/libc/locale.cpp
            src/linux/libc/locale_data.cpp
            src/linux/libc/iconv.cpp src/linux/libc/net.cpp src/linux/libc/math.cpp
            src/linux/libc/misc.cpp src/linux/libc/threads.cpp src/linux/libc/start.cpp)
        # exit.cpp is the static program's exit path (stage 9). It is not linked into the portable
        # dynamic build on purpose: a hidden atexit there would take stdio's final flush onto the
        # program's own list, which only the program's exit runs, and glibc's __libc_start_main still
        # ends that build, so the flush would never happen.
        if(CORDINAL_STATIC)
            list(APPEND OWN_LIBC src/linux/libc/exit.cpp src/linux/libc/loader.cpp)
        endif()
        if(CORDINAL_OWN_MALLOC)
            list(APPEND OWN_LIBC src/linux/libc/malloc.cpp)
        else()
            target_compile_definitions(cordinal-core-test PRIVATE CORDINAL_GLIBC_MALLOC)
        endif()
        set_source_files_properties(src/linux/libc/malloc.cpp PROPERTIES COMPILE_OPTIONS "-fno-builtin")
        set_source_files_properties(src/linux/libc/string.cpp PROPERTIES
            COMPILE_OPTIONS "-fno-builtin;-fno-tree-loop-distribute-patterns")
        # exit.cpp defines exit and abort, which the compiler also knows as builtins.
        set_source_files_properties(src/linux/libc/exit.cpp PROPERTIES COMPILE_OPTIONS "-fno-builtin")
        # start.cpp runs before the thread pointer exists, so nothing in it may read thread-local
        # storage: the compiler's stack-protector prologue would read the canary at %fs:0x28 and
        # fault at the first instruction. Its own calls are raw system calls for the same reason.
        set_source_files_properties(src/linux/libc/start.cpp PROPERTIES COMPILE_OPTIONS "-fno-stack-protector")
        # stdio, printf, scanf, the number parsers, the locale layer, iconv, the network names and the
        # last imports (strerror, fnmatch, qsort_r, sysconf and strfromf128 among them) define the
        # standard functions the compiler also knows as builtins, so their own calls must
        # not be rewritten into calls to themselves (a fortified sprintf would otherwise call the
        # __sprintf_chk beside it, and strtod's definition would be flagged as mismatching the
        # compiler's own declaration).
        set_source_files_properties(src/linux/libc/stdio.cpp src/linux/libc/printf.cpp
            src/linux/libc/scanf.cpp src/linux/libc/numbers.cpp src/linux/libc/locale.cpp
            src/linux/libc/iconv.cpp src/linux/libc/net.cpp src/linux/libc/math.cpp
            src/linux/libc/misc.cpp
            PROPERTIES COMPILE_OPTIONS "-fno-builtin")
        target_sources(cordinal PRIVATE ${OWN_LIBC})
        target_sources(cordinal-core-test PRIVATE ${OWN_LIBC})
        target_compile_definitions(cordinal-core-test PRIVATE CORDINAL_OWN_LIBC)
        # Each name is bound to an older glibc version by its __wrap_ function in glibc_compat.cpp.
        # A static program has no glibc to bind to (glibc_compat.cpp is not compiled there), so it
        # needs no wraps and no libpthread. The pthread family and __libc_single_threaded are
        # cordinal's own since stage 8 (libc/threads.cpp), so they need no binding either.
        if(NOT CORDINAL_STATIC)
            foreach(name __libc_start_main _dl_find_object)
                target_link_options(cordinal PRIVATE "-Wl,--wrap=${name}")
            endforeach()
            # Before glibc 2.34 the pthread functions are in libpthread.so.0 (since then an empty stub).
            target_link_libraries(cordinal PRIVATE "-Wl,--push-state,--no-as-needed" "-l:libpthread.so.0" "-Wl,--pop-state")
        endif()
        if(CORDINAL_STATIC)
            # The static program: one file with no shared library, no dynamic loader and nothing from
            # the distribution, position-independent so the kernel's ASLR still applies. -static-pie
            # asks for that form and adds --no-dynamic-linker, so the link has no PT_INTERP either;
            # the program relocates itself in start.cpp before it reads a global. -nostdlib drops the
            # startup files, glibc and the driver's own -lstdc++/-lgcc, so the C++ runtime and the
            # unwinder are named here; the entry point is the program's own _start, and CORDINAL_STATIC
            # tells files.cpp that the environment array is the program's. --eh-frame-hdr is what
            # libgcc's unwinder reads: without it the link has no PT_GNU_EH_FRAME and the first throw
            # aborts. -fPIE compiles every file for a link that moves, which a non-PIC object's
            # absolute 32-bit relocations would refuse.
            set(CORDINAL_STATIC_LINK "-Wl,--start-group" "-lstdc++" "-lgcc" "-lgcc_eh" "-Wl,--end-group")
            # The program's exported set (linux/cordial-export.map): CMake turns the list into the table
            # loader.cpp resolves a loaded object's imports through, which is what carries the surface.
            # The dynamic build exports nothing: only this block reads the file.
            file(READ "${CMAKE_CURRENT_SOURCE_DIR}/linux/cordial-export.map" CORDINAL_EXPORT_MAP)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS linux/cordial-export.map)
            # One name per line in the script's global list; everything else there is a comment or the
            # script's own syntax. A CMake list is the simplest way to walk the lines, and the match
            # keeps only a line that is exactly one name.
            string(REPLACE "\n" ";" CORDINAL_EXPORT_LINES "${CORDINAL_EXPORT_MAP}")
            set(CORDINAL_EXPORT_DECLARATIONS "")
            set(CORDINAL_EXPORT_TABLE "")
            foreach(entry ${CORDINAL_EXPORT_LINES})
                if(entry MATCHES "^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*$")
                    set(name "${CMAKE_MATCH_1}")
                    if(name STREQUAL "__environ")
                        # The one data symbol in the list is a pointer, not bytes: declare it as
                        # loader.cpp does (its own extern matches, so no conflicting declaration),
                        # and store its address, which is what dlsym hands back.
                        string(APPEND CORDINAL_EXPORT_DECLARATIONS "extern \"C\" char** ${name};\n")
                        string(APPEND CORDINAL_EXPORT_TABLE "    {\"${name}\", (void*)&${name}},\n")
                    else()
                        string(APPEND CORDINAL_EXPORT_DECLARATIONS "extern \"C\" char ${name}[];\n")
                        string(APPEND CORDINAL_EXPORT_TABLE "    {\"${name}\", (void*)${name}},\n")
                    endif()
                endif()
            endforeach()
            # loader.cpp declares each name as a byte array rather than a function of the right type:
            # the table needs only the address, and no prototype can be wrong. -fno-builtin keeps the
            # compiler from knowing strlen and friends as functions.
            file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/loader_exports.inc"
                 "// Generated from linux/cordial-export.map by CMakeLists.txt; do not edit.\n"
                 "namespace cordlibc_loader {\nextern \"C\" {\n${CORDINAL_EXPORT_DECLARATIONS}}\n"
                 "const Export kProgramExports[] = {\n${CORDINAL_EXPORT_TABLE}};\n}\n")
            # The allocator's names are the only libc definitions with default visibility (malloc.cpp
            # leaves them as the compiler's declarations are; every other file hides its definitions).
            # A static link can put only those in the executable's dynamic symbol table: a version script
            # cannot promote a hidden definition, and naming a hidden one makes the references to it
            # dynamic and then fails to find them (measured: five names core_test.cpp uses stopped
            # resolving). So the linker is given the allocator names, tagged by a generated version
            # script, and loader.cpp resolves the rest of the surface through the table above.
            set(CORDINAL_STATIC_EXPORTS aligned_alloc calloc free malloc malloc_trim malloc_usable_size
                memalign posix_memalign pvalloc realloc valloc)
            set(CORDINAL_LINKER_LIST "")
            foreach(name ${CORDINAL_STATIC_EXPORTS})
                string(APPEND CORDINAL_LINKER_LIST "    ${name};\n")
            endforeach()
            file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cordial-export.list" "{\n${CORDINAL_LINKER_LIST}};\n")
            file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cordial-export-symbols.map"
                 "/* Generated from CMakeLists.txt's CORDINAL_STATIC_EXPORTS; do not edit. The part of\n"
                 "   linux/cordial-export.map a static link can export (see that file's comment). */\n"
                 "CORDINAL_1 {\n  global:\n${CORDINAL_LINKER_LIST}  local:\n    *;\n};\n")
            set_source_files_properties(src/linux/libc/loader.cpp PROPERTIES COMPILE_OPTIONS "-fno-builtin")
            foreach(target cordinal cordinal-core-test)
                target_compile_definitions(${target} PRIVATE CORDINAL_STATIC)
                target_compile_options(${target} PRIVATE -fPIE)
                target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
                target_link_options(${target} PRIVATE -static-pie -nostdlib "-Wl,-e,_start" "-Wl,--eh-frame-hdr"
                    "-Wl,--dynamic-list=${CMAKE_CURRENT_BINARY_DIR}/cordial-export.list"
                    "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/cordial-export-symbols.map")
                target_link_libraries(${target} PRIVATE ${CORDINAL_STATIC_LINK})
            endforeach()
        endif()
    endif()
endif()

install(TARGETS cordinal RUNTIME DESTINATION bin)
install(FILES LICENSE DESTINATION share/licenses/cordinal)
install(FILES linux/cordinal.desktop DESTINATION share/applications)
install(FILES docs/icon.png DESTINATION share/icons/hicolor/512x512/apps RENAME cordinal.png)

target_sources(cordinal PRIVATE src/tests/selftest_linux_layout.cpp src/tests/selftest_linux_tabs.cpp)

# The platform layer on its own: a window that prints its events, for tools/platform-gui-test.sh.
add_executable(cordinal-platform-probe EXCLUDE_FROM_ALL src/linux/platform/probe.cpp
    src/linux/platform/video.cpp src/linux/platform/render.cpp src/linux/platform/image_write.cpp src/linux/platform/keys.cpp
    src/linux/platform/desktop.cpp src/linux/platform/overlay.cpp src/linux/platform/x11.cpp src/linux/platform/x11_wire.cpp
    src/linux/platform/xkb_keymap.cpp src/linux/platform/wayland_wire.cpp src/linux/platform/wayland.cpp src/linux/dbus.cpp)
target_include_directories(cordinal-platform-probe PRIVATE src/linux src)
target_compile_options(cordinal-platform-probe PRIVATE -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers)
target_link_libraries(cordinal-platform-probe PRIVATE Threads::Threads)
