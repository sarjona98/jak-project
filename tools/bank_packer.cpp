#include <algorithm>
#include <cstdio>

#include "common/audio/audio_formats.h"
#include "common/audio/sound_bank.h"
#include "common/util/FileUtil.h"
#include "common/util/unicode_util.h"
#include "common/versions/versions.h"

#include "fmt/format.h"
#include "third-party/json.hpp"

namespace {

void print_usage() {
  printf(
      "usage: bank_packer <path to folder with the extracted files> <bank description files...>\n"
      "  the description files are the .json files made by bank_unpacker.\n"
      "  every sample in it can be a .adpcm (used as is) or a .wav (encoded by this tool)\n");
}

std::string lower_extension(const fs::path& p) {
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return tolower(c); });
  return ext;
}

/*!
 * A sample that was removed leaves a hole that must not be taken for the start of the next
 * sample, so unused space is filled with the block that goes after a sample.
 */
void pad_with_trap_blocks(std::vector<u8>& sample, size_t size) {
  while (sample.size() + 16 <= size) {
    sample.push_back(0x0c);
    sample.push_back(7);
    sample.resize(sample.size() + 14, 0);
  }
}

bool pack_bank(const fs::path& base_path, const fs::path& description_path) {
  nlohmann::json description;
  std::string name;
  try {
    description = nlohmann::json::parse(file_util::read_text_file(description_path));
    name = description.at("file_name").get<std::string>();
  } catch (std::exception& e) {
    printf("%s: invalid description: %s\n", description_path.string().c_str(), e.what());
    return false;
  }

  printf("Packing %s\n", name.c_str());
  sound_bank::Bank bank;
  std::vector<sound_bank::SampleExtent> old_extents, new_extents;
  try {
    auto read = [&](const nlohmann::json& j) {
      return file_util::read_binary_file(base_path / j.get<std::string>());
    };
    bank.container_type = description.at("container_type").get<u32>();
    if (!description.at("prefix").is_null()) {
      bank.prefix = read(description["prefix"]);
    }
    bank.descriptor = read(description.at("descriptor"));
    if (!description.at("sequence").is_null()) {
      bank.sequence = read(description["sequence"]);
    }

    int replaced = 0;
    for (auto& s : description.at("samples")) {
      sound_bank::SampleExtent old_extent;
      old_extent.offset = s.at("offset").get<u32>();
      old_extent.size = s.at("size").get<u32>();
      old_extents.push_back(old_extent);

      const fs::path sample_path = base_path / s.at("file").get<std::string>();
      std::vector<u8> data;
      if (lower_extension(sample_path) == ".wav") {
        WaveData wave;
        std::string error;
        if (!read_wave_file(sample_path, &wave, &error)) {
          printf("%s: %s\n", sample_path.string().c_str(), error.c_str());
          return false;
        }
        if (wave.left_samples.empty()) {
          printf("%s: the wave file has no samples\n", sample_path.string().c_str());
          return false;
        }
        // samples are mono
        if (!wave.right_samples.empty()) {
          for (size_t i = 0; i < wave.left_samples.size(); i++) {
            wave.left_samples[i] = ((s32)wave.left_samples[i] + wave.right_samples[i]) / 2;
          }
        }
        // the SPU plays samples at a fixed rate unless the pitch is changed. Pitch is set by the
        // tones (and the notes) in the bank, so the wave has to be at that rate.
        if ((u32)wave.sample_rate != sound_bank::SAMPLE_RATE) {
          printf("  %s is %d Hz, converting to %d Hz\n", sample_path.filename().string().c_str(),
                 wave.sample_rate, sound_bank::SAMPLE_RATE);
          wave.left_samples =
              sound_bank::resample(wave.left_samples, wave.sample_rate, sound_bank::SAMPLE_RATE);
        }
        data = sound_bank::build_sample(wave.left_samples, s.value("loop", false));
        // If it is smaller it keeps the space of the old one, so nothing has to move.
        pad_with_trap_blocks(data, old_extent.size);
        replaced++;
      } else {
        data = file_util::read_binary_file(sample_path);
        if (data.empty() || data.size() % 16) {
          printf("%s: the size must be a multiple of 16 bytes\n", sample_path.string().c_str());
          return false;
        }
        if (data.size() != old_extent.size) {
          replaced++;
        }
      }

      sound_bank::SampleExtent new_extent;
      new_extent.offset = bank.samples.size();
      new_extent.size = data.size();
      new_extents.push_back(new_extent);
      bank.samples.insert(bank.samples.end(), data.begin(), data.end());
    }
    printf("  %d samples, %d replaced\n", (int)new_extents.size(), replaced);
  } catch (std::exception& e) {
    printf("%s: %s\n", description_path.string().c_str(), e.what());
    return false;
  }

  bool moved = false;
  for (size_t i = 0; i < old_extents.size(); i++) {
    moved |= old_extents[i].offset != new_extents[i].offset ||
             old_extents[i].size != new_extents[i].size;
  }
  if (moved) {
    std::string error;
    if (!sound_bank::relocate_samples(&bank, old_extents, new_extents, &error)) {
      printf("  can't move the samples: %s\n", error.c_str());
      return false;
    }
    printf(
        "  a sample changed size, so the ones after it moved and the offsets to them in the "
        "bank were updated\n");
  }

  auto out = sound_bank::write_bank(bank, description.value("file_size", 0));
  const fs::path out_path = base_path / ("mod_" + name);
  file_util::write_binary_file(out_path, out.data(), out.size());
  printf("  wrote %s (%d bytes)\n", out_path.string().c_str(), (int)out.size());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  ArgumentGuard u8_guard(argc, argv);

  printf("OpenGOAL version %d.%d\n", versions::GOAL_VERSION_MAJOR, versions::GOAL_VERSION_MINOR);
  printf("Sound Bank (.SBK / .MUS) Packing Tool\n");

  if (argc < 3) {
    print_usage();
    return 1;
  }

  const fs::path base_path = argv[1];
  bool ok = true;
  for (int i = 2; i < argc; i++) {
    ok &= pack_bank(base_path, argv[i]);
  }
  printf("Done\n");
  return ok ? 0 : 1;
}
