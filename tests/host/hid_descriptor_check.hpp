#pragma once

// A small HID report-descriptor walker for the host tests. It reads the short
// items of a descriptor (HID 1.11 section 6.2.2), tracks the global state that
// decides how wide each Input field is, and reports:
//
//   - whether the whole descriptor parsed (no long items, no truncated item,
//     no End Collection without a Collection)
//   - how many bytes it consumed
//   - the total Input bits per Report ID
//   - whether Collection / End Collection are balanced
//   - whether every data (non-constant) Input has Logical Min <= Logical Max
//
// Item prefix byte: bSize = bits 0-1 (3 means 4 data bytes), bType = bits 2-3
// (0 Main, 1 Global, 2 Local), bTag = bits 4-7. The prefix 0xFE introduces a
// long item, which this walker rejects.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hid_check {

struct DescriptorInfo {
  bool parsed{false};      ///< every byte was consumed as a well-formed short item
  std::size_t consumed{0}; ///< bytes walked before finishing or failing
  std::string error;       ///< why parsing stopped, empty on success
  bool collections_balanced{false};
  int max_collection_depth{0};
  bool logical_ranges_ok{true}; ///< no data Input with Logical Min > Logical Max
  /// Sum of Report Size * Report Count over every Input item, per Report ID
  /// (0 when the descriptor never sets a Report ID). The Report ID byte itself
  /// is not counted: it is a prefix on the wire, not an Input field.
  std::map<int, std::size_t> input_bits;

  std::size_t bits_for(int report_id) const {
    const auto it = input_bits.find(report_id);
    return it == input_bits.end() ? 0 : it->second;
  }
};

namespace detail {

constexpr int TYPE_MAIN = 0;
constexpr int TYPE_GLOBAL = 1;

constexpr int MAIN_INPUT = 0x8;
constexpr int MAIN_COLLECTION = 0xA;
constexpr int MAIN_END_COLLECTION = 0xC;

constexpr int GLOBAL_LOGICAL_MIN = 0x1;
constexpr int GLOBAL_LOGICAL_MAX = 0x2;
constexpr int GLOBAL_REPORT_SIZE = 0x7;
constexpr int GLOBAL_REPORT_ID = 0x8;
constexpr int GLOBAL_REPORT_COUNT = 0x9;
constexpr int GLOBAL_PUSH = 0xA;
constexpr int GLOBAL_POP = 0xB;

/// The global items the walker follows. Usage Page is parsed and ignored: it
/// does not affect the bit count.
struct Globals {
  std::size_t report_size{0};
  std::size_t report_count{0};
  int report_id{0};
  std::int64_t logical_min{0};
  std::int64_t logical_max{0};
};

inline std::uint32_t unsigned_value(const std::uint8_t *p, int size) {
  std::uint32_t v = 0;
  for (int i = 0; i < size; ++i) {
    v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
  }
  return v;
}

inline std::int64_t signed_value(const std::uint8_t *p, int size) {
  const std::uint32_t u = unsigned_value(p, size);
  if (size == 0) {
    return 0;
  }
  const std::uint32_t sign_bit = 1u << (8 * size - 1);
  if (size < 4 && (u & sign_bit) != 0) {
    return static_cast<std::int64_t>(u) - (std::int64_t{1} << (8 * size));
  }
  if (size == 4) {
    return static_cast<std::int32_t>(u);
  }
  return static_cast<std::int64_t>(u);
}

} // namespace detail

inline DescriptorInfo parse_descriptor(const std::uint8_t *data, std::size_t size) {
  using namespace detail;
  DescriptorInfo info;
  Globals globals;
  std::vector<Globals> stack;
  int depth = 0;
  bool depth_never_negative = true;
  std::size_t pos = 0;

  const auto fail = [&](const std::string &why) {
    info.error = why;
    info.consumed = pos;
    info.collections_balanced = false;
    return info;
  };

  while (pos < size) {
    const std::uint8_t prefix = data[pos];
    if (prefix == 0xFE) {
      return fail("long item (0xFE) at byte " + std::to_string(pos));
    }
    const int size_code = prefix & 0x03;
    const int data_len = size_code == 3 ? 4 : size_code;
    const int type = (prefix >> 2) & 0x03;
    const int tag = (prefix >> 4) & 0x0F;
    if (pos + 1 + static_cast<std::size_t>(data_len) > size) {
      return fail("item at byte " + std::to_string(pos) + " runs past the end");
    }
    const std::uint8_t *payload = data + pos + 1;

    if (type == TYPE_GLOBAL) {
      switch (tag) {
      case GLOBAL_LOGICAL_MIN:
        globals.logical_min = signed_value(payload, data_len);
        break;
      case GLOBAL_LOGICAL_MAX:
        // HID 6.2.2.7: when Logical Min is non-negative the maximum is unsigned.
        globals.logical_max = globals.logical_min >= 0
                                  ? static_cast<std::int64_t>(unsigned_value(payload, data_len))
                                  : signed_value(payload, data_len);
        break;
      case GLOBAL_REPORT_SIZE:
        globals.report_size = unsigned_value(payload, data_len);
        break;
      case GLOBAL_REPORT_ID:
        globals.report_id = static_cast<int>(unsigned_value(payload, data_len));
        break;
      case GLOBAL_REPORT_COUNT:
        globals.report_count = unsigned_value(payload, data_len);
        break;
      case GLOBAL_PUSH:
        stack.push_back(globals);
        break;
      case GLOBAL_POP:
        if (stack.empty()) {
          return fail("Pop with an empty global stack at byte " + std::to_string(pos));
        }
        globals = stack.back();
        stack.pop_back();
        break;
      default:
        break; // Usage Page, Physical Min/Max, Unit, Unit Exponent: not needed
      }
    } else if (type == TYPE_MAIN) {
      switch (tag) {
      case MAIN_INPUT: {
        info.input_bits[globals.report_id] += globals.report_size * globals.report_count;
        const bool constant = data_len > 0 && (payload[0] & 0x01) != 0;
        if (!constant && globals.logical_min > globals.logical_max) {
          info.logical_ranges_ok = false;
        }
        break;
      }
      case MAIN_COLLECTION:
        ++depth;
        if (depth > info.max_collection_depth) {
          info.max_collection_depth = depth;
        }
        break;
      case MAIN_END_COLLECTION:
        if (depth == 0) {
          depth_never_negative = false;
          return fail("End Collection without a Collection at byte " + std::to_string(pos));
        }
        --depth;
        break;
      default:
        break; // Output and Feature items do not add Input bits
      }
    }
    // Local items (Usage, Usage Min/Max, ...) need no tracking here.

    pos += 1 + static_cast<std::size_t>(data_len);
  }

  info.consumed = pos;
  info.collections_balanced = depth_never_negative && depth == 0;
  info.parsed = true;
  return info;
}

/// Walks anything with data() and size(), e.g. an hid::rdf::descriptor.
template <typename Descriptor> DescriptorInfo parse_descriptor(const Descriptor &d) {
  return parse_descriptor(reinterpret_cast<const std::uint8_t *>(d.data()),
                          static_cast<std::size_t>(d.size()));
}

} // namespace hid_check
