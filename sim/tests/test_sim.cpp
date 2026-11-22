// Script validation, stick patterns, the sensor model through the real TMAG5273
// driver, and the two-slot flash fake.

#include <cmath>
#include <filesystem>
#include <string>

#include "json.hpp"
#include "script.hpp"
#include "sensor.hpp"
#include "sim_flash.hpp"
#include "stick.hpp"
#include "test_harness.hpp"
#include "tmag5273.hpp"

using namespace sim;

namespace {

bool valid(const std::string &steps, std::string *why = nullptr) {
  Script s;
  std::string err;
  const bool ok = parse_script_text(R"({"version":1,"steps":[)" + steps + "]}", s, err);
  if (why) {
    *why = err;
  }
  return ok;
}

} // namespace

// ---- scripts -----------------------------------------------------------------------------

TEST(script_accepts_every_step_kind) {
  CHECK(valid(R"(
    {"at_ms":0,"pattern":"calibrate"},
    {"at_ms":10,"stick":[0.5,-0.5]},
    {"at_ms":20,"fault":"weak_magnet"},
    {"at_ms":30,"drift":0.02},
    {"at_ms":40,"noise":0.002},
    {"at_ms":50,"next_image":"crash_loop"},
    {"at_ms":60,"action":"set_mode","value":"xac"},
    {"at_ms":65,"action":"set_nudge_enabled","value":false},
    {"at_ms":67,"action":"finish_calibration","expect_reply":"error"},
    {"at_ms":70,"action":"flash","version":"1.2.3"},
    {"at_ms":75,"action":"fw_info","expect_reply":"unsupported"},
    {"at_ms":80,"expect":{"calibrated":true,"center_max":0.05,"mouse_moved":"right","mode":"xac"}}
  )"));
}

TEST(script_enforces_the_documented_limits) {
  CHECK(!valid(R"({"at_ms":-1,"stick":[0,0]})"));
  CHECK(!valid(R"({"at_ms":86400001,"stick":[0,0]})"));
  CHECK(valid(R"({"at_ms":86400000,"stick":[0,0]})"));
  CHECK(!valid(R"({"at_ms":5,"stick":[0,0]},{"at_ms":4,"stick":[0,0]})"));
  CHECK(!valid(R"({"at_ms":0,"stick":[1.01,0]})")); // rejected, not clamped
  CHECK(!valid(R"({"at_ms":0,"stick":[0,-1.5]})"));
  CHECK(valid(R"({"at_ms":0,"stick":[1,-1]})"));
  CHECK(!valid(R"({"at_ms":0,"drift":10.5})"));
  CHECK(valid(R"({"at_ms":0,"drift":10})"));
  CHECK(!valid(R"({"at_ms":0,"drift":-1})"));
  CHECK(!valid(R"({"at_ms":0,"noise":1.5})"));
  CHECK(valid(R"({"at_ms":0,"noise":1})"));
}

TEST(script_limits_step_count_and_size) {
  std::string many;
  for (int i = 0; i < 5000; i++) {
    many += std::string(i ? "," : "") + R"({"at_ms":0,"stick":[0,0]})";
  }
  CHECK(valid(many));
  CHECK(!valid(many + R"(,{"at_ms":0,"stick":[0,0]})"));

  Script s;
  std::string err;
  const std::string big =
      R"({"version":1,"steps":[],"name":")" + std::string(300 * 1024, 'x') + "\"}";
  CHECK(!parse_script_text(big, s, err));
}

TEST(script_deadzone_limits_apply_unless_the_step_expects_invalid) {
  CHECK(valid(R"({"at_ms":0,"action":"set_deadzone","value":{"center":0.5,"range":0.1}})"));
  CHECK(!valid(R"({"at_ms":0,"action":"set_deadzone","value":{"center":0.9}})"));
  CHECK(!valid(R"({"at_ms":0,"action":"set_deadzone","value":{"range":0.2}})"));
  CHECK(!valid(R"({"at_ms":0,"action":"set_deadzone","value":{"center":-0.1}})"));
  // On purpose: the firmware's own check is what the scenario tests.
  CHECK(valid(
      R"({"at_ms":0,"action":"set_deadzone","value":{"center":0.9},"expect_reply":"invalid"})"));
  // ... but only for "invalid".
  CHECK(
      !valid(R"({"at_ms":0,"action":"set_deadzone","value":{"center":0.9},"expect_reply":"ok"})"));
}

