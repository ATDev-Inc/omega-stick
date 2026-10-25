#pragma once

// A two-slot fake of the ESP-IDF OTA partition API, for the desktop simulator.
//
// What it keeps: for each of ota_0 / ota_1, whether it holds an app image, that
// image's esp_app_desc_t, and its otadata state (NEW, PENDING_VERIFY, VALID,
// INVALID, ABORTED); which slot runs and which boots next. The state is saved to
// a file in the simulator's data directory, so it survives a simulated restart
// (and a simulator restart).
//
// What it models of the bootloader (boot()): rollback is enabled, so a freshly
// activated image (NEW) runs as PENDING_VERIFY; every image that is still
// PENDING_VERIFY at the next boot was never confirmed and becomes ABORTED, and
// when that was the boot slot the device rolls back to the other slot if that
// slot holds a complete image.
//
// Of esp_ota_begin(): the slot that is not running is erased and loses its
// otadata state. Once the first bytes of an upload carry an app descriptor the
// slot reads as an image again, but a "partial" one that does not verify
// (esp_image_verify(), esp_ota_set_boot_partition()) until OTA_FINISH
// completes it. An aborted or expired upload leaves it that way.
//
// What it does not model: image signatures, flash contents (an uploaded image
// is kept only as its app descriptor), partition tables, secure boot.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// ---- ESP-IDF types (same layout and names as the real headers) ---------------

typedef struct {
  uint32_t magic_word;
  uint32_t secure_version;
  uint32_t reserv1[2];
  char version[32];
  char project_name[32];
  char time[16];
  char date[16];
  char idf_ver[32];
  uint8_t app_elf_sha256[32];
  uint16_t min_efuse_blk_rev_full;
  uint16_t max_efuse_blk_rev_full;
  uint8_t mmu_page_size;
  uint8_t reserv3[3];
  uint32_t reserv2[18];
} esp_app_desc_t;
static_assert(sizeof(esp_app_desc_t) == 256, "esp_app_desc_t must be 256 bytes");

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_IMAGE_INVALID 0x2002
#define ESP_ERR_OTA_VALIDATE_FAILED 0x1503

inline const char *esp_err_to_name(esp_err_t err) {
  switch (err) {
  case ESP_OK:
    return "ESP_OK";
  case ESP_FAIL:
    return "ESP_FAIL";
  case ESP_ERR_NOT_FOUND:
    return "ESP_ERR_NOT_FOUND";
  case ESP_ERR_IMAGE_INVALID:
    return "ESP_ERR_IMAGE_INVALID";
  case ESP_ERR_OTA_VALIDATE_FAILED:
    return "ESP_ERR_OTA_VALIDATE_FAILED";
  default:
    return "UNKNOWN ERROR";
  }
}

struct esp_partition_t {
  char label[17];
  uint32_t address;
  uint32_t size;
};

// ---- esp_image_format.h ----------------------------------------------------

typedef struct {
  uint32_t offset;
  uint32_t size;
} esp_partition_pos_t;

typedef enum {
  ESP_IMAGE_VERIFY,
  ESP_IMAGE_VERIFY_SILENT,
} esp_image_load_mode_t;

typedef struct {
  uint32_t start_addr;
  uint32_t image_len;
} esp_image_metadata_t;

typedef enum {
  ESP_OTA_IMG_NEW = 0,
  ESP_OTA_IMG_PENDING_VERIFY = 1,
  ESP_OTA_IMG_VALID = 2,
  ESP_OTA_IMG_INVALID = 3,
  ESP_OTA_IMG_ABORTED = 4,
  ESP_OTA_IMG_UNDEFINED = -1,
} esp_ota_img_states_t;

