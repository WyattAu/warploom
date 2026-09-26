//! @file test_control_server.cpp
//! @brief Headless M0 protocol proofs against a fake ControlHost: welcome +
//!        snapshot on connect, every command round-trip, malformed/unknown
//!        input errors, multi-client isolation, and clean reconnect.

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "engine/core/control_server.hpp"

namespace {

using omnicpp::core::ControlCommand;
using omnicpp::core::ControlHost;
using omnicpp::core::ControlReply;
using omnicpp::core::ControlServer;

//! Deterministic fake host: pauses/steps/counts, records camera/sun/cube.
class FakeHost final : public ControlHost {
 public:
  ControlReply on_control(const ControlCommand& command) override {
    ControlReply reply;
    reply.ok = true;
    switch (command.kind) {
      case ControlCommand::Kind::Ping:
        reply.detail = "pong";
        break;
      case ControlCommand::Kind::Pause:
        paused_ = true;
        reply.detail = "paused";
        break;
      case ControlCommand::Kind::Resume:
        paused_ = false;
        reply.detail = "resumed";
        break;
      case ControlCommand::Kind::Step:
        steps_ += static_cast<std::uint64_t>(command.numbers[0]);
        reply.detail = "stepped " + std::to_string(command.numbers[0]);
        break;
      case ControlCommand::Kind::SetCamera:
        camera_[0] = command.numbers[0];
        camera_[1] = command.numbers[1];
        camera_[2] = command.numbers[2];
        reply.detail = "camera set";
        break;
      case ControlCommand::Kind::SetSun:
        sun_[0] = command.numbers[0];
        sun_[1] = command.numbers[1];
        sun_[2] = command.numbers[2];
        reply.detail = "sun set";
        break;
      case ControlCommand::Kind::SpawnCube:
        ++cubes_;
        reply.detail = "cube " + std::to_string(cubes_);
        break;
      case ControlCommand::Kind::Capture:
        captures_ += 1;
        reply.detail = "captured";
        break;
      case ControlCommand::Kind::ScrubStart:
        // Echo the parsed frame so tests can pin the wire-key mapping.
        last_scrub_start_ = command.number_count > 0U
                                ? static_cast<std::uint64_t>(command.numbers[0])
                                : 0U;
        reply.detail = "scrub_start " + std::to_string(last_scrub_start_);
        break;
      case ControlCommand::Kind::ScrubTo:
        last_scrub_to_ = command.number_count > 0U
                             ? static_cast<std::uint64_t>(command.numbers[0])
                             : 0U;
        reply.detail = "scrub_to " + std::to_string(last_scrub_to_);
        break;
      case ControlCommand::Kind::ScrubInfo:
        reply.detail = "scrub_info";
        break;
      default:
        reply.ok = false;
        reply.error = "fake host: unhandled kind";
        break;
    }
    return reply;
  }

  [[nodiscard]] std::string snapshot_json() const override {
    return std::string("{\"paused\":") + (paused_ ? "true" : "false") +
           ",\"steps\":" + std::to_string(steps_) +
           ",\"cubes\":" + std::to_string(cubes_) + "}";
  }

  bool paused_{false};
  std::uint64_t steps_{0};
  std::uint64_t cubes_{0};
  std::uint64_t captures_{0};
  // Sentinel 7777 = "never scrubbed": distinguishes a real 0 from no command.
  std::uint64_t last_scrub_start_{7777};
  std::uint64_t last_scrub_to_{7777};
  double camera_[3]{8.0, 3.0, 0.0};
  double sun_[3]{0.45, 0.7, 0.55};
};

class ControlServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = std::string("/tmp/omnicpp_test_control_") +
            std::to_string(::getpid()) + "_" +
            std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".sock";
    ::unlink(path_.c_str());
  }
  void TearDown() override { ::unlink(path_.c_str()); }

  //! Connects a raw client and returns its fd.
  [[nodiscard]] int connect_client() const {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path_.c_str());
    EXPECT_EQ(::connect(fd, reinterpret_cast<const sockaddr*>(&addr),
                        sizeof(addr)), 0)
        << "connect failed for " << path_;
    return fd;
  }

  //! Reads one newline-terminated protocol message (blocking, with cap).
  [[nodiscard]] std::string read_message(int fd) const {
    std::string message;
    char chunk[256];
    while (message.find('\n') == std::string::npos && message.size() < 65536) {
      const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
      if (got <= 0) break;
      message.append(chunk, static_cast<std::size_t>(got));
    }
    return message;
  }

  //! Sends a command line, pumps the server until a reply arrives, and
  //! returns it. The client fd stays in blocking mode; the server is the
  //! only thing polled.
  [[nodiscard]] std::string roundtrip(int fd, const std::string& line,
                                      ControlServer& server,
                                      ControlHost& host) const {
    const std::string wire = line + "\n";
    EXPECT_EQ(::send(fd, wire.data(), wire.size(), 0),
              static_cast<ssize_t>(wire.size()));
    std::string acc;
    for (int pump = 0; pump < 100 && acc.find('\n') == std::string::npos;
         ++pump) {
      (void)server.poll(host);
      // Non-blocking peek so a missing reply surfaces as a test failure
      // instead of hanging the suite.
      char chunk[256];
      const ssize_t got = ::recv(fd, chunk, sizeof(chunk), MSG_DONTWAIT);
      if (got > 0) acc.append(chunk, static_cast<std::size_t>(got));
    }
    EXPECT_NE(acc.find('\n'), std::string::npos)
        << "no reply for: " << line;
    return acc;
  }

  std::string path_;
};

