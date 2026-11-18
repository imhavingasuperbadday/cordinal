# Which self-test suites cover which files, for `test.bat -Changed` (tools\test.ps1). Every rule a
# changed path matches adds its suites; Paths are PowerShell -like patterns on repository paths with
# forward slashes. Test sources need no rule: `--list-tests` names the file each suite lives in.
#
# Suites '*' means every suite: use it for headers and scripts nearly everything depends on, where a
# narrow guess would let a regression through. Note is printed instead of (or beside) suites for
# changes no Windows suite checks. tools\test-runner.ps1 fails when a tracked source file under src\
# matches no rule or a rule names a suite that does not exist, so add a rule with any new file.
@{
    Rules = @(
        # Shared everywhere: a change here can break any suite.
        @{ Paths = @('src/app.h', 'src/app_state.h', 'src/library.h', 'src/tags.h', 'src/resource.h',
                     'src/native_audio.h', 'src/tests/selftest_common.h', 'src/tests/selftest_runner.cpp',
                     'src/tests/test_entry.cpp', 'tools/build.ps1', 'build.bat', 'src/cordinal.rc', 'src/*.manifest')
           Suites = @('*') }
        @{ Paths = @('src/tests/testlog.h', 'src/tests/test_report.*', 'src/stub/*'); Suites = @('harness') }
        @{ Paths = @('src/tests/*bench*', 'src/tests/reference_audio.*', 'src/tests/g726_reference.*')
           Suites = @('native-codecs', 'exact-pcm') }
        @{ Paths = @('src/tests/metadata_checks.h'); Suites = @('metadata') }
        @{ Paths = @('src/tests/selftest_linux_*', 'src/tests/selftest_hls.cpp', 'src/tests/selftest_radio_stream.cpp')
           Suites = @('radio', 'live-audio'); Note = 'Linux test sources also run under .\linux.bat.' }
        @{ Paths = @('src/tests/*'); Suites = @() }  # mapped through --list-tests; listed so the check sees a rule

        # Library and its views of the data.
        @{ Paths = @('src/library*', 'src/librarydb*', 'src/sections.*', 'src/track_set.*', 'src/textcase.*')
           Suites = @('library-db', 'library-workflow', 'search', 'sorting', 'scan-merge', 'title-format') }
        @{ Paths = @('src/scanner.*'); Suites = @('scanner', 'scan-merge', 'library-workflow') }
        @{ Paths = @('src/titleformat.*'); Suites = @('title-format', 'search') }
        @{ Paths = @('src/sort.*'); Suites = @('sorting') }
        @{ Paths = @('src/album_editions*', 'src/app_editions.*', 'src/app_groups.*'); Suites = @('album-editions', 'ui') }
        @{ Paths = @('src/album_maker.*', 'src/app_albummaker.*'); Suites = @('album-maker', 'ui') }
        @{ Paths = @('src/album_tagger.*', 'src/app_albumtag.*'); Suites = @('metadata', 'ui') }
        @{ Paths = @('src/duplicates.*', 'src/app_duplicates.*'); Suites = @('duplicates', 'ui') }
        @{ Paths = @('src/column_layout.*'); Suites = @('ui'); Note = 'Shared with Linux: also run .\linux.bat.' }
        @{ Paths = @('src/playlist.*', 'src/playlist_files.*', 'src/playlist_shuffle.*', 'src/app_playlists.*')
           Suites = @('playlist-files', 'storage', 'sorting') }
        @{ Paths = @('src/autoplaylist.*', 'src/app_autoplaylist.*'); Suites = @('auto-playlists') }
        @{ Paths = @('src/playlog.*', 'src/foobar_stats.*', 'src/app_foobar.*'); Suites = @('play-log', 'migration') }
        @{ Paths = @('src/migration.*', 'src/backup.*', 'src/app_backup.*', 'src/app_import.*'); Suites = @('migration') }
        @{ Paths = @('src/rediscover.*', 'src/taste.*', 'src/app_recent.*'); Suites = @('listening-model', 'play-log') }
        @{ Paths = @('src/points.*', 'src/app_points.*', 'src/app_badges.*'); Suites = @('points') }
        @{ Paths = @('src/streamfile.*', 'src/recycle.*'); Suites = @('storage', 'playlist-files', 'organize', 'tag-undo') }
        @{ Paths = @('src/organize.*', 'src/app_organize.*'); Suites = @('organize') }
        @{ Paths = @('src/devicecopy.*', 'src/app_copy.*'); Suites = @('copy') }
        @{ Paths = @('src/collection.*', 'src/app_collection.*'); Suites = @('collection') }
        @{ Paths = @('src/ipod.h', 'src/ipod/*', 'src/app_ipod.*'); Suites = @('ipod') }
        @{ Paths = @('src/headsets.*', 'src/app_headphones.*'); Suites = @('headsets') }
        @{ Paths = @('src/command_line.*'); Suites = @('explorer', 'harness') }

        # Tags.
        @{ Paths = @('src/tags*', 'src/tag_suggest.*', 'src/app_tageditor.*')
           Suites = @('tags', 'chapters-lyrics', 'replaygain-tags', 'steam') }
        @{ Paths = @('src/tagwriter*'); Suites = @('tag-writing-mp4', 'tag-writing-ogg', 'replaygain-tags', 'tag-undo') }
        @{ Paths = @('src/tagundo.*'); Suites = @('tag-undo') }
        @{ Paths = @('src/steam.*'); Suites = @('steam') }
        @{ Paths = @('src/cue.*'); Suites = @('cue') }

        # Playback, decoders and sound.
        @{ Paths = @('src/decoder.*', 'src/audio_*', 'src/md5.*')
           Suites = @('native-codecs', 'exact-pcm', 'gapless', 'ranges') }
        @{ Paths = @('src/audio_benchmark.*', 'src/audio_kernels.*', 'src/audio_bits.*'); Suites = @('sample-conversion') }
        @{ Paths = @('src/engine.*', 'src/audio_output.*', 'src/asio*', 'src/app_playback.*')
           Suites = @('engine', 'sample-conversion', 'crossfade', 'gapless') }
        @{ Paths = @('src/speakers.*', 'src/tests/selftest_speakers.cpp')
           Suites = @('speakers'); Note = 'Network speakers also play on Linux: run .\linux.bat for the shared code.' }
        @{ Paths = @('src/audio_resample.*', 'src/audio_polyphase.*'); Suites = @('resampler') }
        @{ Paths = @('src/dsp.*', 'src/eq_profile.*', 'src/app_eq.*'); Suites = @('equalizer', 'effects', 'loudness') }
        @{ Paths = @('src/tempo.*', 'src/upmix.*'); Suites = @('effects', 'loudness') }
        @{ Paths = @('src/replaygain.*'); Suites = @('loudness', 'replaygain-scan', 'replaygain-tags', 'duplicates') }
        @{ Paths = @('src/peaks.*', 'src/app_waveform.*'); Suites = @('peaks') }
        @{ Paths = @('src/sfx*'); Suites = @('sound-effects') }
        @{ Paths = @('src/game_music.*', 'src/chiptune/*', 'src/app_games.*', 'src/audio_midi.*', 'src/audio_module.*')
           Suites = @('game-music', 'loops', 'formats', 'nes', 'ym2612') }
        @{ Paths = @('src/inflate.*', 'src/archive.*'); Suites = @('gzip', 'formats') }
        @{ Paths = @('src/audio_dsd.*'); Suites = @('formats') }
        @{ Paths = @('src/dsd.h'); Suites = @('formats', 'engine') }
        @{ Paths = @('src/cord*', 'src/audio_cord.*', 'src/audio_encode.*', 'src/aac_encoder.*', 'src/encode/*',
                     'src/convert.*', 'src/app_convert.*')
           Suites = @('cord', 'cord-conversion') }
        # The Convert window, and the decoders Convert reads exactly (Decoder::LosslessFormat).
        @{ Paths = @('src/app_convert.*'); Suites = @('ui') }
        @{ Paths = @('src/audio_alac.*', 'src/audio_wavpack.*', 'src/audio_ape.*', 'src/audio_tta.*'); Suites = @('cord-conversion') }

        # Radio, web, online services and helpers.
        @{ Paths = @('src/radio*', 'src/app_radio.*', 'src/audio_live.*'); Suites = @('radio', 'live-audio') }
        @{ Paths = @('src/web_*', 'src/qr.*', 'src/app_web.*', 'src/third_party/bearssl/*'); Suites = @('web') }
        @{ Paths = @('src/metadata_fetch.*', 'src/coverfetch.*', 'src/customcover.*', 'src/musicbrainz.*',
                     'src/coverupload.*', 'src/app_coverfetch.*', 'src/app_artistinfo.*', 'src/app_review.*')
           Suites = @('metadata') }
        @{ Paths = @('src/app_lyrics.*'); Suites = @('chapters-lyrics', 'metadata') }
        @{ Paths = @('src/listenbrainz.*', 'src/discord.*'); Suites = @('listenbrainz') }
        @{ Paths = @('src/helpers.*', 'src/app_helpers.*'); Suites = @('helpers') }
        # settings.ini: read by nearly every feature, and backed up with the profile.
        @{ Paths = @('src/app_ini.*', 'src/app_settings.*'); Suites = @('ui', 'migration', 'headsets', 'helpers') }
        @{ Paths = @('src/packs.*', 'src/app_packs.*', 'src/app_packfiles.*'); Suites = @('packs') }
        @{ Paths = @('src/crashdump.*'); Suites = @('crash-dumps') }
        @{ Paths = @('src/memory_report.*', 'src/packed_image.*', 'src/delayload.*', 'src/background_cpu.*',
                     'src/app_memory.*', 'src/app_lightweight.*')
           Suites = @('memory', 'memory-budget') }

        # Window, menus, preferences, themes and drawing: the native UI suite, which needs a window.
        @{ Paths = @('src/app*', 'src/ui_spec.h', 'src/theme_*', 'src/libview*', 'src/art.*', 'src/thumbs.*',
                     'src/discart.*', 'src/soft_canvas.*', 'src/cover_fade.*', 'src/wind.*', 'src/smtc.*',
                     'src/keymap.*', 'src/apollo/*', 'src/deltarune/*')
           Suites = @('ui') }
        @{ Paths = @('src/theme_palette.*', 'src/ui_spec.h'); Suites = @('packs')
           Note = 'Shared layout or palette: also run .\linux.bat for the Linux layout check.' }

        # Vendored code with no suite of its own beyond the ones above.
        @{ Paths = @('src/third_party/*'); Suites = @('native-codecs', 'web') }

        # Outside the Windows suites.
        @{ Paths = @('src/linux/*', 'CMakeLists.txt', 'linux/*', 'linux.bat', 'tools/linux.ps1')
           Suites = @(); Note = 'Linux sources: run .\linux.bat (and .\linux.bat test -Suite e2e for UI changes).' }
        @{ Paths = @('tools/test.ps1', 'tools/test-runner.ps1', 'tools/test-map.psd1', 'test.bat')
           Suites = @('harness'); Note = 'Test runner changed: also run pwsh -File tools\test-runner.ps1.' }
        @{ Paths = @('tools/make-fixtures.ps1'); Suites = @('*') }
        @{ Paths = @('tools/fuzz-*', 'tools/opus-conformance.*')
           Suites = @(); Note = 'Fuzzers and conformance: run .\linux.bat test -Suite sanitize (and tools\fuzz-parsers.ps1 -BuildOnly on Windows).' }
        @{ Paths = @('tools/publish.ps1', 'tools/release.ps1', 'tools/version-fields.psd1', 'tools/check-updates.ps1')
           Suites = @(); Note = 'Release tooling: no suite covers it; inspect a publish.ps1 -Out export instead.' }
        @{ Paths = @('tools/check-unwind.py')
           Suites = @(); Note = 'Unwinding checker: rerun it as its header says (docs/size.md).' }
        @{ Paths = @('cdpack/*', 'tools/pack-*.ps1', 'tools/packing-mode.ps1')
           Suites = @(); Note = 'Packer: run build.bat cdpack and py -3 cdpack\tests\verify.py.' }
        @{ Paths = @('tools/make-code-profile.py', 'tools/code-profile.txt')
           Suites = @(); Note = 'Code profile and link order: run build.bat cdpack, then cordinal-test --memory-report on build\cordinal.exe.' }
    )
}