namespace sim {

/// How an image flashed through the console behaves once it has booted.
enum class NextImage {
  OK,
  FAIL_SELF_TEST,
  CRASH_LOOP,
  SLOW_SENSOR_START,
  SLOW_SENSOR_FAIL,
  /// Runs normally while pending, so the console can confirm it; once confirmed it crashes on
  /// every boot (a confirmed image that crash-loops, for the crash-loop handover).
  CRASH_WHEN_CONFIRMED,
};

inline const char *next_image_name(NextImage b) {
  switch (b) {
  case NextImage::FAIL_SELF_TEST:
    return "fail_self_test";
  case NextImage::CRASH_LOOP:
    return "crash_loop";
  case NextImage::SLOW_SENSOR_START:
    return "slow_sensor_start";
  case NextImage::SLOW_SENSOR_FAIL:
    return "slow_sensor_fail";
  case NextImage::CRASH_WHEN_CONFIRMED:
    return "crash_when_confirmed";
  case NextImage::OK:
  default:
    return "ok";
  }
}

inline bool parse_next_image(const std::string &name, NextImage &out) {
  for (const NextImage b : {NextImage::OK, NextImage::FAIL_SELF_TEST, NextImage::CRASH_LOOP,
                            NextImage::SLOW_SENSOR_START, NextImage::SLOW_SENSOR_FAIL,
                            NextImage::CRASH_WHEN_CONFIRMED}) {
    if (name == next_image_name(b)) {
      out = b;
      return true;
    }
  }
  return false;
}

constexpr uint32_t kAppDescMagic = 0xABCD5432u;
/// esp_image_header_t (24 bytes) + esp_image_segment_header_t (8 bytes).
constexpr size_t kAppDescOffset = 32;

inline void copy_str(char *dst, size_t cap, const std::string &s) {
  std::memset(dst, 0, cap);
  std::memcpy(dst, s.data(), s.size() < cap - 1 ? s.size() : cap - 1);
}

inline esp_app_desc_t make_desc(const std::string &version, const std::string &project) {
  esp_app_desc_t d{};
  d.magic_word = kAppDescMagic;
  copy_str(d.version, sizeof(d.version), version);
  copy_str(d.project_name, sizeof(d.project_name), project);
  copy_str(d.time, sizeof(d.time), "00:00:00");
  copy_str(d.date, sizeof(d.date), "Jan  1 2026");
  copy_str(d.idf_ver, sizeof(d.idf_ver), "sim");
  return d;
}

/// The descriptor's bytes as lowercase hex, for the saved-state file.
inline std::string desc_to_hex(const esp_app_desc_t &d) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  const auto *b = reinterpret_cast<const uint8_t *>(&d);
  for (size_t i = 0; i < sizeof(d); i++) {
    out += digits[b[i] >> 4];
    out += digits[b[i] & 0x0F];
  }
  return out;
}