TEST(script_rejects_malformed_steps) {
  CHECK(!valid(R"({"stick":[0,0]})"));                        // no at_ms
  CHECK(!valid(R"({"at_ms":0})"));                            // no body
  CHECK(!valid(R"({"at_ms":0,"stick":[0,0],"fault":"ok"})")); // two bodies
  CHECK(!valid(R"({"at_ms":0,"pattern":"nope"})"));
  CHECK(!valid(R"({"at_ms":0,"fault":"melted"})"));
  CHECK(!valid(R"({"at_ms":0,"action":"explode"})"));
  CHECK(!valid(R"({"at_ms":0,"action":"set_mode","value":"gamepad"})"));
  CHECK(!valid(R"({"at_ms":0,"action":"set_nudge_enabled"})"));
  CHECK(!valid(R"({"at_ms":0,"action":"set_nudge_enabled","value":"off"})"));
  CHECK(!valid(R"({"at_ms":0,"action":"set_nudge_enabled","value":0})"));
  CHECK(!valid(R"({"at_ms":0,"action":"recalibrate","expect_reply":"fine"})"));
  CHECK(!valid(R"({"at_ms":0,"expect":{"unknown_key":true}})"));
  CHECK(!valid(R"({"at_ms":0,"expect":{"mouse_moved":"sideways"}})"));
  CHECK(!valid(R"({"at_ms":0,"stick":[0]})"));
}

// ---- patterns and the stick source ---------------------------------------------------------

TEST(patterns_have_the_documented_shapes) {
  const StickPos rest = pattern_position("rest", 1234, -1);
  CHECK_NEAR(rest.x, 0, 0);
  CHECK_NEAR(rest.y, 0, 0);
  // circle: full deflection all the way around, about 8 s per turn
  for (double t = 0; t < 8000; t += 250) {
    const StickPos p = pattern_position("circle", t, -1);
    CHECK_NEAR(std::hypot(p.x, p.y), 1.0, 1e-9);
  }
  CHECK_NEAR(pattern_position("circle", 2000, -1).y, 1.0, 1e-9); // quarter turn: stick up
  // calibrate: released until the range prompt, then once around, then released
  CHECK_NEAR(std::hypot(pattern_position("calibrate", 5000, -1).x, 0), 0, 0);
  CHECK_NEAR(std::hypot(pattern_position("calibrate", 5000, 900).x,
                        pattern_position("calibrate", 5000, 900).y),
             1.0, 1e-9);
  CHECK_NEAR(std::hypot(pattern_position("calibrate", 5000, 7900).x,
                        pattern_position("calibrate", 5000, 7900).y),
             1.0, 1e-9);
  CHECK_NEAR(pattern_position("calibrate", 5000, 2000).y, 1.0, 1e-9); // quarter turn at 2 s
  CHECK_NEAR(std::hypot(pattern_position("calibrate", 5000, 8500).x, 0), 0, 0);
  // nudges: a push past the threshold and back
  CHECK_NEAR(pattern_position("nudge_right", 200, -1).x, 1.0, 0);
  CHECK_NEAR(pattern_position("nudge_left", 200, -1).x, -1.0, 0);
  CHECK_NEAR(pattern_position("nudge_up", 200, -1).y, 1.0, 0);
  CHECK_NEAR(pattern_position("nudge_down", 200, -1).y, -1.0, 0);
  CHECK_NEAR(pattern_position("nudge_right", 20, -1).x, 0, 0);
  CHECK_NEAR(pattern_position("nudge_right", 550, -1).x, 0, 0);
  // wander stays inside the unit circle
  for (double t = 0; t < 120000; t += 500) {
    const StickPos p = pattern_position("wander", t, -1);
    CHECK(std::hypot(p.x, p.y) <= 1.0 + 1e-9);
  }
}

TEST(manual_stick_input_is_clamped) {
  StickSource s;
  s.set_manual(3.0, -7.0);
  CHECK_NEAR(s.position(0).x, 1.0, 0);
  CHECK_NEAR(s.position(0).y, -1.0, 0);
}

