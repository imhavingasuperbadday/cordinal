# Built-in MP3 and AAC-LC encoders

These are cordinal's original encoders. They are compiled directly into the player; no
separate library, executable or system codec is loaded.

- `mp3_encoder.*`: MPEG-1 Layer III, 32/44.1/48 kHz, mono or stereo, standard CBR bitrates.
- `aac_lc_encoder.*`: AAC-LC, 32/44.1/48 kHz, mono or stereo, ADTS output with M4A-friendly
  priming. Only the ISO numeric Huffman and scalefactor-band tables are shared with cordinal's
  own decoders in `../audio_*_tables.h`.
- `bitwriter.h`: MSB-first bit writer used by both encoders.

The encoder code is part of cordinal and is covered by the repository's GPL-3.0 license. The
Windows executable embeds that notice as RCDATA; Linux packages and CMake installations include
the same repository license.

AAC emits two leading silence frames and three tail frames. The M4A writer trims the resulting
3072-sample delay through the edit list and iTunSMPB. MP3 frames are raw CBR and carry the codec's
normal filterbank delay; callers that need sample accuracy should account for it.