TEST_F(ControlServerTest, WelcomeCarriesSnapshotOnConnect) {
  FakeHost host;
  ControlServer server;
  std::string error;
  ASSERT_TRUE(server.start(path_, error)) << error;

  const int fd = connect_client();
  ASSERT_EQ(server.poll(host), 0U);  // accept + welcome
  const std::string welcome = read_message(fd);
  EXPECT_NE(welcome.find("\"event\":\"welcome\""), std::string::npos) << welcome;
  EXPECT_NE(welcome.find("\"protocol\":1"), std::string::npos) << welcome;
  EXPECT_NE(welcome.find("\"snapshot\":{\"paused\":false,\"steps\":0,\"cubes\":0}"),
            std::string::npos)
      << welcome;
  EXPECT_EQ(server.client_count(), 1U);
  ::close(fd);
}

TEST_F(ControlServerTest, EveryCommandRoundTrips) {
  FakeHost host;
  ControlServer server;
  std::string error;
  ASSERT_TRUE(server.start(path_, error)) << error;

  const int fd = connect_client();
  (void)server.poll(host);
  (void)read_message(fd);  // swallow welcome

  struct Case {
    const char* line;
    const char* expect_detail;
  };
  const Case cases[] = {
      {"{\"cmd\":\"ping\",\"id\":1}", "pong"},
      {"{\"cmd\":\"pause\",\"id\":2}", "paused"},
      {"{\"cmd\":\"resume\",\"id\":3}", "resumed"},
      {"{\"cmd\":\"step\",\"id\":4,\"ticks\":7}", "stepped 7"},
      {"{\"cmd\":\"step\",\"id\":5}", "stepped 1"},  // default ticks
      {"{\"cmd\":\"set_camera\",\"id\":6,\"ex\":1,\"ey\":2,\"ez\":3}",
       "camera set"},
      {"{\"cmd\":\"set_sun\",\"id\":7,\"x\":0.3,\"y\":0.65,\"z\":0.7}",
       "sun set"},
      {"{\"cmd\":\"spawn_cube\",\"id\":8}", "cube 1"},
      {"{\"cmd\":\"capture\",\"id\":9}", "captured"},
  };
  for (const Case& c : cases) {
    const std::string reply = roundtrip(fd, c.line, server, host);
    EXPECT_NE(reply.find(std::string("\"detail\":\"") + c.expect_detail + "\""),
              std::string::npos)
        << c.line << " -> " << reply;
    // The client-assigned id must echo exactly.
    const std::string id_marker = reply.substr(0, reply.find(",\"ok\""));
    EXPECT_NE(id_marker.find("\"id\":"), std::string::npos) << reply;
  }
  EXPECT_TRUE(host.paused_ == false);  // resumed last
  EXPECT_EQ(host.steps_, 8U);
  EXPECT_EQ(host.camera_[0], 1.0);
  EXPECT_EQ(host.camera_[1], 2.0);
  EXPECT_EQ(host.camera_[2], 3.0);
  EXPECT_NEAR(host.sun_[0], 0.3, 1e-6);
  EXPECT_NEAR(host.sun_[1], 0.65, 1e-6);
  EXPECT_NEAR(host.sun_[2], 0.7, 1e-6);
  EXPECT_EQ(host.cubes_, 1U);
  EXPECT_EQ(host.captures_, 1U);
  ::close(fd);
}