TEST(scripts_interpolate_between_keyframes_and_hold_the_last_one) {
  Script script;
  std::string err;
  CHECK(parse_script_text(R"({"steps":[{"at_ms":0,"stick":[0,0]},{"at_ms":1000,"stick":[1,0]},
                            {"at_ms":2000,"stick":[1,0]},{"at_ms":3000,"pattern":"rest"}]})",
                          script, err));
  StickSource s;
  s.play_script(script, 100);
  CHECK_NEAR(s.position(100).x, 0, 1e-12);
  CHECK_NEAR(s.position(600).x, 0.5, 1e-12);
  CHECK_NEAR(s.position(1100).x, 1.0, 1e-12);
  CHECK_NEAR(s.position(2600).x, 1.0, 1e-12); // held until the next track event
  CHECK_NEAR(s.position(3200).x, 0.0, 1e-12); // the rest pattern
  CHECK(s.info(5000).done);
  s.stop();
  CHECK(s.info(5000).kind == StickSource::Kind::Manual);
}

// ---- the sensor, through the real driver -------------------------------------------------------

TEST(real_tmag_driver_reads_the_modelled_field) {
  SensorModel model;
  model.set_noise(0);
  StickPos pos{0.5, 0.25};
  FakeTmagBus bus(model, [&pos] { return std::make_pair(pos, int64_t{0}); });
  espp::TMAG5273 tmag(bus.config(0x35));
  std::error_code ec;
  CHECK(tmag.initialize(ec));

  const auto m = tmag.read_magnetic(ec);
  CHECK(!ec);
  CHECK_NEAR(m.x, SensorModel::kCenterXmT + 0.5 * SensorModel::mT_per_unit_x(), 0.05);
  // Physical up (+y) is the negative field direction so that it moves the cursor up.
  CHECK_NEAR(m.y, SensorModel::kCenterYmT - 0.25 * SensorModel::mT_per_unit_y(), 0.05);
  CHECK_NEAR(m.z, SensorModel::kZmT, 0.05);
}

TEST(conversion_counter_advances_unless_stuck) {
  SensorModel model;
  FakeTmagBus bus(model, [] { return std::make_pair(StickPos{}, int64_t{0}); });
  espp::TMAG5273 tmag(bus.config(0x35));
  std::error_code ec;
  CHECK(tmag.initialize(ec));
  const auto a = tmag.read_magnetic(ec);
  const auto b = tmag.read_magnetic(ec);
  CHECK(a.conversion_count != b.conversion_count);

  model.set_fault(Fault::StuckCounter);
  const auto c = tmag.read_magnetic(ec);
  const auto d = tmag.read_magnetic(ec);
  CHECK(!ec);
  CHECK_EQ(int(c.conversion_count), int(d.conversion_count));
  CHECK_NEAR(c.x, d.x, 0);
}

TEST(no_response_fails_every_transaction_and_recovers) {
  SensorModel model;
  FakeTmagBus bus(model, [] { return std::make_pair(StickPos{}, int64_t{0}); });
  espp::TMAG5273 tmag(bus.config(0x35));
  std::error_code ec;
  CHECK(tmag.initialize(ec));
  model.set_fault(Fault::NoResponse);
  tmag.read_magnetic(ec);
  CHECK(bool(ec));
  CHECK(!tmag.initialize(ec));
  model.set_fault(Fault::Ok);
  ec.clear();
  tmag.read_magnetic(ec);
  CHECK(!ec);
}

TEST(weak_magnet_shrinks_travel_but_not_the_resting_offset) {
  SensorModel model;
  model.set_noise(0);
  const Field rest = model.field({0, 0}, 0);
  const Field full = model.field({1, 0}, 0);
  model.set_fault(Fault::WeakMagnet);
  const Field weak_rest = model.field({0, 0}, 0);
  const Field weak_full = model.field({1, 0}, 0);
  CHECK_NEAR(weak_rest.bx, rest.bx, 0.05);
  CHECK((weak_full.bx - weak_rest.bx) < 0.2 * (full.bx - rest.bx));
}

