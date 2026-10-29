# Built-in MP3 and AAC-LC encoders

The encoder-only sources come from [glint](https://github.com/CrispStrobe/glint), revision
`77738f3ed9b15f627196cc5bbd7f6406814ba2fb`, under the MIT license in LICENSE.txt.
The public header is limited to the two encoders; decoders, resampling, CLI programs, bindings,
upstream tests and build scripts are not included. Cordinal uses its own decoders and resampler.
The source is compiled directly into the player; no separate library, executable or system
codec is loaded.
The Windows executable embeds the full notice as RCDATA; Linux packages and CMake installation
include a readable copy of the same license.

Local adaptations: `.hpp` headers become `.h` so cordinal's incremental build tracks them;
includes use the local header; SIMD is fixed at baseline SSE2, avoiding shared mutable dispatch;
thread-pool support is disabled; integer lookup tables initialize on demand; experimental
environment overrides are removed. AAC adds 2048 samples of priming, recorded in M4A's edit list
and iTunSMPB. Native decoder round trips cover every published format and bitrate.
