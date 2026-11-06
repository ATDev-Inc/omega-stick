#pragma once

// The transport-independent half of a minimal RFC 6455 WebSocket server: SHA-1,
// base64, the handshake (accept key, request validation, Origin policy) and the
// frame codec. No sockets here, so all of it is unit-tested directly.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sim::ws {

// ---- SHA-1 (RFC 3174) -------------------------------------------------------
// Used only for the handshake's Sec-WebSocket-Accept; not for anything security
// sensitive.

inline std::array<uint8_t, 20> sha1(std::span<const uint8_t> data) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };

  std::vector<uint8_t> msg(data.begin(), data.end());
  const uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8u;
  msg.push_back(0x80);
  while (msg.size() % 64 != 56) {
    msg.push_back(0);
  }
  for (int i = 7; i >= 0; i--) {
    msg.push_back(static_cast<uint8_t>(bit_len >> (i * 8)));
  }

  for (size_t off = 0; off < msg.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
      const size_t j = off + static_cast<size_t>(i) * 4;
      w[i] = (static_cast<uint32_t>(msg[j]) << 24) | (static_cast<uint32_t>(msg[j + 1]) << 16) |
             (static_cast<uint32_t>(msg[j + 2]) << 8) | static_cast<uint32_t>(msg[j + 3]);
    }
    for (int i = 16; i < 80; i++) {
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t temp = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  std::array<uint8_t, 20> out{};
  for (int i = 0; i < 5; i++) {
    out[static_cast<size_t>(i) * 4 + 0] = static_cast<uint8_t>(h[i] >> 24);
    out[static_cast<size_t>(i) * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
    out[static_cast<size_t>(i) * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
    out[static_cast<size_t>(i) * 4 + 3] = static_cast<uint8_t>(h[i]);
  }
  return out;
}

inline std::string to_hex(std::span<const uint8_t> bytes) {
  static const char *digits = "0123456789abcdef";
  std::string s;
  for (const uint8_t b : bytes) {
    s.push_back(digits[b >> 4]);
    s.push_back(digits[b & 15]);
  }
  return s;
}

// ---- base64 -----------------------------------------------------------------

inline std::string base64_encode(std::span<const uint8_t> data) {
  static const char *table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
    out.push_back(table[(v >> 18) & 63]);
    out.push_back(table[(v >> 12) & 63]);
    out.push_back(table[(v >> 6) & 63]);
    out.push_back(table[v & 63]);
  }
  if (i + 1 == data.size()) {
    const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    out.push_back(table[(v >> 18) & 63]);
    out.push_back(table[(v >> 12) & 63]);
    out += "==";
  } else if (i + 2 == data.size()) {
    const uint32_t v =
        (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
    out.push_back(table[(v >> 18) & 63]);
    out.push_back(table[(v >> 12) & 63]);
    out.push_back(table[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

inline std::string base64_encode(std::string_view text) {
  return base64_encode(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}

/// Sec-WebSocket-Accept for a Sec-WebSocket-Key (RFC 6455 section 4.2.2).
inline std::string accept_key(std::string_view client_key) {
  const std::string joined = std::string(client_key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const auto digest = sha1(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(joined.data()), joined.size()));
  return base64_encode(digest);
}

// ---- handshake --------------------------------------------------------------

/// The Origin policy: the calibration console opened from a file (Origin "null",
/// or "file://" when Chrome runs with --allow-file-access-from-files),
/// from a local web server, or from its published page; a request without an
/// Origin (a script, not a browser) is allowed too. Any other web page is refused
/// so a site you happen to visit cannot drive the simulator.
inline bool origin_allowed(const std::optional<std::string> &origin) {
  if (!origin) {
    return true;
  }
  const std::string &o = *origin;
  if (o == "null" || o == "file://" || o == "https://atdev-inc.github.io") {
    return true;
  }
  for (const std::string_view prefix : {"http://localhost", "http://127.0.0.1"}) {
    if (o.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    const std::string_view rest = std::string_view(o).substr(prefix.size());
    if (rest.empty()) {
      return true;
    }
    if (rest.size() >= 2 && rest[0] == ':') {
      bool digits = true;
      for (const char c : rest.substr(1)) {
        digits = digits && c >= '0' && c <= '9';
      }
      if (digits) {
        return true;
      }
    }
  }
  return false;
}

struct Handshake {
  int status{101}; ///< 101 accepted, else the HTTP status to answer with
  std::string reason;
  std::string accept; ///< Sec-WebSocket-Accept, when accepted
};

namespace detail {

inline std::string lower(std::string s) {
  for (char &c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

inline std::string trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return std::string(s);
}

} // namespace detail

/// Validates an HTTP upgrade request (everything up to and including the blank
/// line) and works out the answer.
inline Handshake parse_handshake(std::string_view request) {
  Handshake hs;
  auto reject = [&hs](int status, const char *reason) {
    hs.status = status;
    hs.reason = reason;
    return hs;
  };

  size_t pos = request.find('\n');
  if (pos == std::string_view::npos) {
    return reject(400, "Bad Request");
  }
  const std::string request_line = detail::trim(request.substr(0, pos));
  if (request_line.compare(0, 4, "GET ") != 0) {
    return reject(405, "Method Not Allowed");
  }

  std::optional<std::string> key, origin, version;
  bool upgrade_ws = false, connection_upgrade = false;
  size_t line_start = pos + 1;
  while (line_start < request.size()) {
    size_t line_end = request.find('\n', line_start);
    if (line_end == std::string_view::npos) {
      line_end = request.size();
    }
    const std::string_view line = request.substr(line_start, line_end - line_start);
    line_start = line_end + 1;
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    const std::string name = detail::lower(detail::trim(line.substr(0, colon)));
    const std::string value = detail::trim(line.substr(colon + 1));
    if (name == "upgrade") {
      upgrade_ws = detail::lower(value) == "websocket";
    } else if (name == "connection") {
      connection_upgrade = detail::lower(value).find("upgrade") != std::string::npos;
    } else if (name == "sec-websocket-key") {
      key = value;
    } else if (name == "sec-websocket-version") {
      version = value;
    } else if (name == "origin") {
      origin = value;
    }
  }

  if (!origin_allowed(origin)) {
    return reject(403, "Forbidden");
  }
  if (!upgrade_ws || !connection_upgrade || !key || key->empty()) {
    return reject(400, "Bad Request");
  }
  if (!version || *version != "13") {
    return reject(426, "Upgrade Required");
  }
  hs.accept = accept_key(*key);
  return hs;
}

// ---- frames -----------------------------------------------------------------

enum class Op : uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

/// Largest message (after reassembling fragments) the server accepts.
inline constexpr size_t kMaxMessageSize = 1024 * 1024;

/// One frame as the server sends it (never masked).
inline std::vector<uint8_t> encode_frame(Op op, std::span<const uint8_t> payload, bool fin = true,
                                         const uint8_t *mask_key = nullptr) {
  std::vector<uint8_t> out;
  out.push_back(static_cast<uint8_t>((fin ? 0x80 : 0x00) | static_cast<uint8_t>(op)));
  const uint8_t mask_bit = mask_key ? 0x80 : 0x00;
  if (payload.size() < 126) {
    out.push_back(static_cast<uint8_t>(mask_bit | payload.size()));
  } else if (payload.size() <= 0xFFFF) {
    out.push_back(static_cast<uint8_t>(mask_bit | 126));
    out.push_back(static_cast<uint8_t>(payload.size() >> 8));
    out.push_back(static_cast<uint8_t>(payload.size()));
  } else {
    out.push_back(static_cast<uint8_t>(mask_bit | 127));
    for (int i = 7; i >= 0; i--) {
      out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(payload.size()) >> (i * 8)));
    }
  }
  if (mask_key) {
    out.insert(out.end(), mask_key, mask_key + 4);
  }
  for (size_t i = 0; i < payload.size(); i++) {
    out.push_back(mask_key ? static_cast<uint8_t>(payload[i] ^ mask_key[i % 4]) : payload[i]);
  }
  return out;
}

inline std::vector<uint8_t> encode_frame(Op op, std::string_view text) {
  return encode_frame(
      op, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}

struct Message {
  Op op{Op::Text};
  std::vector<uint8_t> data;
};

/// Incremental decoder for client-to-server frames. Control frames are returned
/// as they arrive, even between the fragments of a data message. After an error
/// the decoder refuses further input; `close_code()` says what to answer with.
class Decoder {
public:
  enum class Status { Ok, ProtocolError, TooBig };

  Status feed(std::span<const uint8_t> bytes, std::vector<Message> &out) {
    if (status_ != Status::Ok) {
      return status_;
    }
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    size_t consumed = 0;
    while (true) {
      const size_t used = parse_one(consumed, out);
      if (status_ != Status::Ok) {
        return status_;
      }
      if (used == 0) {
        break;
      }
      consumed += used;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(consumed));
    return status_;
  }

  uint16_t close_code() const { return status_ == Status::TooBig ? 1009 : 1002; }

private:
  // Parses the frame starting at buf_[start]; returns the bytes it used, or 0 when
  // more input is needed (or on error, with status_ set).
  size_t parse_one(size_t start, std::vector<Message> &out) {
    const size_t avail = buf_.size() - start;
    if (avail < 2) {
      return 0;
    }
    const uint8_t b0 = buf_[start], b1 = buf_[start + 1];
    const bool fin = (b0 & 0x80) != 0;
    const uint8_t opcode = b0 & 0x0F;
    const bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7F;
    size_t header = 2;
    if ((b0 & 0x70) != 0 || !masked) { // reserved bits set, or an unmasked client frame
      status_ = Status::ProtocolError;
      return 0;
    }
    const bool control = (opcode & 0x08) != 0;
    const bool known = opcode <= 2 || (opcode >= 8 && opcode <= 10);
    if (!known || (control && (!fin || len > 125))) {
      status_ = Status::ProtocolError;
      return 0;
    }
    if (len == 126) {
      if (avail < 4) {
        return 0;
      }
      len = (static_cast<uint64_t>(buf_[start + 2]) << 8) | buf_[start + 3];
      header = 4;
    } else if (len == 127) {
      if (avail < 10) {
        return 0;
      }
      len = 0;
      for (int i = 0; i < 8; i++) {
        len = (len << 8) | buf_[start + 2 + static_cast<size_t>(i)];
      }
      header = 10;
    }
    if (len > kMaxMessageSize || msg_.size() + len > kMaxMessageSize) {
      status_ = Status::TooBig;
      return 0;
    }
    if (avail < header + 4 + len) {
      return 0;
    }
    uint8_t key[4];
    std::memcpy(key, &buf_[start + header], 4);
    std::vector<uint8_t> payload(static_cast<size_t>(len));
    for (size_t i = 0; i < payload.size(); i++) {
      payload[i] = static_cast<uint8_t>(buf_[start + header + 4 + i] ^ key[i % 4]);
    }
    const size_t used = header + 4 + static_cast<size_t>(len);

    if (control) {
      out.push_back({static_cast<Op>(opcode), std::move(payload)});
      return used;
    }
    if (opcode == 0) { // continuation
      if (!in_message_) {
        status_ = Status::ProtocolError;
        return 0;
      }
      msg_.insert(msg_.end(), payload.begin(), payload.end());
    } else {
      if (in_message_) { // a new data frame while a fragmented message is open
        status_ = Status::ProtocolError;
        return 0;
      }
      msg_ = std::move(payload);
      msg_op_ = static_cast<Op>(opcode);
      in_message_ = true;
    }
    if (fin) {
      Message done{msg_op_, {}};
      done.data.swap(msg_);
      out.push_back(std::move(done));
      in_message_ = false;
    }
    return used;
  }

  std::vector<uint8_t> buf_;
  std::vector<uint8_t> msg_;
  Op msg_op_{Op::Text};
  bool in_message_{false};
  Status status_{Status::Ok};
};

} // namespace sim::ws