TEST(drift_moves_the_true_center_over_time) {
  SensorModel model;
  const auto c0 = model.true_center_mT(0);
  model.set_drift(0.6, 0);
  const auto c1 = model.true_center_mT(60'000'000'000ll); // one minute
  CHECK(std::fabs(c1.first - c0.first) > 10.0);
  model.set_drift(0, 60'000'000'000ll); // stops drifting but keeps what it moved
  const auto c2 = model.true_center_mT(120'000'000'000ll);
  CHECK_NEAR(c2.first, c1.first, 1e-9);
}

// ---- flash
// ---------------------------------------------------------------------------------------

TEST(flash_parses_the_descriptor_of_an_uploaded_image) {
  const auto img = make_image("2.3.4-test", "omega_stick");
  esp_app_desc_t d{};
  CHECK(parse_image_desc(img, d));
  CHECK_EQ(std::string(d.version), std::string("2.3.4-test"));
  CHECK_EQ(std::string(d.project_name), std::string("omega_stick"));

  auto bad = img;
  bad[0] = 0x00;
  CHECK(!parse_image_desc(bad, d));
  bad = img;
  bad[kAppDescOffset] ^= 0xFF; // descriptor magic
  CHECK(!parse_image_desc(bad, d));
  CHECK(!parse_image_desc(std::span<const uint8_t>(img.data(), 100), d));
}

TEST(flash_two_slot_update_rollback_and_revert) {
  Flash f;
  f.set_native_version("0.1.0+sim");
  f.load_or_format(""); // in memory only
  CHECK_EQ(f.running_label(), std::string("ota_0"));
  CHECK(!f.pending_verify());
  esp_app_desc_t other{};
  CHECK(f.describe(f.next_update_partition(), &other) == ESP_ERR_NOT_FOUND); // ota_1 is empty
  CHECK(!f.mark_invalid_and_rollback());                                     // nowhere to go

  esp_app_desc_t desc{};
  CHECK(parse_image_desc(make_image("9.9.9"), desc));
  f.set_next_image(NextImage::FAIL_SELF_TEST);
  f.commit_image(desc);
  CHECK(f.next_image() == NextImage::OK); // consumed
  CHECK_EQ(f.take_restart_reason(), std::string("update"));

  CHECK(!f.boot());
  CHECK_EQ(f.running_label(), std::string("ota_1"));
  CHECK(f.pending_verify());
  CHECK(f.running_behavior() == NextImage::FAIL_SELF_TEST);

  // The health check gives up: invalid, boot the other slot.
  CHECK(f.mark_invalid_and_rollback());
  CHECK(!f.boot());
  CHECK_EQ(f.running_label(), std::string("ota_0"));
  esp_ota_img_states_t st{};
  CHECK(f.state_of(f.next_update_partition(), &st) == ESP_OK);
  CHECK(st == ESP_OTA_IMG_INVALID);

  // REVERT-style switch to the other slot resets its state to NEW, and it runs pending.
  CHECK(f.set_boot(f.next_update_partition()) == ESP_OK);
  CHECK_EQ(f.take_restart_reason(), std::string("revert"));
  CHECK(!f.boot());
  CHECK_EQ(f.running_label(), std::string("ota_1"));
  CHECK(f.pending_verify());
  f.mark_valid();
  CHECK(!f.pending_verify());
}

TEST(flash_bootloader_rolls_back_an_image_that_reboots_unconfirmed) {
  Flash f;
  f.load_or_format("");
  esp_app_desc_t desc{};
  CHECK(parse_image_desc(make_image("9.9.9"), desc));
  f.commit_image(desc);
  CHECK(!f.boot());
  CHECK(f.pending_verify());
  CHECK(f.boot()); // reset without confirming
  CHECK_EQ(f.running_label(), std::string("ota_0"));
  esp_ota_img_states_t st{};
  CHECK(f.state_of(f.next_update_partition(), &st) == ESP_OK);
  CHECK(st == ESP_OTA_IMG_ABORTED);
}

// Like the real bootloader: every slot still pending at boot was never confirmed,
// not only the boot slot (here: REVERT from a pending image).
TEST(flash_bootloader_aborts_every_pending_slot) {
  Flash f;
  f.load_or_format("");
  esp_app_desc_t desc{};
  CHECK(parse_image_desc(make_image("9.9.9"), desc));
  f.commit_image(desc);
  CHECK(!f.boot());
  CHECK(f.pending_verify());                              // ota_1 pending
  CHECK(f.set_boot(f.next_update_partition()) == ESP_OK); // REVERT to ota_0, unconfirmed
  CHECK(!f.boot());
  CHECK_EQ(f.running_label(), std::string("ota_0"));
  esp_ota_img_states_t st{};
  CHECK(f.state_of(f.next_update_partition(), &st) == ESP_OK);
  CHECK(st == ESP_OTA_IMG_ABORTED);
}

// esp_ota_check_rollback_is_possible(): no rollback to a slot without a complete image
// and a valid otadata entry.
TEST(flash_rollback_needs_a_bootable_other_slot) {
  Flash f;
  f.load_or_format("");
  esp_app_desc_t desc{};
  CHECK(parse_image_desc(make_image("9.9.9"), desc));
  f.commit_image(desc);
  CHECK(!f.boot());
  f.mark_valid(); // ota_1 confirmed
  // The next update lands in ota_0 and is rejected: back to ota_1 (VALID) works.
  f.commit_image(desc);
  CHECK(!f.boot());
  CHECK_EQ(f.running_label(), std::string("ota_0"));
  CHECK(f.mark_invalid_and_rollback());
  CHECK(!f.boot());
  CHECK_EQ(f.running_label(), std::string("ota_1"));
  // Now ota_0 is INVALID: no rollback to it.
  CHECK(!f.mark_invalid_and_rollback());
}

// esp_ota_begin() erases the other slot; the first bytes of an upload make it read as an
// image again, but one that does not verify until the upload is finished.
TEST(flash_an_unfinished_upload_leaves_a_partial_slot) {
  Flash f;
  f.load_or_format("");
  esp_app_desc_t desc{};
  CHECK(parse_image_desc(make_image("9.9.9"), desc));
  f.commit_image(desc);
  CHECK(!f.boot());
  f.mark_valid();
  CHECK_EQ(f.running_label(), std::string("ota_1"));
  const esp_partition_t *other = f.next_update_partition();
  const esp_partition_pos_t pos{.offset = other->address, .size = other->size};
  CHECK(f.verify(&pos) == ESP_OK); // ota_0 holds the native image

  f.begin_upload();
  esp_app_desc_t got{};
  CHECK(f.describe(other, &got) == ESP_ERR_NOT_FOUND); // erased
  esp_app_desc_t next{};
  CHECK(parse_image_desc(make_image("10.0.0"), next));
  f.upload_descriptor(next);
  CHECK(f.describe(other, &got) == ESP_OK);
  CHECK_EQ(std::string(got.version), std::string("10.0.0"));
  esp_ota_img_states_t st{};
  CHECK(f.state_of(other, &st) == ESP_ERR_NOT_FOUND); // otadata entry dropped
  CHECK(f.verify(&pos) == ESP_ERR_IMAGE_INVALID);
  CHECK(f.set_boot(other) == ESP_ERR_OTA_VALIDATE_FAILED); // REVERT fails
  CHECK(!f.mark_invalid_and_rollback());

  // Finishing the upload completes it.
  f.commit_image(next);
  CHECK(f.verify(&pos) == ESP_OK);
}

TEST(flash_saved_state_keeps_the_uploaded_descriptor_whole) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "omega_sim_flash_desc_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path file = dir / "flash.txt";

  esp_app_desc_t desc{};
  CHECK(parse_image_desc(make_image("9.9.9"), desc));
  std::snprintf(desc.idf_ver, sizeof(desc.idf_ver), "v5.4.1-test");
  desc.secure_version = 7;
  for (size_t i = 0; i < sizeof(desc.app_elf_sha256); i++) {
    desc.app_elf_sha256[i] = static_cast<uint8_t>(0xA0 + i);
  }
  {
    Flash f;
    f.load_or_format(file);
    f.commit_image(desc);
    CHECK(!f.boot());
    f.mark_valid(); // confirmed
  }

  Flash g; // a fresh process reading the same file
  g.load_or_format(file);
  CHECK(!g.boot());
  CHECK_EQ(g.running_label(), std::string("ota_1"));
  CHECK(!g.pending_verify());
  const esp_app_desc_t *got = g.running_desc();
  CHECK(got != nullptr);
  CHECK_EQ(std::string(got->version), std::string("9.9.9"));
  CHECK_EQ(std::string(got->project_name), std::string("omega_stick"));
  CHECK_EQ(std::string(got->idf_ver), std::string("v5.4.1-test"));
  CHECK_EQ(got->secure_version, uint32_t{7});
  CHECK(std::memcmp(got->app_elf_sha256, desc.app_elf_sha256, sizeof(desc.app_elf_sha256)) == 0);

  // A file from before the whole descriptor was saved still loads, with the old fallbacks.
  {
    std::ifstream in(file);
    std::string text, line;
    while (std::getline(in, line)) {
      if (line.find(".desc=") == std::string::npos) {
        text += line + "\n";
      }
    }
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << text;
  }
  Flash legacy;
  legacy.load_or_format(file);
  CHECK(!legacy.boot());
  CHECK_EQ(std::string(legacy.running_desc()->version), std::string("9.9.9"));
  CHECK_EQ(std::string(legacy.running_desc()->idf_ver), std::string("sim"));
  fs::remove_all(dir);
}
