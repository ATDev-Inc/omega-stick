// WebSocket protocol pieces: SHA-1, base64, the accept key, the handshake and Origin
// policy, and the frame codec.

#include <array>
#include <string>
#include <vector>

#include "test_harness.hpp"
#include "ws_protocol.hpp"

using namespace sim::ws;

namespace {

std::span<const uint8_t> bytes_of(const std::string &s) {
  return {reinterpret_cast<const uint8_t *>(s.data()), s.size()};
}

std::string sha1_hex(const std::string &s) { return to_hex(sha1(bytes_of(s))); }

std::vector<uint8_t> masked(Op op, const std::string &payload, bool fin = true) {
  const uint8_t key[4] = {0x37, 0xfa, 0x21, 0x3d};
  return encode_frame(op, bytes_of(payload), fin, key);
}

} // namespace

// ---- SHA-1 and base64 (RFC 3174, RFC 4648) ------------------------------------

TEST(sha1_matches_the_rfc_3174_vectors) {
  CHECK_EQ(sha1_hex("abc"), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
  CHECK_EQ(sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
           std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
  CHECK_EQ(sha1_hex(std::string(1000000, 'a')),
           std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
  std::string ten;
  for (int i = 0; i < 10; i++) {
    ten += "0123456701234567012345670123456701234567012345670123456701234567";
  }
  CHECK_EQ(sha1_hex(ten), std::string("dea356a2cddd90c7a7ecedc5ebb563934f460452"));
  CHECK_EQ(sha1_hex(""), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
}

TEST(base64_matches_the_rfc_4648_vectors) {
  CHECK_EQ(base64_encode(std::string_view("")), std::string(""));
  CHECK_EQ(base64_encode(std::string_view("f")), std::string("Zg=="));
  CHECK_EQ(base64_encode(std::string_view("fo")), std::string("Zm8="));
  CHECK_EQ(base64_encode(std::string_view("foo")), std::string("Zm9v"));
  CHECK_EQ(base64_encode(std::string_view("foob")), std::string("Zm9vYg=="));
  CHECK_EQ(base64_encode(std::string_view("fooba")), std::string("Zm9vYmE="));
  CHECK_EQ(base64_encode(std::string_view("foobar")), std::string("Zm9vYmFy"));
}

TEST(accept_key_matches_the_rfc_6455_example) {
  CHECK_EQ(accept_key("dGhlIHNhbXBsZSBub25jZQ=="), std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

// ---- handshake and Origin ------------------------------------------------------

namespace {
std::string request(const std::string &extra_headers, const std::string &version = "13") {
  return "GET / HTTP/1.1\r\nHost: 127.0.0.1:8765\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: " +
         version + "\r\n" + extra_headers + "\r\n";
}
} // namespace

TEST(handshake_accepts_a_valid_upgrade_without_an_origin) {
  const Handshake hs = parse_handshake(request(""));
  CHECK_EQ(hs.status, 101);
  CHECK_EQ(hs.accept, std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

TEST(handshake_header_names_and_values_are_case_insensitive) {
  const std::string req =
      "GET /x HTTP/1.1\r\nhost: h\r\nUPGRADE: WebSocket\r\ncOnNeCtIoN: keep-alive, "
      "Upgrade\r\nsec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "SEC-WEBSOCKET-VERSION: 13\r\n\r\n";
  CHECK_EQ(parse_handshake(req).status, 101);
}

TEST(handshake_origin_policy) {
  for (const char *ok :
       {"null", "http://localhost", "http://localhost:8000", "http://127.0.0.1:5500",
        "http://127.0.0.1", "https://atdev-inc.github.io"}) {
    CHECK_EQ(parse_handshake(request(std::string("Origin: ") + ok + "\r\n")).status, 101);
  }
  for (const char *bad :
       {"https://evil.example", "http://localhost.evil.example", "http://localhost:80evil",
        "http://127.0.0.1.evil.example", "https://atdev-inc.github.io.evil.example",
        "http://atdev-inc.github.io", "https://localhost:8000", ""}) {
    CHECK_EQ(parse_handshake(request(std::string("Origin: ") + bad + "\r\n")).status, 403);
  }
}

TEST(handshake_rejects_malformed_requests) {
  CHECK_EQ(parse_handshake("POST / HTTP/1.1\r\n\r\n").status, 405);
  CHECK_EQ(parse_handshake("GET / HTTP/1.1\r\nHost: h\r\n\r\n").status, 400);
  CHECK_EQ(parse_handshake(request("", "8")).status, 426);
  const std::string no_key = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Version: 13\r\n\r\n";
  CHECK_EQ(parse_handshake(no_key).status, 400);
}

// ---- frames ----------------------------------------------------------------------

TEST(server_frames_are_unmasked_with_the_right_length_forms) {
  const auto small = encode_frame(Op::Text, std::string_view("Hello"));
  CHECK(small == (std::vector<uint8_t>{0x81, 0x05, 'H', 'e', 'l', 'l', 'o'}));

  const std::string mid(300, 'x');
  const auto f126 = encode_frame(Op::Binary, bytes_of(mid));
  CHECK_EQ(f126[0], uint8_t{0x82});
  CHECK_EQ(f126[1], uint8_t{126});
  CHECK_EQ(f126[2], uint8_t{1});
  CHECK_EQ(f126[3], uint8_t{44});
  CHECK_EQ(f126.size(), size_t{4 + 300});

  const std::string big(70000, 'y');
  const auto f127 = encode_frame(Op::Binary, bytes_of(big));
  CHECK_EQ(f127[1], uint8_t{127});
  CHECK_EQ(f127.size(), size_t{10 + 70000});
  CHECK_EQ(f127[7], uint8_t{1});
  CHECK_EQ(f127[8], uint8_t{0x11});
  CHECK_EQ(f127[9], uint8_t{0x70});
}

TEST(decoder_reads_the_rfc_masked_hello) {
  const std::vector<uint8_t> wire = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                     0x7f, 0x9f, 0x4d, 0x51, 0x58};
  Decoder d;
  std::vector<Message> out;
  CHECK(d.feed(wire, out) == Decoder::Status::Ok);
  CHECK_EQ(out.size(), size_t{1});
  CHECK(out[0].op == Op::Text);
  CHECK_EQ(std::string(out[0].data.begin(), out[0].data.end()), std::string("Hello"));
}

TEST(decoder_handles_split_input_and_pipelined_frames) {
  std::vector<uint8_t> wire = masked(Op::Text, "one");
  const auto second = masked(Op::Binary, std::string(200, 'z'));
  wire.insert(wire.end(), second.begin(), second.end());
  Decoder d;
  std::vector<Message> out;
  for (const uint8_t b : wire) { // one byte at a time
    CHECK(d.feed(std::span<const uint8_t>(&b, 1), out) == Decoder::Status::Ok);
  }
  CHECK_EQ(out.size(), size_t{2});
  CHECK(out[0].op == Op::Text);
  CHECK(out[1].op == Op::Binary);
  CHECK_EQ(out[1].data.size(), size_t{200});

  std::vector<Message> both;
  Decoder d2;
  CHECK(d2.feed(wire, both) == Decoder::Status::Ok);
  CHECK_EQ(both.size(), size_t{2});
}

TEST(decoder_reassembles_fragments_around_control_frames) {
  std::vector<uint8_t> wire = masked(Op::Text, "Hel", false);
  const auto ping = masked(Op::Ping, "p");
  const auto rest = masked(Op::Continuation, "lo", true);
  wire.insert(wire.end(), ping.begin(), ping.end());
  wire.insert(wire.end(), rest.begin(), rest.end());
  Decoder d;
  std::vector<Message> out;
  CHECK(d.feed(wire, out) == Decoder::Status::Ok);
  CHECK_EQ(out.size(), size_t{2});
  CHECK(out[0].op == Op::Ping); // delivered between the fragments
  CHECK(out[1].op == Op::Text);
  CHECK_EQ(std::string(out[1].data.begin(), out[1].data.end()), std::string("Hello"));
}

TEST(decoder_rejects_protocol_violations) {
  auto errors = [](const std::vector<uint8_t> &wire) {
    Decoder d;
    std::vector<Message> out;
    return d.feed(wire, out) == Decoder::Status::ProtocolError;
  };
  // Unmasked client frame.
  CHECK(errors(encode_frame(Op::Text, std::string_view("hi"))));
  // Reserved bit set.
  auto rsv = masked(Op::Text, "hi");
  rsv[0] |= 0x40;
  CHECK(errors(rsv));
  // Unknown opcode.
  auto unknown = masked(Op::Text, "hi");
  unknown[0] = 0x83;
  CHECK(errors(unknown));
  // Continuation with nothing to continue.
  CHECK(errors(masked(Op::Continuation, "x")));
  // Fragmented control frame.
  CHECK(errors(masked(Op::Ping, "x", false)));
  // Oversized control frame.
  CHECK(errors(masked(Op::Ping, std::string(126, 'x'))));
  // A new data frame in the middle of a fragmented message.
  auto wire = masked(Op::Text, "a", false);
  const auto again = masked(Op::Text, "b");
  wire.insert(wire.end(), again.begin(), again.end());
  CHECK(errors(wire));
  // After an error the decoder stays failed.
  Decoder d;
  std::vector<Message> out;
  CHECK(d.feed(masked(Op::Continuation, "x"), out) == Decoder::Status::ProtocolError);
  CHECK(d.feed(masked(Op::Text, "fine"), out) == Decoder::Status::ProtocolError);
  CHECK_EQ(d.close_code(), uint16_t{1002});
}

TEST(decoder_enforces_the_one_mebibyte_message_limit) {
  // A header that announces more than the limit is refused before the payload arrives.
  const uint8_t header[] = {0x82, 0xFF, 0, 0, 0, 0, 0, 0x10, 0x00, 0x01, 1, 2, 3, 4};
  Decoder d;
  std::vector<Message> out;
  CHECK(d.feed(header, out) == Decoder::Status::TooBig);
  CHECK_EQ(d.close_code(), uint16_t{1009});

  // Exactly the limit is fine; fragments that add up past it are not.
  Decoder ok;
  std::vector<Message> got;
  const auto at_limit = masked(Op::Binary, std::string(kMaxMessageSize, 'a'));
  CHECK(ok.feed(at_limit, got) == Decoder::Status::Ok);
  CHECK_EQ(got.size(), size_t{1});
  CHECK_EQ(got[0].data.size(), kMaxMessageSize);

  Decoder frag;
  std::vector<Message> none;
  auto first = masked(Op::Binary, std::string(kMaxMessageSize, 'a'), false);
  CHECK(frag.feed(first, none) == Decoder::Status::Ok);
  CHECK(frag.feed(masked(Op::Continuation, "b"), none) == Decoder::Status::TooBig);
}

TEST(encode_masked_roundtrips_through_the_decoder) {
  for (const size_t n :
       {size_t{0}, size_t{1}, size_t{125}, size_t{126}, size_t{65535}, size_t{65536}}) {
    const std::string payload(n, 'q');
    Decoder d;
    std::vector<Message> out;
    CHECK(d.feed(masked(Op::Binary, payload), out) == Decoder::Status::Ok);
    CHECK_EQ(out.size(), size_t{1});
    CHECK_EQ(out[0].data.size(), n);
  }
}
