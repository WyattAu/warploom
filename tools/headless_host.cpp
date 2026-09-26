//! @file headless_host.cpp
//! @brief Headless control host for CI: EditorSession + ControlServer with
//!        no GPU, no window. Exposes the exact protocol surface the
//!        viewport exposes (minus host-owned camera/sun/capture visuals),
//!        so tools/live_proof.py runs identically on a CI runner and on
//!        hardware.
//!
//! Usage: omnicpp_headless_host <socket-path>
//!
//! Deterministic and drive-by-wire: the sim only advances via `step`.

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "engine/core/control_server.hpp"
#include "engine/core/editor_session.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

//! Forwards every command to the embedded session; nothing is host-owned
//! here (headless = no camera, no sun, no capture scheduling).
class HeadlessHost final : public omnicpp::core::ControlHost {
 public:
  [[nodiscard]] omnicpp::core::ControlReply on_control(
      const omnicpp::core::ControlCommand& command) override {
    return editor_.on_control(command);
  }
  [[nodiscard]] std::string snapshot_json() const override {
    return editor_.snapshot_json();
  }
  omnicpp::editor::EditorSession editor_{};
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <socket-path>\n", argv[0]);
    return 2;
  }
  const std::string socket_path = argv[1];

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  omnicpp::core::ControlServer server;
  std::string error;
  if (!server.start(socket_path, error)) {
    std::fprintf(stderr, "headless_host: %s\n", error.c_str());
    return 1;
  }
  std::fprintf(stderr, "headless_host: control server on %s\n",
               socket_path.c_str());

  HeadlessHost host;
  while (g_stop == 0) {
    (void)server.poll(host);  // non-blocking; 1ms sleep keeps CPU idle
    struct timespec ts{0, 1000000};  // 1ms
    ::nanosleep(&ts, nullptr);
  }
  server.stop();
  return 0;
}
