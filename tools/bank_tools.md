## Sound Bank Tools
The bank packer and unpacker work on the `.SBK` (sound effects) and `.MUS` (music) files. Both use the same container, so one pair of tools handles both. They work for Jak 1, 2 and 3 (Jak 3 has no `.MUS` files).

A bank has:
- a **descriptor**: the sounds, and for every sound the grains or programs and tones that say how to play it (which sample, pitch, volume, envelope). Sound effects (`SBlk`) call these grains, music (`SBv2`) has programs and tones.
- the **samples**: SPU ADPCM, one after the other.
- for music, the **sequence**: the MIDI data of the song.

### Unpacking
```tools/bank_unpacker <path to output> <path to .SBK or .MUS files...> [--wav]```

For every bank `NAME.EXT` it creates a folder `NAME.EXT` with the parts of the bank, and a `NAME.EXT.json` description:
- `descriptor.bin`, `sequence.bin` (music only) and `prefix.bin` (whatever is before the container, like the name table of Jak 1 `.SBK` files) are copies of the data in the file.
- `samples/NNNN.adpcm` is every sample in the bank. With `--wav` a `samples/NNNN.wav` is written too, so you can listen to them (mono, 48000 Hz).

A file that is not a bank (like `TWEAKVAL.MUS`) is skipped.

### Repacking
```tools/bank_packer <path to the folder with the output of the unpacker> <description files...>```

For every description it writes `mod_NAME.EXT` in the folder. Nothing changed gives a file that is identical to the original.

### Replacing a sample
In the description, set the `file` of a sample to your own. It can be:
- a `.adpcm`, used as is (a multiple of 16 bytes).
- a `.wav` (16-bit PCM). It is turned into mono and converted to 48000 Hz if it is not, then encoded. Add `"loop": true` to the sample in the description to make the whole sample repeat. Convert other formats first, for example `ffmpeg -i in.mp3 -c:a pcm_s16le out.wav`.

How high or low a sample sounds is set by the tones in the descriptor (the note that the sample is at), not by the sample, so a replacement should be at the same pitch as what it replaces if the sound is played with different notes.

If the new sample is not longer than the old one, it keeps the same space and nothing else changes. If it is longer, everything after it moves, and the offsets to the samples in the descriptor are updated to match. This finds every tone that is used by the games' sound effects and music, but it can't know about any other place that might point to samples, so keep replacements the same size or smaller when you can.

### What is not supported
- Adding or removing samples, or changing the sounds, tones and envelopes in the descriptor. The descriptor is copied as it is (editing `descriptor.bin` by hand does work, unless you change where the samples are).
- Changing the sequence of a song other than replacing `sequence.bin` with another one.
- `.SBK` files in other layouts than the ones of the retail games (the chunks have to be next to each other with only padding after them).

### Format notes
- The container is a `u32` type (1 or 3), a `u32` number of chunks, and an (offset, size) for each chunk. It starts at a multiple of 2048 bytes in the file. This is what `Loader::BankLoad` in `game/sound/989snd/loader.cpp` reads.
- A sample is 16 byte blocks (a filter/shift byte, a flag byte, 28 samples). The last block of a sample has flag 1 (or 3 if it loops) and is followed by a block with flag 7, as in the VAG files. The tones point to the first block of a sample, or the one after it.
- The shared code lives in `common/audio/sound_bank.h`.
