//! @file test_contract.cpp
//! @brief Contract-facility proofs: OMNICPP_CONTRACT violations abort with a
//!        diagnostic naming the expression and location, and the ECS runs its
//!        full lifecycle under check-mode contracts (every converted invariant
//!        holds on the happy path).

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "engine/core/contract.hpp"
#include "engine/core/ecs.hpp"

namespace {

TEST(Contract, ViolationAborts) {
  // The violation path prints to stderr and aborts (verified exit status in
  // CI); run it in a child process so the test binary survives.
  const std::string cmd =
      "/bin/sh -c '" OMNICPP_TEST_BIN_DIR
      "/omnicpp_contract_violator 2>&1'";  // violator reports on stderr
  std::array<char, 128> buf{};
  std::string output;
  FILE* pipe = popen(cmd.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
    output += buf.data();
  }
  const int rc = pclose(pipe);
  EXPECT_TRUE(WIFEXITED(rc) || WIFSIGNALED(rc));
  EXPECT_NE(output.find("omnicpp contract violation"), std::string::npos)
      << "stderr was: " << output;
  EXPECT_NE(output.find("1 == 2"), std::string::npos) << output;
  // popen reports signal death as 128+SIGABRT through the shell.
  EXPECT_EQ(rc / 256, 134) << "violation must abort, rc=" << rc;
}

TEST(Contract, EcsLifecycleUnderContracts) {
  omnicpp::core::World world;
  const auto e1 = world.create_entity();
  const auto e2 = world.create_entity();
  EXPECT_NE(e1.id, e2.id);

  world.add_component<int>(e1, 41);
  world.add_component<double>(e2, 2.5);
  EXPECT_TRUE(world.has_component<int>(e1));
  EXPECT_FALSE(world.has_component<int>(e2));
  EXPECT_EQ(world.get_component<int>(e1), 41);

  std::size_t visits = 0;
  world.for_each<int>([&](omnicpp::core::Entity, int& v) {
    v += 1;
    ++visits;
  });
  EXPECT_EQ(visits, 1U);
  EXPECT_EQ(world.get_component<int>(e1), 42);

  world.destroy_entity(e1);
  EXPECT_FALSE(world.is_alive(e1));
  EXPECT_FALSE(world.has_component<int>(e1));
}

}  // namespace
