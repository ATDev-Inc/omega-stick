#include "ws_server.hpp"

#include <cstring>

#include "ws_protocol.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static constexpr sock_t kInvalidSocket = INVALID_SOCKET;
static void close_socket(sock_t s) { closesocket(s); }
static void shutdown_socket(sock_t s) { shutdown(s, SD_BOTH); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using sock_t = int;
static constexpr sock_t kInvalidSocket = -1;
static void close_socket(sock_t s) { close(s); }
static void shutdown_socket(sock_t s) { shutdown(s, SHUT_RDWR); }
#endif

namespace sim::ws {

namespace {

struct SocketRuntime {
  SocketRuntime() {
#if defined(_WIN32)
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
  }
  ~SocketRuntime() {
#if defined(_WIN32)
    WSACleanup();
#endif
  }
};

void ensure_runtime() {
  static const SocketRuntime runtime;
  (void)runtime;
}

void set_timeouts(sock_t s, int recv_ms, int send_ms) {
#if defined(_WIN32)
  const DWORD r = static_cast<DWORD>(recv_ms), w = static_cast<DWORD>(send_ms);
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&r), sizeof(r));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&w), sizeof(w));
#else
  timeval r{recv_ms / 1000, (recv_ms % 1000) * 1000};
  timeval w{send_ms / 1000, (send_ms % 1000) * 1000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof(r));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &w, sizeof(w));
#endif
}

bool send_all(sock_t s, const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const int n = static_cast<int>(
        send(s, reinterpret_cast<const char *>(data + sent), static_cast<int>(len - sent), 0));
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

} // namespace

struct Server::Conn {
  sock_t sock{kInvalidSocket};
  std::mutex write_mu;
  std::atomic<bool> open{true};

  ~Conn() {
    if (sock != kInvalidSocket) {
      close_socket(sock);
    }
  }
  void shut() {
    if (open.exchange(false)) {
      shutdown_socket(sock);
    }
  }
};

Server::Server(Handlers handlers)
    : handlers_(std::move(handlers)) {
  ensure_runtime();
}

Server::~Server() { stop(); }

bool Server::start(uint16_t listen_port, std::string &error) {
  const sock_t s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == kInvalidSocket) {
    error = "socket() failed";
    return false;
  }
#if !defined(_WIN32)
  const int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(listen_port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
  if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(s, 4) != 0) {
    error = "cannot listen on 127.0.0.1:" + std::to_string(listen_port) +
            " (is another simulator running?)";
    close_socket(s);
    return false;
  }
  sockaddr_in bound{};
#if defined(_WIN32)
  int bound_len = sizeof(bound);
#else
  socklen_t bound_len = sizeof(bound);
#endif
  getsockname(s, reinterpret_cast<sockaddr *>(&bound), &bound_len);
  port_ = ntohs(bound.sin_port);
  listen_socket_ = static_cast<std::intptr_t>(s);
  stopping_ = false;
  accept_thread_ = std::thread([this] { accept_loop(); });
  return true;
}

void Server::stop() {
  if (stopping_.exchange(true)) {
    return;
  }
  if (listen_socket_ != -1) {
    const auto s = static_cast<sock_t>(listen_socket_);
    shutdown_socket(s);
    close_socket(s);
    listen_socket_ = -1;
  }
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  std::vector<std::thread> readers;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (current_) {
      current_->shut();
    }
    readers.swap(readers_);
  }
  for (auto &t : readers) {
    if (t.joinable()) {
      t.join();
    }
  }
}

std::shared_ptr<Server::Conn> Server::current() const {
  std::lock_guard<std::mutex> lock(mu_);
  return current_;
}

bool Server::connected() const {
  const auto c = current();
  return c && c->open.load();
}

bool Server::send_frame(const std::shared_ptr<Conn> &conn, const std::vector<uint8_t> &frame) {
  if (!conn || !conn->open.load()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(conn->write_mu);
  if (!send_all(conn->sock, frame.data(), frame.size())) {
    conn->shut();
    return false;
  }
  return true;
}

bool Server::send_text(std::string_view text) {
  return send_frame(current(), encode_frame(Op::Text, text));
}

bool Server::send_binary(std::span<const uint8_t> data) {
  return send_frame(current(), encode_frame(Op::Binary, data));
}

void Server::close_client(uint16_t code) {
  const auto conn = current();
  if (!conn) {
    return;
  }
  const uint8_t payload[2] = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code)};
  send_frame(conn, encode_frame(Op::Close, std::span<const uint8_t>(payload, 2)));
  conn->shut();
}