//! v1.6: scrub commands must parse their "frame" wire key into numbers[0]
//! (scrub_start optional with default 0, scrub_to required) — the parser path
//! the live socket clients exercise and struct-built tests bypass.
TEST_F(ControlServerTest, ScrubCommandsParseWirePayloads) {
  FakeHost host;
  ControlServer server;
  std::string error;
  ASSERT_TRUE(server.start(path_, error)) << error;

  const int fd = connect_client();
  (void)server.poll(host);
  (void)read_message(fd);  // swallow welcome

  const std::string r1 =
      roundtrip(fd, "{\"cmd\":\"scrub_start\",\"id\":30,\"frame\":42}", server,
                host);
  EXPECT_NE(r1.find("\"detail\":\"scrub_start 42\""), std::string::npos) << r1;

  // No "frame" key: scrub_start defaults to frame 0.
  const std::string r2 =
      roundtrip(fd, "{\"cmd\":\"scrub_start\",\"id\":31}", server, host);
  EXPECT_NE(r2.find("\"detail\":\"scrub_start 0\""), std::string::npos) << r2;

  const std::string r3 =
      roundtrip(fd, "{\"cmd\":\"scrub_to\",\"id\":32,\"frame\":42}", server, host);
  EXPECT_NE(r3.find("\"detail\":\"scrub_to 42\""), std::string::npos) << r3;

  const std::string r4 =
      roundtrip(fd, "{\"cmd\":\"scrub_info\",\"id\":33}", server, host);
  EXPECT_NE(r4.find("\"detail\":\"scrub_info\""), std::string::npos) << r4;
  EXPECT_NE(r4.find("\"ok\":true"), std::string::npos) << r4;

  // scrub_to without "frame" is a parse error before any host dispatch.
  const std::string r5 =
      roundtrip(fd, "{\"cmd\":\"scrub_to\",\"id\":34}", server, host);
  EXPECT_NE(r5.find("missing \\\"frame\\\" unsigned field"), std::string::npos)
      << r5;
  EXPECT_NE(r5.find("\"ok\":false"), std::string::npos) << r5;

  EXPECT_EQ(host.last_scrub_start_, 0U);  // default-0 case ran last
  EXPECT_EQ(host.last_scrub_to_, 42U);
  ::close(fd);
}

TEST_F(ControlServerTest, MalformedAndUnknownAreProtocolErrors) {
  FakeHost host;
  ControlServer server;
  std::string error;
  ASSERT_TRUE(server.start(path_, error)) << error;

  const int fd = connect_client();
  (void)server.poll(host);
  (void)read_message(fd);

  const std::string reply1 =
      roundtrip(fd, "{\"cmd\":\"warp\",\"id\":10}", server, host);
  EXPECT_NE(reply1.find("unknown command \\\"warp\\\""), std::string::npos)
      << reply1;
  EXPECT_NE(reply1.find("\"ok\":false"), std::string::npos) << reply1;

  const std::string reply2 = roundtrip(fd, "{\"id\":11}", server, host);
  EXPECT_NE(reply2.find("missing \\\"cmd\\\" string field"), std::string::npos)
      << reply2;

  // Blank lines are ignored silently (no reply), so the next real command's
  // reply must carry its own id — no interleaved garbage.
  const std::string reply3 =
      roundtrip(fd, "   \n{\"cmd\":\"ping\",\"id\":12}", server, host);
  EXPECT_NE(reply3.find("\"id\":12"), std::string::npos) << reply3;
  EXPECT_NE(reply3.find("pong"), std::string::npos) << reply3;
  ::close(fd);
}

TEST_F(ControlServerTest, MultiClientIsolation) {
  FakeHost host;
  ControlServer server;
  std::string error;
  ASSERT_TRUE(server.start(path_, error)) << error;

  const int a = connect_client();
  const int b = connect_client();
  ASSERT_EQ(server.poll(host), 0U);
  (void)read_message(a);
  (void)read_message(b);

  (void)server.poll(host);  // accept both + welcomes
  const std::string ra = roundtrip(a, "{\"cmd\":\"ping\",\"id\":21}", server, host);
  (void)roundtrip(b, "{\"cmd\":\"step\",\"id\":22,\"ticks\":3}", server, host);
  EXPECT_NE(ra.find("pong"), std::string::npos);
  EXPECT_EQ(host.steps_, 3U);
  EXPECT_EQ(server.client_count(), 2U);
  ::close(a);
  ::close(b);
}

TEST_F(ControlServerTest, DisconnectSurvivesAndReconnectWorks) {
  FakeHost host;
  ControlServer server;
  std::string error;
  ASSERT_TRUE(server.start(path_, error)) << error;

  int fd = connect_client();
  (void)server.poll(host);
  (void)read_message(fd);
  ::close(fd);  // abrupt disconnect
  ASSERT_EQ(server.poll(host), 0U);
  EXPECT_EQ(server.client_count(), 0U);

  // Reconnect gets a fresh full snapshot (steps survived on the host).
  host.steps_ = 42;
  fd = connect_client();
  ASSERT_EQ(server.poll(host), 0U);
  const std::string welcome = read_message(fd);
  EXPECT_NE(welcome.find("\"steps\":42"), std::string::npos) << welcome;
  ::close(fd);
}

TEST(ControlServerBasics, DoubleStartRejectedAndStopIsIdempotent) {
  FakeHost host;
  ControlServer server;
  const std::string path =
      std::string("/tmp/omnicpp_test_control_basics_") +
      std::to_string(reinterpret_cast<std::uintptr_t>(&server)) + ".sock";
  ::unlink(path.c_str());
  std::string error;
  ASSERT_TRUE(server.start(path, error)) << error;
  std::string error2;
  EXPECT_FALSE(server.start(path, error2));
  server.stop();
  server.stop();  // idempotent
  EXPECT_FALSE(server.is_running());
  // The socket file is cleaned up.
  FILE* f = std::fopen(path.c_str(), "r");
  EXPECT_EQ(f, nullptr);
  ::unlink(path.c_str());
}

}  // namespace