/// Inverse of desc_to_hex(); false (leaving `d` alone) unless the text is exactly one
/// descriptor with the right magic.
inline bool desc_from_hex(const std::string &hex, esp_app_desc_t &d) {
  if (hex.size() != sizeof(d) * 2) {
    return false;
  }
  auto nibble = [](char c) {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  esp_app_desc_t out{};
  auto *b = reinterpret_cast<uint8_t *>(&out);
  // The size check above makes hex exactly two digits per descriptor byte.
  for (size_t j = 0; j + 1 < hex.size(); j += 2) {
    const int hi = nibble(hex[j]), lo = nibble(hex[j + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    b[j / 2] = static_cast<uint8_t>(hi << 4 | lo);
  }
  if (out.magic_word != kAppDescMagic) {
    return false;
  }
  out.version[sizeof(out.version) - 1] = '\0';
  out.project_name[sizeof(out.project_name) - 1] = '\0';
  out.time[sizeof(out.time) - 1] = '\0';
  out.date[sizeof(out.date) - 1] = '\0';
  out.idf_ver[sizeof(out.idf_ver) - 1] = '\0';
  d = out;
  return true;
}

/// Reads the app descriptor out of a firmware image (what esp_ota_end checks
/// for): image magic 0xE9, then esp_app_desc_t right after the first segment
/// header. Returns false for anything that is not an app image.
inline bool parse_image_desc(std::span<const uint8_t> image, esp_app_desc_t &desc) {
  if (image.size() < kAppDescOffset + sizeof(esp_app_desc_t) || image[0] != 0xE9) {
    return false;
  }
  std::memcpy(&desc, image.data() + kAppDescOffset, sizeof(desc));
  if (desc.magic_word != kAppDescMagic) {
    return false;
  }
  desc.version[sizeof(desc.version) - 1] = '\0';
  desc.project_name[sizeof(desc.project_name) - 1] = '\0';
  desc.time[sizeof(desc.time) - 1] = '\0';
  desc.date[sizeof(desc.date) - 1] = '\0';
  desc.idf_ver[sizeof(desc.idf_ver) - 1] = '\0';
  return true;
}

/// A minimal firmware image that parse_image_desc() accepts: used by the
/// scripts' `flash` action and by tests.
inline std::vector<uint8_t> make_image(const std::string &version,
                                       const std::string &project = "omega_stick",
                                       size_t size = 8192) {
  const size_t min_size = kAppDescOffset + sizeof(esp_app_desc_t) + 16;
  const size_t total = size < min_size ? min_size : size;
  std::vector<uint8_t> img(total, 0);
  img[0] = 0xE9; // image magic
  img[1] = 1;    // one segment
  const esp_app_desc_t desc = make_desc(version, project);
  std::memcpy(img.data() + kAppDescOffset, &desc, sizeof(desc));
  for (size_t i = kAppDescOffset + sizeof(desc); i < total; i++) {
    img[i] = static_cast<uint8_t>(i * 31u);
  }
  return img;
}

class Flash {
public:
  struct Slot {
    esp_partition_t part{};
    bool has_image{false};
    /// The image this simulator binary itself stands for (not an upload).
    bool native{false};
    /// The start of an upload that never finished: the descriptor reads, the image does
    /// not verify.
    bool partial{false};
    esp_app_desc_t desc{};
    esp_ota_img_states_t state{ESP_OTA_IMG_UNDEFINED};
    NextImage behavior{NextImage::OK};
  };

  Flash() {
    std::snprintf(slots_[0].part.label, sizeof(slots_[0].part.label), "ota_0");
    std::snprintf(slots_[1].part.label, sizeof(slots_[1].part.label), "ota_1");
    // partitions.csv: two 1920 KB app slots.
    slots_[0].part.address = 0x10000;
    slots_[1].part.address = 0x1F0000;
    slots_[0].part.size = slots_[1].part.size = 0x1E0000;
  }

  /// Version string of the image the simulator binary itself stands for.
  void set_native_version(const std::string &version) {
    std::lock_guard<std::mutex> lock(mu_);
    native_version_ = version;
  }

  /// Called when the (simulated) firmware asks for a restart from a thread that
  /// cannot unwind (Ota::restart() after the reply flush delay).
  void set_restart_hook(std::function<void(const char *reason)> hook) {
    std::lock_guard<std::mutex> lock(mu_);
    restart_hook_ = std::move(hook);
  }

  /// A brand-new board: the simulator's own image in ota_0, nothing in ota_1.
  void format() {
    std::lock_guard<std::mutex> lock(mu_);
    format_locked();
    save_locked();
  }

  /// Loads the saved state from `file`, or formats when there is none. An empty
  /// path keeps the state in memory only.
  void load_or_format(const std::filesystem::path &file) {
    std::lock_guard<std::mutex> lock(mu_);
    path_ = file;
    if (path_.empty() || !load_locked()) {
      format_locked();
    }
    refresh_native_locked();
    save_locked();
  }

  /// The bootloader: picks the slot to run from the boot slot and the image
  /// states. Returns true when it rolled an unconfirmed image back.
  bool boot() {
    std::lock_guard<std::mutex> lock(mu_);
    bool rolled_back = false;
    // As the real bootloader does, every image still pending was never confirmed, not
    // only the boot slot's (a REVERT from a pending image leaves the running one so).
    const bool boot_was_pending = slots_[boot_].state == ESP_OTA_IMG_PENDING_VERIFY;
    for (Slot &s : slots_) {
      if (s.state == ESP_OTA_IMG_PENDING_VERIFY) {
        s.state = ESP_OTA_IMG_ABORTED;
      }
    }
    if (boot_was_pending) {
      const int other = 1 - boot_;
      if (slots_[other].has_image && !slots_[other].partial) {
        boot_ = other;
        rolled_back = true;
      }
    }
    Slot &run = slots_[boot_];
    if (run.state == ESP_OTA_IMG_NEW) {
      run.state = ESP_OTA_IMG_PENDING_VERIFY;
    }
    running_ = boot_;
    save_locked();
    return rolled_back;
  }

  // ---- esp_ota_ops ----------------------------------------------------------

  const esp_partition_t *running_partition() const {
    std::lock_guard<std::mutex> lock(mu_);
    return &slots_[running_].part;
  }

  const esp_partition_t *next_update_partition() const {
    std::lock_guard<std::mutex> lock(mu_);
    return &slots_[1 - running_].part;
  }

  esp_err_t describe(const esp_partition_t *part, esp_app_desc_t *desc) const {
    std::lock_guard<std::mutex> lock(mu_);
    const Slot *s = slot_of(part);
    if (!s || !s->has_image) {
      return ESP_ERR_NOT_FOUND;
    }
    *desc = s->desc;
    return ESP_OK;
  }

  esp_err_t state_of(const esp_partition_t *part, esp_ota_img_states_t *state) const {
    std::lock_guard<std::mutex> lock(mu_);
    const Slot *s = slot_of(part);
    if (!s || !s->has_image || s->state == ESP_OTA_IMG_UNDEFINED) {
      return ESP_ERR_NOT_FOUND;
    }
    *state = s->state;
    return ESP_OK;
  }

  /// esp_ota_set_boot_partition: also resets the slot's state to NEW, so a slot
  /// that was once rejected can be tried again on purpose.
  esp_err_t set_boot(const esp_partition_t *part) {
    std::lock_guard<std::mutex> lock(mu_);
    const int idx = index_of(part);
    if (idx < 0 || !slots_[idx].has_image) {
      return ESP_ERR_NOT_FOUND;
    }
    if (slots_[idx].partial) {
      return ESP_ERR_OTA_VALIDATE_FAILED; // the real one verifies the image first
    }
    slots_[idx].state = ESP_OTA_IMG_NEW;
    boot_ = idx;
    if (boot_ != running_) {
      restart_reason_ = "revert";
    }
    save_locked();
    return ESP_OK;
  }

  // ---- espp::Ota backing ----------------------------------------------------

  std::string running_label() const {
    std::lock_guard<std::mutex> lock(mu_);
    return slots_[running_].part.label;
  }
  std::string boot_label() const {
    std::lock_guard<std::mutex> lock(mu_);
    return slots_[boot_].part.label;
  }
  int running_index() const {
    std::lock_guard<std::mutex> lock(mu_);
    return running_;
  }
  bool pending_verify() const {
    std::lock_guard<std::mutex> lock(mu_);
    return slots_[running_].state == ESP_OTA_IMG_PENDING_VERIFY;
  }
  NextImage running_behavior() const {
    std::lock_guard<std::mutex> lock(mu_);
    return slots_[running_].behavior;
  }
  const esp_app_desc_t *running_desc() const {
    std::lock_guard<std::mutex> lock(mu_);
    return &slots_[running_].desc;
  }

  void mark_valid() {
    std::lock_guard<std::mutex> lock(mu_);
    slots_[running_].state = ESP_OTA_IMG_VALID;
    save_locked();
  }

  /// esp_image_verify(): ESP_OK when the slot at `pos` holds a complete image.
  esp_err_t verify(const esp_partition_pos_t *pos) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const Slot &s : slots_) {
      if (pos && s.part.address == pos->offset) {
        return s.has_image && !s.partial ? ESP_OK : ESP_ERR_IMAGE_INVALID;
      }
    }
    return ESP_ERR_IMAGE_INVALID;
  }

  /// Marks the running image invalid and points the next boot at the other
  /// slot. False (nothing changed) when no rollback is possible, judged as
  /// esp_ota_check_rollback_is_possible() does: the other slot needs a valid
  /// otadata entry (not INVALID or ABORTED; UNDEFINED is no entry) and an
  /// image that verifies.
  bool mark_invalid_and_rollback() {
    std::lock_guard<std::mutex> lock(mu_);
    const int other = 1 - running_;
    const Slot &o = slots_[other];
    const bool entry_valid = o.state == ESP_OTA_IMG_NEW || o.state == ESP_OTA_IMG_PENDING_VERIFY ||
                             o.state == ESP_OTA_IMG_VALID;
    if (!o.has_image || o.partial || !entry_valid) {
      return false;
    }
    slots_[running_].state = ESP_OTA_IMG_INVALID;
    boot_ = other;
    restart_reason_ = "rollback";
    save_locked();
    return true;
  }

  /// esp_ota_begin(): erases the slot that is not running and drops its otadata
  /// entry. A boot slot that pointed there falls back to the running slot.
  void begin_upload() {
    std::lock_guard<std::mutex> lock(mu_);
    const int target = 1 - running_;
    Slot &s = slots_[target];
    s.has_image = false;
    s.native = false;
    s.partial = false;
    s.desc = {};
    s.state = ESP_OTA_IMG_UNDEFINED;
    s.behavior = NextImage::OK;
    if (boot_ == target) {
      boot_ = running_;
    }
    save_locked();
  }

  /// The upload's first bytes carried an app descriptor: the slot now reads as an image,
  /// but a partial one until commit_image().
  void upload_descriptor(const esp_app_desc_t &desc) {
    std::lock_guard<std::mutex> lock(mu_);
    Slot &s = slots_[1 - running_];
    s.has_image = true;
    s.partial = true;
    s.desc = desc;
    save_locked();
  }

  /// Stores a validated upload in the slot that is not running, makes it the
  /// boot slot and gives it the behaviour chosen with set_next_image().
  void commit_image(const esp_app_desc_t &desc) {
    std::lock_guard<std::mutex> lock(mu_);
    const int target = 1 - running_;
    Slot &s = slots_[target];
    s.has_image = true;
    s.native = false;
    s.partial = false;
    s.desc = desc;
    s.state = ESP_OTA_IMG_NEW;
    s.behavior = next_image_;
    next_image_ = NextImage::OK;
    boot_ = target;
    restart_reason_ = "update";
    save_locked();
  }

  void set_next_image(NextImage b) {
    std::lock_guard<std::mutex> lock(mu_);
    next_image_ = b;
    save_locked();
  }
  NextImage next_image() const {
    std::lock_guard<std::mutex> lock(mu_);
    return next_image_;
  }

  /// Why the restart that is about to happen is happening ("restart" unless an
  /// update, revert or rollback set it).
  std::string take_restart_reason() {
    std::lock_guard<std::mutex> lock(mu_);
    std::string r = restart_reason_.empty() ? "restart" : restart_reason_;
    restart_reason_.clear();
    return r;
  }

  /// espp::Ota::restart(): hands the restart to the host through the hook.
  void request_restart() {
    std::function<void(const char *)> hook;
    {
      std::lock_guard<std::mutex> lock(mu_);
      hook = restart_hook_;
    }
    const std::string reason = take_restart_reason();
    if (hook) {
      hook(reason.c_str());
    }
  }

private:
  int index_of(const esp_partition_t *part) const {
    for (int i = 0; i < 2; i++) {
      if (part == &slots_[i].part) {
        return i;
      }
    }
    return -1;
  }
  const Slot *slot_of(const esp_partition_t *part) const {
    const int i = index_of(part);
    return i < 0 ? nullptr : &slots_[i];
  }

  void refresh_native_locked() {
    for (Slot &s : slots_) {
      if (s.native && s.has_image) {
        s.desc = make_desc(native_version_, "omega_stick");
      }
    }
  }

  void format_locked() {
    for (Slot &s : slots_) {
      s.has_image = false;
      s.native = false;
      s.partial = false;
      s.desc = {};
      s.state = ESP_OTA_IMG_UNDEFINED;
      s.behavior = NextImage::OK;
    }
    slots_[0].has_image = true;
    slots_[0].native = true;
    slots_[0].desc = make_desc(native_version_, "omega_stick");
    slots_[0].state = ESP_OTA_IMG_VALID;
    running_ = boot_ = 0;
    next_image_ = NextImage::OK;
    restart_reason_.clear();
  }

  void save_locked() const {
    if (path_.empty()) {
      return;
    }
    std::ofstream out(path_, std::ios::trunc);
    if (!out) {
      return;
    }
    out << "running=" << running_ << "\nboot=" << boot_
        << "\nnext_image=" << next_image_name(next_image_) << "\n";
    for (int i = 0; i < 2; i++) {
      const Slot &s = slots_[i];
      const std::string p = "slot" + std::to_string(i) + ".";
      out << p << "has=" << (s.has_image ? 1 : 0) << "\n"
          << p << "native=" << (s.native ? 1 : 0) << "\n"
          << p << "partial=" << (s.partial ? 1 : 0) << "\n"
          << p << "state=" << static_cast<int>(s.state) << "\n"
          << p << "behavior=" << next_image_name(s.behavior) << "\n"
          << p << "version=" << s.desc.version << "\n"
          << p << "project=" << s.desc.project_name << "\n"
          << p << "date=" << s.desc.date << "\n"
          << p << "time=" << s.desc.time << "\n"
          << p << "desc=" << desc_to_hex(s.desc) << "\n";
    }
  }

  bool load_locked() {
    std::ifstream in(path_);
    if (!in) {
      return false;
    }
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
      const auto eq = line.find('=');
      if (eq != std::string::npos) {
        kv[line.substr(0, eq)] = line.substr(eq + 1);
      }
    }
    if (kv.find("running") == kv.end()) {
      return false;
    }
    auto geti = [&kv](const std::string &k, int def) {
      const auto it = kv.find(k);
      if (it == kv.end()) {
        return def;
      }
      try {
        return std::stoi(it->second);
      } catch (const std::exception &) {
        return def;
      }
    };
    running_ = geti("running", 0) == 1 ? 1 : 0;
    boot_ = geti("boot", running_) == 1 ? 1 : 0;
    if (kv.count("next_image") != 0) {
      parse_next_image(kv["next_image"], next_image_);
    }
    for (int i = 0; i < 2; i++) {
      const std::string p = "slot" + std::to_string(i) + ".";
      Slot &s = slots_[i];
      s.has_image = geti(p + "has", 0) != 0;
      s.native = geti(p + "native", 0) != 0;
      s.partial = geti(p + "partial", 0) != 0;
      s.state = static_cast<esp_ota_img_states_t>(geti(p + "state", -1));
      if (kv.count(p + "behavior") != 0) {
        parse_next_image(kv[p + "behavior"], s.behavior);
      }
      s.desc = make_desc(kv[p + "version"], kv[p + "project"]);
      if (kv.count(p + "date") != 0) {
        copy_str(s.desc.date, sizeof(s.desc.date), kv[p + "date"]);
      }
      if (kv.count(p + "time") != 0) {
        copy_str(s.desc.time, sizeof(s.desc.time), kv[p + "time"]);
      }
      // The whole descriptor (idf_ver, secure_version, app_elf_sha256, ...). Files from
      // before it was saved lack it and keep the fields above.
      esp_app_desc_t full{};
      if (kv.count(p + "desc") != 0 && desc_from_hex(kv[p + "desc"], full)) {
        s.desc = full;
      }
    }
    return true;
  }

  mutable std::mutex mu_;
  std::array<Slot, 2> slots_{};
  int running_{0};
  int boot_{0};
  NextImage next_image_{NextImage::OK};
  std::string native_version_{"0.0.0+sim"};
  std::string restart_reason_;
  std::filesystem::path path_;
  std::function<void(const char *)> restart_hook_;
};