void Server::accept_loop() {
  while (!stopping_) {
    const sock_t ls = static_cast<sock_t>(listen_socket_);
    const sock_t client = accept(ls, nullptr, nullptr);
    if (client == kInvalidSocket) {
      if (stopping_) {
        break;
      }
      continue;
    }
    set_timeouts(client, 5000, 2000);

    // Read the HTTP request head.
    std::string head;
    char buf[1024];
    bool complete = false;
    while (head.size() < 16 * 1024) {
      const int n = static_cast<int>(recv(client, buf, sizeof(buf), 0));
      if (n <= 0) {
        break;
      }
      head.append(buf, static_cast<size_t>(n));
      if (head.find("\r\n\r\n") != std::string::npos) {
        complete = true;
        break;
      }
    }
    if (!complete) {
      close_socket(client);
      continue;
    }
    const size_t head_end = head.find("\r\n\r\n") + 4;
    const std::string leftover = head.substr(head_end);
    const Handshake hs = parse_handshake(std::string_view(head).substr(0, head_end));
    std::string reply;
    if (hs.status == 101) {
      reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
              "Upgrade\r\nSec-WebSocket-Accept: " +
              hs.accept + "\r\n\r\n";
    } else {
      reply = "HTTP/1.1 " + std::to_string(hs.status) + " " + hs.reason +
              "\r\nContent-Length: 0\r\nConnection: close\r\n";
      if (hs.status == 426) {
        reply += "Sec-WebSocket-Version: 13\r\n";
      }
      reply += "\r\n";
    }
    send_all(client, reinterpret_cast<const uint8_t *>(reply.data()), reply.size());
    if (hs.status != 101) {
      close_socket(client);
      continue;
    }

    // Reads wait for as long as the client stays quiet; sends still time out.
    set_timeouts(client, 0, 2000);
    auto conn = std::make_shared<Conn>();
    conn->sock = client;
    std::shared_ptr<Conn> old;
    {
      std::lock_guard<std::mutex> lock(mu_);
      old = current_;
      current_ = conn;
    }
    if (old) {
      old->shut(); // replaced: its reader sees EOF and ends
    }
    if (handlers_.on_connect) {
      handlers_.on_connect();
    }
    std::lock_guard<std::mutex> lock(mu_);
    readers_.emplace_back([this, conn, leftover] { serve(conn, leftover); });
  }
}

void Server::serve(std::shared_ptr<Conn> conn, const std::string &leftover) {
  Decoder decoder;
  std::vector<uint8_t> buf(8192);
  bool close_sent = false;
  size_t have = 0; // bytes of buf to decode this round: first the handshake leftovers
  if (!leftover.empty() && leftover.size() <= buf.size()) {
    std::memcpy(buf.data(), leftover.data(), leftover.size());
    have = leftover.size();
  }
  while (conn->open.load()) {
    if (have == 0) {
      const int n = static_cast<int>(
          recv(conn->sock, reinterpret_cast<char *>(buf.data()), static_cast<int>(buf.size()), 0));
      if (n <= 0) {
        break;
      }
      have = static_cast<size_t>(n);
    }
    std::vector<Message> messages;
    const auto status = decoder.feed(std::span<const uint8_t>(buf.data(), have), messages);
    have = 0;
    for (Message &m : messages) {
      switch (m.op) {
      case Op::Text:
        if (handlers_.on_text && current() == conn) {
          handlers_.on_text(std::string(m.data.begin(), m.data.end()));
        }
        break;
      case Op::Binary:
        if (handlers_.on_binary && current() == conn) {
          handlers_.on_binary(m.data);
        }
        break;
      case Op::Ping:
        send_frame(conn, encode_frame(Op::Pong, m.data));
        break;
      case Op::Close: {
        send_frame(conn, encode_frame(Op::Close, m.data));
        close_sent = true;
        conn->shut();
        break;
      }
      default:
        break;
      }
    }
    if (status != Decoder::Status::Ok) {
      if (!close_sent) {
        const uint16_t code = decoder.close_code();
        const uint8_t payload[2] = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code)};
        send_frame(conn, encode_frame(Op::Close, std::span<const uint8_t>(payload, 2)));
      }
      conn->shut();
      break;
    }
  }
  conn->shut();
  bool was_current = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (current_ == conn) {
      current_.reset();
      was_current = true;
    }
  }
  if (was_current && handlers_.on_disconnect) {
    handlers_.on_disconnect();
  }
}

} // namespace sim::ws
