#pragma once

// Host-test stand-in for espp/file_system. The real one mounts a LittleFS
// partition; this one points get_root_path() at a per-process directory
// under the system temp folder (overridable via set_root_path for tests).

#include <filesystem>
#include <string>
#include <system_error>

namespace espp {

class FileSystem {
public:
  static FileSystem &get() {
    static FileSystem instance;
    return instance;
  }

  std::filesystem::path get_root_path() const { return root_; }

  bool remove(const std::filesystem::path &path, std::error_code &ec) {
    return std::filesystem::remove(path, ec);
  }

  // ---- test-only (not part of the real espp API) ----
  void set_root_path(std::filesystem::path root) { root_ = std::move(root); }

private:
  FileSystem() {
    root_ = std::filesystem::temp_directory_path() / "omega_stick_host_tests";
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
  }

  std::filesystem::path root_;
};

} // namespace espp
