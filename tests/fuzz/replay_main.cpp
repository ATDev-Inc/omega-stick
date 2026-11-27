// Replays corpus files through LLVMFuzzerTestOneInput without libFuzzer, so
// the seeds run in ctest on every compiler. Usage: omega_frame_replay <dir>...
// Exits nonzero if a directory is missing or has no files.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern size_t omega_fuzz_reply_count; // frame_fuzz.cpp, replay build
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv) {
  namespace fs = std::filesystem;
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <corpus dir>...\n", argv[0]);
    return 2;
  }
  size_t count = 0;
  for (int i = 1; i < argc; ++i) {
    std::error_code ec;
    if (!fs::is_directory(argv[i], ec)) {
      std::fprintf(stderr, "not a directory: %s\n", argv[i]);
      return 1;
    }
    size_t in_dir = 0;
    for (const auto &entry : fs::directory_iterator(argv[i], ec)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      std::ifstream in(entry.path(), std::ios::binary);
      if (!in) {
        std::fprintf(stderr, "cannot open %s\n", entry.path().string().c_str());
        return 1;
      }
      const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
      // End of file sets eofbit only; badbit means a read error.
      if (in.bad()) {
        std::fprintf(stderr, "cannot read %s\n", entry.path().string().c_str());
        return 1;
      }
      const size_t before = omega_fuzz_reply_count;
      LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
      // A request seed is a valid frame for a real handler: it must be answered.
      const std::string name = entry.path().filename().string();
      if (name.rfind("request_", 0) == 0 && omega_fuzz_reply_count == before) {
        std::fprintf(stderr, "no reply to %s: seed is stale or the frame is not dispatched\n",
                     name.c_str());
        return 1;
      }
      ++in_dir;
    }
    if (in_dir == 0) {
      std::fprintf(stderr, "no files in %s\n", argv[i]);
      return 1;
    }
    count += in_dir;
  }
  std::printf("replayed %zu corpus files\n", count);
  return 0;
}
