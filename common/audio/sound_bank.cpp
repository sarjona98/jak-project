#include "sound_bank.h"

#include <algorithm>
#include <cstring>

#include "common/audio/audio_formats.h"
#include "common/util/BinaryReader.h"

#include "fmt/format.h"

namespace sound_bank {

namespace {

constexpr u32 CONTAINER_ALIGNMENT = 2048;
constexpr u32 BLOCK_SIZE = 16;

// grain types that have a tone with a sample
constexpr u32 GRAIN_TYPE_TONE = 1;
constexpr u32 GRAIN_TYPE_TONE2 = 9;

// size of a tone, the sample offset is at 16
constexpr u32 TONE_SIZE = 24;
constexpr u32 TONE_SAMPLE_OFFSET = 16;

constexpr u8 FLAG_END = 1;
constexpr u8 FLAG_LOOP_START = 4;
constexpr u8 FLAG_LOOP_END = 3;
constexpr u8 FLAG_TRAP = 7;

/*!
 * Reads from a buffer, remembering if anything was out of bounds.
 */
struct SafeReader {
  const std::vector<u8>& data;
  bool ok = true;

  explicit SafeReader(const std::vector<u8>& d) : data(d) {}

  u32 u32_at(size_t pos) {
    if (pos + 4 > data.size()) {
      ok = false;
      return 0;
    }
    u32 v;
    memcpy(&v, data.data() + pos, 4);
    return v;
  }
  s16 s16_at(size_t pos) {
    if (pos + 2 > data.size()) {
      ok = false;
      return 0;
    }
    s16 v;
    memcpy(&v, data.data() + pos, 2);
    return v;
  }
  s8 s8_at(size_t pos) {
    if (pos + 1 > data.size()) {
      ok = false;
      return 0;
    }
    return (s8)data[pos];
  }
};

u32 fourcc_at(const std::vector<u8>& d, size_t pos) {
  u32 v = 0;
  if (pos + 4 <= d.size()) {
    memcpy(&v, d.data() + pos, 4);
  }
  return v;
}

constexpr u32 make_fourcc(const char* s) {
  return (u8)s[0] | ((u8)s[1] << 8) | ((u8)s[2] << 16) | ((u32)(u8)s[3] << 24);
}

void put_u32(std::vector<u8>& out, u32 v) {
  for (int i = 0; i < 4; i++) {
    out.push_back((v >> (8 * i)) & 0xff);
  }
}

void set_u32(std::vector<u8>& d, size_t pos, u32 v) {
  memcpy(d.data() + pos, &v, 4);
}

std::vector<u8> make_block(u8 shift_filter, u8 flags) {
  std::vector<u8> result(BLOCK_SIZE, 0);
  result[0] = shift_filter;
  result[1] = flags;
  return result;
}

}  // namespace

std::string Bank::kind() const {
  return std::string((const char*)descriptor.data(), std::min<size_t>(4, descriptor.size()));
}

std::optional<Bank> parse_bank(const std::vector<u8>& file, std::string* error) {
  auto fail = [&](const std::string& msg) -> std::optional<Bank> {
    if (error) {
      *error = msg;
    }
    return std::nullopt;
  };

  // The container starts at the first multiple of 2048 where it makes sense.
  for (size_t off = 0; off + 16 <= file.size(); off += CONTAINER_ALIGNMENT) {
    SafeReader r(file);
    const u32 type = r.u32_at(off);
    const u32 num_chunks = r.u32_at(off + 4);
    if ((type != 1 && type != 3) || num_chunks < 2 || num_chunks > 3 ||
        off + 8 + (size_t)num_chunks * 8 > file.size()) {
      continue;
    }

    std::vector<std::pair<u32, u32>> chunks;
    bool in_bounds = true;
    for (u32 i = 0; i < num_chunks; i++) {
      u32 coff = r.u32_at(off + 8 + i * 8);
      u32 csz = r.u32_at(off + 12 + i * 8);
      if (off + (size_t)coff + csz > file.size()) {
        in_bounds = false;
        break;
      }
      chunks.push_back({coff, csz});
    }
    if (!in_bounds) {
      continue;
    }
    u32 magic = fourcc_at(file, off + chunks[0].first);
    if (magic != make_fourcc("SBlk") && magic != make_fourcc("SBv2")) {
      continue;
    }

    // The chunks have to be back to back so that they can be rebuilt.
    size_t expected = 8 + (size_t)num_chunks * 8;
    for (auto& [coff, csz] : chunks) {
      if (coff != expected) {
        return fail("the chunks in this file are not next to each other, this is not supported");
      }
      expected += csz;
    }
    size_t end = off + expected;
    for (size_t i = end; i < file.size(); i++) {
      if (file[i]) {
        return fail("there is data after the last chunk, this is not supported");
      }
    }

    Bank bank;
    bank.prefix.assign(file.begin(), file.begin() + off);
    bank.container_type = type;
    auto chunk_data = [&](int i) {
      auto begin = file.begin() + off + chunks[i].first;
      return std::vector<u8>(begin, begin + chunks[i].second);
    };
    bank.descriptor = chunk_data(0);
    bank.samples = chunk_data(1);
    if (num_chunks == 3) {
      bank.sequence = chunk_data(2);
    }
    bank.file_size = file.size();
    return bank;
  }
  return fail("not a sound bank (no SBlk or SBv2 container found)");
}

std::vector<u8> write_bank(const Bank& bank, size_t min_file_size) {
  std::vector<u8> out = bank.prefix;
  const u32 num_chunks = bank.sequence ? 3 : 2;

  put_u32(out, bank.container_type);
  put_u32(out, num_chunks);
  u32 offset = 8 + num_chunks * 8;
  std::vector<const std::vector<u8>*> chunks = {&bank.descriptor, &bank.samples};
  if (bank.sequence) {
    chunks.push_back(&*bank.sequence);
  }
  for (auto* c : chunks) {
    put_u32(out, offset);
    put_u32(out, c->size());
    offset += c->size();
  }
  for (auto* c : chunks) {
    out.insert(out.end(), c->begin(), c->end());
  }

  out.resize(std::max(out.size(), min_file_size), 0);
  return out;
}

std::vector<SampleExtent> find_samples(const std::vector<u8>& samples) {
  std::vector<SampleExtent> result;
  size_t p = 0;
  while (p + BLOCK_SIZE <= samples.size()) {
    const size_t start = p;
    int blocks = 0;
    while (p + BLOCK_SIZE <= samples.size()) {
      u8 flags = samples[p + 1];
      p += BLOCK_SIZE;
      blocks++;
      if (flags & FLAG_END) {
        break;
      }
    }

    // A single silent block with flag 7 after the end of a sample is just part of that sample.
    bool trap = blocks == 1 && samples[start + 1] == FLAG_TRAP;
    for (size_t i = start + 2; trap && i < start + BLOCK_SIZE; i++) {
      trap = samples[i] == 0;
    }
    if (trap && !result.empty()) {
      result.back().size = p - result.back().offset;
    } else {
      result.push_back({(u32)start, (u32)(p - start)});
    }
  }
  if (!result.empty()) {
    // don't lose anything if the data doesn't end on a block
    result.back().size = samples.size() - result.back().offset;
  }
  return result;
}

std::optional<std::vector<u32>> find_sample_references(const std::vector<u8>& descriptor) {
  SafeReader r(descriptor);
  const u32 magic = r.u32_at(0);
  const u32 version = r.u32_at(4);
  std::vector<u32> positions;

  if (magic == make_fourcc("SBlk")) {
    const s16 num_sounds = r.s16_at(22);
    const u32 first_sound = r.u32_at(28);
    const u32 first_grain = r.u32_at(32);
    const u32 grain_data = version >= 2 ? r.u32_at(52) : 0;
    if (!r.ok || num_sounds < 0) {
      return std::nullopt;
    }
    for (int i = 0; i < num_sounds; i++) {
      const size_t sound = first_sound + (size_t)i * 12;
      const s8 num_grains = r.s8_at(sound + 4);
      const u32 sound_first_grain = r.u32_at(sound + 8);
      for (int g = 0; g < num_grains && r.ok; g++) {
        if (version < 2) {
          // the tone is inside the grain
          const size_t grain = first_grain + sound_first_grain + (size_t)g * 0x28;
          u32 type = r.u32_at(grain);
          if (type == GRAIN_TYPE_TONE || type == GRAIN_TYPE_TONE2) {
            positions.push_back(grain + 8 + TONE_SAMPLE_OFFSET);
          }
        } else {
          // the grain has a pointer to the tone
          const size_t grain = first_grain + sound_first_grain + (size_t)g * 8;
          u32 op = r.u32_at(grain);
          u32 type = op >> 24;
          if (type == GRAIN_TYPE_TONE || type == GRAIN_TYPE_TONE2) {
            positions.push_back(grain_data + (op & 0xffffff) + TONE_SAMPLE_OFFSET);
          }
        }
      }
    }
  } else if (magic == make_fourcc("SBv2")) {
    const s16 num_progs = r.s16_at(22);
    const u32 first_prog = r.u32_at(32);
    if (!r.ok || num_progs < 0) {
      return std::nullopt;
    }
    for (int i = 0; i < num_progs; i++) {
      const size_t prog = first_prog + (size_t)i * 8;
      const s8 num_tones = r.s8_at(prog);
      const u32 first_tone = r.u32_at(prog + 4);
      for (int t = 0; t < num_tones; t++) {
        positions.push_back(first_tone + (size_t)t * TONE_SIZE + TONE_SAMPLE_OFFSET);
      }
    }
  } else {
    return std::nullopt;
  }

  if (!r.ok) {
    return std::nullopt;
  }
  std::sort(positions.begin(), positions.end());
  positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
  for (u32 p : positions) {
    if (p + 4 > descriptor.size()) {
      return std::nullopt;
    }
  }
  return positions;
}

bool relocate_samples(Bank* bank,
                      const std::vector<SampleExtent>& old_extents,
                      const std::vector<SampleExtent>& new_extents,
                      std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) {
      *error = msg;
    }
    return false;
  };
  if (old_extents.size() != new_extents.size() || old_extents.empty()) {
    return fail("the number of samples doesn't match");
  }

  auto refs = find_sample_references(bank->descriptor);
  if (!refs) {
    return fail("the bank descriptor is not understood, so samples can't be moved");
  }

  for (u32 pos : *refs) {
    u32 value;
    memcpy(&value, bank->descriptor.data() + pos, 4);
    auto it = std::upper_bound(old_extents.begin(), old_extents.end(), value,
                               [](u32 v, const SampleExtent& e) { return v < e.offset; });
    if (it == old_extents.begin()) {
      return fail(fmt::format("offset 0x{:x} at 0x{:x} is not in a sample", value, pos));
    }
    size_t idx = (it - old_extents.begin()) - 1;
    u32 delta = value - old_extents[idx].offset;
    if (delta >= old_extents[idx].size) {
      return fail(fmt::format("offset 0x{:x} at 0x{:x} is not in a sample", value, pos));
    }
    if (delta > new_extents[idx].size) {
      return fail(fmt::format("sample {} is too short, something points into it", idx));
    }
    set_u32(bank->descriptor, pos, new_extents[idx].offset + delta);
  }

  // the size of all the samples is also in the descriptor
  const u32 old_total = old_extents.back().offset + old_extents.back().size;
  const u32 new_total = new_extents.back().offset + new_extents.back().size;
  const size_t size_pos = bank->kind() == "SBlk" ? 40 : 44;
  if (size_pos + 4 <= bank->descriptor.size()) {
    u32 current;
    memcpy(&current, bank->descriptor.data() + size_pos, 4);
    if (current == old_total) {
      set_u32(bank->descriptor, size_pos, new_total);
    }
  }
  return true;
}

