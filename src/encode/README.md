# Built-in MP3, AAC-LC and FLAC encoders

These are cordinal's original encoders. They are compiled directly into the player; no
separate library, executable or system codec is loaded.

- `mp3_encoder.*`: MPEG-1 Layer III, 32/44.1/48 kHz, mono or stereo, standard CBR bitrates.
- `aac_lc_encoder.*`: AAC-LC, 32/44.1/48 kHz, mono or stereo, ADTS output with M4A-friendly
  priming. Only the ISO numeric Huffman and scalefactor-band tables are shared with cordinal's
  own decoders in `../audio_*_tables.h`.
- `flac_encoder.*`: FLAC (RFC 9639), 4 to 32 bits, 1 to 8 channels, in 4096-frame blocks. It
  chooses per channel among constant, verbatim, fixed and LPC (order 12 or less) prediction, with
  wasted bits, partitioned Rice codes and, for stereo, left/side, side/right or mid/side. Output
  stays within FLAC's streamable subset for sample rates the frame header can state. Callers
  write the metadata blocks and patch STREAMINFO (`FlacStreamInfo`) with the frame sizes and the
  MD5 once the audio is written.
- `bitwriter.h`: MSB-first bit writer used by all three encoders.

The encoder code is part of cordinal and is covered by the repository's GPL-3.0 license. The
Windows executable embeds that notice as RCDATA; Linux packages and CMake installations include
the same repository license.

AAC emits two leading silence frames and three tail frames. The M4A writer trims the resulting
3072-sample delay through the edit list and iTunSMPB. MP3 frames are raw CBR and carry the codec's
normal filterbank delay; callers that need sample accuracy should account for it.
