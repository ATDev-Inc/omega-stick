#pragma once

// A WebSocket server for exactly one client at a time, bound to 127.0.0.1.
// A new connection replaces the old one. The protocol work (handshake, frame
// codec) is in ws_protocol.hpp; this file is the sockets and threads around it.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sim::ws {

struct Handlers {
  /// A client finished the handshake and is now the current connection.
  std::function<void()> on_connect;
  /// A text message arrived from the current client.
  std::function<void(const std::string &text)> on_text;
  /// A binary message arrived from the current client.
  std::function<void(std::span<const uint8_t> data)> on_binary;
  /// The current client went away (closed, error, or replaced by a new one).
  std::function<void()> on_disconnect;
};

class Server {
public:
  explicit Server(Handlers handlers);
  ~Server();
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;

  /// Binds 127.0.0.1:port and starts accepting. Returns false (with `error`) on failure.
  /// With port 0 the OS picks a port; port() tells which.
  bool start(uint16_t listen_port, std::string &error);
  void stop();
  uint16_t port() const { return port_; }
  /// Reader threads still held (running, or finished but not yet reaped). Finished ones
  /// are joined when the next client connects, so this stays small however many clients
  /// come and go. For tests.
  size_t reader_count() const;

  bool connected() const;
  /// Sends to the current client; false when there is none or the send failed.
  bool send_text(std::string_view text);
  bool send_binary(std::span<const uint8_t> data);
  /// Sends a close frame and drops the current client.
  void close_client(uint16_t code = 1001);

private:
  struct Conn;
  struct Reader {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done; // set by serve() as its last act
  };
  void accept_loop();
  void serve(std::shared_ptr<Conn> conn, const std::string &leftover);
  /// Joins reader threads that have finished. Joins happen outside mu_.
  void reap_readers();
  bool send_frame(const std::shared_ptr<Conn> &conn, const std::vector<uint8_t> &frame);
  std::shared_ptr<Conn> current() const;

  Handlers handlers_;
  std::intptr_t listen_socket_{-1};
  uint16_t port_{0};
  std::atomic<bool> stopping_{false};
  std::thread accept_thread_;
  mutable std::mutex mu_;
  std::shared_ptr<Conn> current_;
  std::vector<Reader> readers_;
};

} // namespace sim::ws
