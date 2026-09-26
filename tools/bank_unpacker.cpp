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
      "usage: bank_unpacker <path to output> <path to .SBK or .MUS files...> [--wav]\n"
      "  --wav also writes a .wav for every sample, for listening (the .adpcm is what gets "
      "repacked)\n");
}

}  // namespace

int main(int argc, char** argv) {
  ArgumentGuard u8_guard(argc, argv);

  printf("OpenGOAL version %d.%d\n", versions::GOAL_VERSION_MAJOR, versions::GOAL_VERSION_MINOR);
  printf("Sound Bank (.SBK / .MUS) Unpacking Tool\n");

  std::vector<std::string> args;
  bool write_wavs = false;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--wav") {
      write_wavs = true;
    } else {
      args.push_back(argv[i]);
    }
  }
  if (args.size() < 2) {
    print_usage();
    return 1;
  }

  const fs::path out_path = args[0];
  file_util::create_dir_if_needed(out_path);

  int failures = 0;
  for (size_t i = 1; i < args.size(); i++) {
    const fs::path in_path = args[i];
    const std::string name = in_path.filename().string();

    std::string error;
    auto bank = sound_bank::parse_bank(file_util::read_binary_file(in_path), &error);
    if (!bank) {
      printf("Skipping %s: %s\n", name.c_str(), error.c_str());
      failures++;
      continue;
    }

    auto extents = sound_bank::find_samples(bank->samples);
    printf("Unpacking %s (%s, %d samples)\n", name.c_str(), bank->kind().c_str(),
           (int)extents.size());

    auto write_file = [&](const std::string& relative, const std::vector<u8>& data) {
      file_util::create_dir_if_needed_for_file(out_path / relative);
      file_util::write_binary_file(out_path / relative, data.data(), data.size());
      return relative;
    };

    nlohmann::json description;
    description["file_name"] = name;
    description["file_size"] = bank->file_size;
    description["container_type"] = bank->container_type;
    description["kind"] = bank->kind();
    description["prefix"] = nullptr;
    if (!bank->prefix.empty()) {
      description["prefix"] = write_file(name + "/prefix.bin", bank->prefix);
    }
    description["descriptor"] = write_file(name + "/descriptor.bin", bank->descriptor);
    description["sequence"] = nullptr;
    if (bank->sequence) {
      description["sequence"] = write_file(name + "/sequence.bin", *bank->sequence);
    }

    description["samples"] = nlohmann::json::array();
    for (size_t s = 0; s < extents.size(); s++) {
      const auto& e = extents[s];
      std::vector<u8> data(bank->samples.begin() + e.offset,
                           bank->samples.begin() + e.offset + e.size);
      nlohmann::json sample;
      sample["file"] = write_file(fmt::format("{}/samples/{:04d}.adpcm", name, s), data);
      sample["offset"] = e.offset;
      sample["size"] = e.size;
      description["samples"].push_back(sample);

      if (write_wavs) {
        auto decoded = sound_bank::decode_sample(data.data(), data.size());
        write_wave_file(decoded, {}, sound_bank::SAMPLE_RATE,
                        out_path / fmt::format("{}/samples/{:04d}.wav", name, s));
      }
    }

    file_util::write_text_file(out_path / (name + ".json"), description.dump(2));
  }

  printf("Done\n");
  return failures ? 1 : 0;
}
