#pragma once

/*!
 * @file sound_bank.h
 * Reading and writing the sound bank files: .SBK (sound effects, "SBlk") and .MUS (music, "SBv2").
 * Both use the same container (this is the format that the 989snd loader reads):
 *   - optionally something before the container (Jak 1 SBK files have a name table), the container
 *     starts at a multiple of 2048 bytes.
 *   - u32 type (1 or 3), u32 chunk count, then an (offset, size) pair for each chunk.
 *   - chunk 0: the bank descriptor. Sounds, grains, programs and tones. Tones point to the samples.
 *   - chunk 1: the samples. SPU ADPCM, one after the other. Each ends with a block with the end
 * flag.
 *   - chunk 2 (music only): the sequence (MIDI) data.
 *   - zero padding to the end of the file.
 */

#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace sound_bank {

// The SPU plays a sample at this rate when the pitch is not changed.
constexpr u32 SAMPLE_RATE = 48000;

struct Bank {
  std::vector<u8> prefix;                   // bytes before the container
  u32 container_type = 0;                   // 1 or 3
  std::vector<u8> descriptor;               // chunk 0
  std::vector<u8> samples;                  // chunk 1
  std::optional<std::vector<u8>> sequence;  // chunk 2
  size_t file_size = 0;                     // of the original file, including padding

  // "SBlk" for sound effects, "SBv2" for music
  std::string kind() const;
};

std::optional<Bank> parse_bank(const std::vector<u8>& file, std::string* error);

/*!
 * The result is padded with zeros to at least min_file_size (the size of the original file).
 */
std::vector<u8> write_bank(const Bank& bank, size_t min_file_size);

/*!
 * A sample is stored as blocks of 16 bytes ending with a block with the end flag (followed by a
 * block that keeps the SPU from running off the end). This is where each one is.
 */
struct SampleExtent {
  u32 offset = 0;
  u32 size = 0;
};
std::vector<SampleExtent> find_samples(const std::vector<u8>& samples);

/*!
 * Find all the places in the descriptor that hold an offset into the samples. Returns the position
 * of each (a u32) in the descriptor. Returns nullopt if the descriptor is not understood.
 */
std::optional<std::vector<u32>> find_sample_references(const std::vector<u8>& descriptor);

/*!
 * Updates the offsets in the descriptor (and the size of the sample data) after the samples have
 * been rearranged. The extents must be the same samples, in the same order. Every offset that
 * points to the start of a sample (or one block after it) moves with that sample.
 */
bool relocate_samples(Bank* bank,
                      const std::vector<SampleExtent>& old_extents,
                      const std::vector<SampleExtent>& new_extents,
                      std::string* error);

/*!
 * Create a sample from audio. If loop is set, the whole sample repeats.
 */
std::vector<u8> build_sample(const std::vector<s16>& samples, bool loop);

/*!
 * Decode a sample (as found in the samples chunk, including its blocks after the end flag).
 */
std::vector<s16> decode_sample(const u8* data, size_t size);

/*!
 * Convert audio from one rate to another (linear interpolation).
 */
std::vector<s16> resample(const std::vector<s16>& in, u32 from_rate, u32 to_rate);

}  // namespace sound_bank
