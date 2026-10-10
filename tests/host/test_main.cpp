#include <filesystem>

#include "test_harness.hpp"

int main() {
  // Off ESP-IDF, espp::FileSystem::get_root_path() is the current working
  // directory. Run from a throwaway directory so the tests never write into
  // the build tree or the caller's cwd.
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path original = fs::current_path(ec);
  const fs::path scratch = fs::temp_directory_path(ec) / "omega_stick_host_tests";
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  fs::current_path(scratch, ec);

  const int rc = harness::run_all();

  fs::current_path(original, ec);
  fs::remove_all(scratch, ec);
  return rc;
}