/// The one simulated flash. Intentionally never destroyed: the 750 ms reboot
/// thread CalibrationService detaches may still be running at process exit.
inline Flash &flash() {
  static Flash *instance = new Flash();
  return *instance;
}

} // namespace sim

inline const esp_app_desc_t *esp_app_get_description(void) { return sim::flash().running_desc(); }

inline const esp_partition_t *esp_ota_get_running_partition(void) {
  return sim::flash().running_partition();
}

inline const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *) {
  return sim::flash().next_update_partition();
}

inline esp_err_t esp_ota_get_partition_description(const esp_partition_t *part,
                                                   esp_app_desc_t *desc) {
  return sim::flash().describe(part, desc);
}

inline esp_err_t esp_ota_get_state_partition(const esp_partition_t *part,
                                             esp_ota_img_states_t *state) {
  return sim::flash().state_of(part, state);
}

inline esp_err_t esp_ota_set_boot_partition(const esp_partition_t *part) {
  return sim::flash().set_boot(part);
}

inline esp_err_t esp_image_verify(esp_image_load_mode_t, const esp_partition_pos_t *part,
                                  esp_image_metadata_t *data) {
  const esp_err_t err = sim::flash().verify(part);
  if (err == ESP_OK && data) {
    data->start_addr = part->offset;
    data->image_len = part->size;
  }
  return err;
}