std::vector<u8> build_sample(const std::vector<s16>& samples, bool loop) {
  auto audio = encode_adpcm(samples);
  if (audio.empty()) {
    audio.resize(BLOCK_SIZE, 0);
  }

  // silent first block, the audio, then the block that keeps the SPU from going further.
  std::vector<u8> result = make_block(0, 0);
  if (loop) {
    audio[1] = FLAG_LOOP_START;
    audio[audio.size() - BLOCK_SIZE + 1] = FLAG_LOOP_END;
    if (audio.size() == BLOCK_SIZE) {
      audio[1] = FLAG_LOOP_END;
    }
  } else {
    audio[audio.size() - BLOCK_SIZE + 1] = FLAG_END;
  }
  result.insert(result.end(), audio.begin(), audio.end());
  auto trap = make_block(0x0c, FLAG_TRAP);
  result.insert(result.end(), trap.begin(), trap.end());
  return result;
}

std::vector<s16> decode_sample(const u8* data, size_t size) {
  // decode_adpcm expects a 48 byte header first.
  std::vector<u8> with_header(48 + size, 0);
  memcpy(with_header.data() + 48, data, size);
  BinaryReader reader(std::span<const u8>(with_header.data(), with_header.size()));
  return decode_adpcm(reader, false, 0).first;
}

std::vector<s16> resample(const std::vector<s16>& in, u32 from_rate, u32 to_rate) {
  if (from_rate == to_rate || in.empty()) {
    return in;
  }
  const size_t out_size = std::max<size_t>(1, (u64)in.size() * to_rate / from_rate);
  std::vector<s16> out(out_size);
  for (size_t i = 0; i < out_size; i++) {
    double pos = (double)i * from_rate / to_rate;
    size_t i0 = std::min<size_t>((size_t)pos, in.size() - 1);
    size_t i1 = std::min<size_t>(i0 + 1, in.size() - 1);
    double frac = pos - (double)i0;
    out[i] = (s16)std::clamp<double>(in[i0] * (1.0 - frac) + in[i1] * frac, -32768.0, 32767.0);
  }
  return out;
}

}  // namespace sound_bank
