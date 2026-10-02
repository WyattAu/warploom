#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "warploom/core/ecs.hpp"

namespace {

struct Position { float x{0}; float y{0}; };
struct Velocity { float dx{0}; float dy{0}; };
struct Health { int hp{100}; };

TEST(Entity, EqualityAndOrdering) {
  omnicpp::core::Entity a{1, 0};
  omnicpp::core::Entity b{1, 0};
  omnicpp::core::Entity c{2, 0};
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_TRUE(a < c);
}

TEST(Entity, NullEntityIsDefault) {
  omnicpp::core::Entity e;
  EXPECT_EQ(e, omnicpp::core::kNullEntity);
}

TEST(ComponentTypeId, UniquePerType) {
  auto pos_id = omnicpp::core::component_type_id<Position>();
  auto vel_id = omnicpp::core::component_type_id<Velocity>();
  auto hp_id = omnicpp::core::component_type_id<Health>();
  EXPECT_NE(pos_id, vel_id);
  EXPECT_NE(pos_id, hp_id);
  EXPECT_NE(vel_id, hp_id);
}

TEST(ComponentTypeId, StableAcrossCalls) {
  auto a = omnicpp::core::component_type_id<Position>();
  auto b = omnicpp::core::component_type_id<Position>();
  EXPECT_EQ(a, b);
}

TEST(World, CreateEntityReturnsUniqueIds) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  auto e2 = world.create_entity();
  auto e3 = world.create_entity();
  EXPECT_NE(e1.id, e2.id);
  EXPECT_NE(e2.id, e3.id);
  EXPECT_EQ(world.entity_count(), 0u);
}

TEST(World, CreateEntityIncrementsId) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  auto e2 = world.create_entity();
  EXPECT_TRUE(e2.id > e1.id);
}

TEST(World, AddAndGetComponent) {
  omnicpp::core::World world;
  auto e = world.create_entity();
  world.add_component<Position>(e, {1.0f, 2.0f});
  world.add_component<Velocity>(e, {0.1f, 0.2f});

  EXPECT_TRUE(world.has_component<Position>(e));
  EXPECT_TRUE(world.has_component<Velocity>(e));
  EXPECT_FALSE(world.has_component<Health>(e));

  auto& pos = world.get_component<Position>(e);
  EXPECT_FLOAT_EQ(pos.x, 1.0f);
  EXPECT_FLOAT_EQ(pos.y, 2.0f);

  auto& vel = world.get_component<Velocity>(e);
  EXPECT_FLOAT_EQ(vel.dx, 0.1f);
  EXPECT_FLOAT_EQ(vel.dy, 0.2f);
}

TEST(World, AddComponentMultipleEntities) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  auto e2 = world.create_entity();
  world.add_component<Position>(e1, {1.0f, 0.0f});
  world.add_component<Position>(e2, {2.0f, 0.0f});
  // Crash happens here: moving e1 to Position+Health archetype
  world.add_component<Health>(e1, {100});

  EXPECT_FLOAT_EQ(world.get_component<Position>(e1).x, 1.0f);
  EXPECT_FLOAT_EQ(world.get_component<Position>(e2).x, 2.0f);
  EXPECT_EQ(world.get_component<Health>(e1).hp, 100);
  EXPECT_FALSE(world.has_component<Health>(e2));
}

TEST(World, RemoveComponent) {
  omnicpp::core::World world;
  auto e = world.create_entity();
  world.add_component<Position>(e, {1.0f, 2.0f});
  world.add_component<Velocity>(e, {0.1f, 0.2f});

  world.remove_component<Velocity>(e);
  EXPECT_TRUE(world.has_component<Position>(e));
  EXPECT_FALSE(world.has_component<Velocity>(e));
  EXPECT_FLOAT_EQ(world.get_component<Position>(e).x, 1.0f);
}

TEST(World, ForEachSingleComponent) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  auto e2 = world.create_entity();
  auto e3 = world.create_entity();
  world.add_component<Position>(e1, {1.0f, 0.0f});
  world.add_component<Position>(e2, {2.0f, 0.0f});
  world.add_component<Health>(e3, {50});

  std::vector<float> xs;
  world.for_each<Position>([&](omnicpp::core::Entity, Position& p) {
    xs.push_back(p.x);
  });
  EXPECT_EQ(xs.size(), 2u);
  EXPECT_FLOAT_EQ(xs[0], 1.0f);
  EXPECT_FLOAT_EQ(xs[1], 2.0f);
}

TEST(World, ForEachTwoComponents) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  auto e2 = world.create_entity();
  auto e3 = world.create_entity();
  world.add_component<Position>(e1, {1.0f, 0.0f});
  world.add_component<Velocity>(e1, {0.1f, 0.0f});
  world.add_component<Position>(e2, {2.0f, 0.0f});
  world.add_component<Velocity>(e2, {0.2f, 0.0f});
  world.add_component<Health>(e3, {50});

  int count = 0;
  world.for_each<Position, Velocity>([&](omnicpp::core::Entity, Position& p, Velocity& v) {
    p.x += v.dx;
    ++count;
  });
  EXPECT_EQ(count, 2);
  EXPECT_FLOAT_EQ(world.get_component<Position>(e1).x, 1.1f);
  EXPECT_FLOAT_EQ(world.get_component<Position>(e2).x, 2.2f);
}

TEST(World, EntityCountIncludesAllArchetypes) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  auto e2 = world.create_entity();
  auto e3 = world.create_entity();
  world.add_component<Position>(e1, {});
  world.add_component<Position>(e2, {});
  world.add_component<Health>(e3, {});
  EXPECT_EQ(world.entity_count(), 3u);
}

TEST(Archetype, StoresAndRemovesEntities) {
  omnicpp::core::Archetype arch;
  omnicpp::core::ComponentMask mask;
  mask.set(omnicpp::core::component_type_id<Position>());
  arch.set_mask(mask);

  arch.add_entity(omnicpp::core::Entity{1, 0});
  arch.add_entity(omnicpp::core::Entity{2, 0});
  EXPECT_EQ(arch.size(), 2u);

  (void)arch.remove_entity(0);
  EXPECT_EQ(arch.size(), 1u);
  EXPECT_EQ(arch.entities()[0].id, 2u);
}

TEST(Archetype, FindEntity) {
  omnicpp::core::Archetype arch;
  arch.add_entity(omnicpp::core::Entity{5, 0});
  EXPECT_EQ(arch.find_entity(omnicpp::core::Entity{5, 0}), 0u);
  EXPECT_EQ(arch.find_entity(omnicpp::core::Entity{99, 0}), arch.size());
}

TEST(SystemScheduler, TopologicalOrdering) {
  omnicpp::core::SystemScheduler scheduler;
  std::vector<std::string> order;

  scheduler.add_system({"input", [&](omnicpp::core::World&, std::uint64_t) noexcept {
    order.push_back("input");
  }, {}});
  scheduler.add_system({"physics", [&](omnicpp::core::World&, std::uint64_t) noexcept {
    order.push_back("physics");
  }, {"input"}});
  scheduler.add_system({"render", [&](omnicpp::core::World&, std::uint64_t) noexcept {
    order.push_back("render");
  }, {"physics"}});

  EXPECT_TRUE(scheduler.build_schedule());
  omnicpp::core::World world;
  scheduler.run(world, 0);

  EXPECT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], "input");
  EXPECT_EQ(order[1], "physics");
  EXPECT_EQ(order[2], "render");
}

TEST(SystemScheduler, DetectsCycle) {
  omnicpp::core::SystemScheduler scheduler;
  scheduler.add_system({"a", [](omnicpp::core::World&, std::uint64_t) noexcept {}, {"b"}});
  scheduler.add_system({"b", [](omnicpp::core::World&, std::uint64_t) noexcept {}, {"a"}});
  EXPECT_FALSE(scheduler.build_schedule());
}

TEST(SystemScheduler, IndependentSystemsRunInRegistrationOrder) {
  omnicpp::core::SystemScheduler scheduler;
  std::vector<std::string> order;

  scheduler.add_system({"x", [&](omnicpp::core::World&, std::uint64_t) noexcept {
    order.push_back("x");
  }, {}});
  scheduler.add_system({"y", [&](omnicpp::core::World&, std::uint64_t) noexcept {
    order.push_back("y");
  }, {}});
  scheduler.add_system({"z", [&](omnicpp::core::World&, std::uint64_t) noexcept {
    order.push_back("z");
  }, {}});

  EXPECT_TRUE(scheduler.build_schedule());
  omnicpp::core::World world;
  scheduler.run(world, 0);

  EXPECT_EQ(order[0], "x");
  EXPECT_EQ(order[1], "y");
  EXPECT_EQ(order[2], "z");
}

TEST(SystemScheduler, ParallelExecutionRunsIndependentSystemsConcurrently) {
  omnicpp::core::SystemScheduler scheduler;
  std::atomic<int> counter{0};

  // Three systems with no dependencies and different component masks
  omnicpp::core::ComponentMask pos_mask;
  pos_mask.set(omnicpp::core::component_type_id<Position>());
  omnicpp::core::ComponentMask vel_mask;
  vel_mask.set(omnicpp::core::component_type_id<Velocity>());
  omnicpp::core::ComponentMask hp_mask;
  hp_mask.set(omnicpp::core::component_type_id<Health>());

  scheduler.add_system({"physics", [&](omnicpp::core::World&, std::uint64_t) {
    counter.fetch_add(1, std::memory_order_relaxed);
  }, {}, pos_mask});
  scheduler.add_system({"movement", [&](omnicpp::core::World&, std::uint64_t) {
    counter.fetch_add(1, std::memory_order_relaxed);
  }, {}, vel_mask});
  scheduler.add_system({"health", [&](omnicpp::core::World&, std::uint64_t) {
    counter.fetch_add(1, std::memory_order_relaxed);
  }, {}, hp_mask});

  EXPECT_TRUE(scheduler.build_schedule());
  omnicpp::core::World world;
  omnicpp::core::ThreadPool pool(4);
  scheduler.run_parallel(world, 0, pool);
  EXPECT_EQ(counter.load(), 3);
}

TEST(Query, SingleComponentQuery) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  auto e2 = world.create_entity();
  auto e3 = world.create_entity();
  world.add_component<Position>(e1, {1.0f, 0.0f});
  world.add_component<Position>(e2, {2.0f, 0.0f});
  world.add_component<Health>(e3, {50});

  std::vector<float> xs;
  auto count = world.query<Position>([&](omnicpp::core::Entity, Position& p) {
    xs.push_back(p.x);
  });
  EXPECT_EQ(count, 2u);
  EXPECT_EQ(xs.size(), 2u);
  EXPECT_FLOAT_EQ(xs[0], 1.0f);
  EXPECT_FLOAT_EQ(xs[1], 2.0f);
}

TEST(Query, TwoComponentQuery) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  auto e2 = world.create_entity();
  auto e3 = world.create_entity();
  world.add_component<Position>(e1, {1.0f, 0.0f});
  world.add_component<Velocity>(e1, {0.1f, 0.0f});
  world.add_component<Position>(e2, {2.0f, 0.0f});
  world.add_component<Velocity>(e2, {0.2f, 0.0f});
  world.add_component<Health>(e3, {50});

  int count = 0;
  world.query<Position, Velocity>([&](omnicpp::core::Entity, Position& p, Velocity& v) {
    p.x += v.dx;
    ++count;
  });
  EXPECT_EQ(count, 2);
  EXPECT_FLOAT_EQ(world.get_component<Position>(e1).x, 1.1f);
  EXPECT_FLOAT_EQ(world.get_component<Position>(e2).x, 2.2f);
}

TEST(Query, QueryIfWithPredicate) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  auto e2 = world.create_entity();
  auto e3 = world.create_entity();
  world.add_component<Health>(e1, {100});
  world.add_component<Health>(e2, {0});
  world.add_component<Health>(e3, {50});

  auto count = world.query_if<Health>(
      [](omnicpp::core::Entity, Health& h) { return h.hp > 0; },
      [](omnicpp::core::Entity, Health& h) { h.hp -= 10; });
  EXPECT_EQ(count, 2u);
  EXPECT_EQ(world.get_component<Health>(e1).hp, 90);
  EXPECT_EQ(world.get_component<Health>(e2).hp, 0);
  EXPECT_EQ(world.get_component<Health>(e3).hp, 40);
}

TEST(Query, CountIf) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  auto e2 = world.create_entity();
  auto e3 = world.create_entity();
  world.add_component<Health>(e1, {100});
  world.add_component<Health>(e2, {0});
  world.add_component<Health>(e3, {50});

  auto alive = world.count_if<Health>(
      [](omnicpp::core::Entity, const Health& h) { return h.hp > 0; });
  EXPECT_EQ(alive, 2u);
}

TEST(BumpAllocator, AllocateAndReset) {
  omnicpp::core::BumpAllocator alloc(1024);
  EXPECT_EQ(alloc.bytes_used(), 0u);
  EXPECT_EQ(alloc.capacity(), 1024u);

  auto* p1 = alloc.allocate(64);
  EXPECT_NE(p1, nullptr);
  EXPECT_GE(alloc.bytes_used(), 64u);

  auto* p2 = alloc.allocate(128);
  EXPECT_NE(p2, nullptr);
  EXPECT_GT(p2, p1);

  alloc.reset();
  EXPECT_EQ(alloc.bytes_used(), 0u);
}

TEST(BumpAllocator, ExhaustionReturnsNull) {
  omnicpp::core::BumpAllocator alloc(64);
  auto* p1 = alloc.allocate(64);
  EXPECT_NE(p1, nullptr);
  auto* p2 = alloc.allocate(1);
  EXPECT_EQ(p2, nullptr);
}

TEST(BumpAllocator, AlignedAllocation) {
  omnicpp::core::BumpAllocator alloc(256);
  auto* p1 = alloc.allocate(1, 64);
  EXPECT_NE(p1, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p1) % 64, 0u);
  auto* p2 = alloc.allocate(1, 64);
  EXPECT_NE(p2, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p2) % 64, 0u);
}

TEST(EntityRecycling, DestroyAndReuse) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  auto e2 = world.create_entity();
  EXPECT_EQ(e1.id, 1u);
  EXPECT_EQ(e2.id, 2u);

  world.add_component<Position>(e1, {1.0f, 0.0f});
  world.destroy_entity(e1);
  EXPECT_FALSE(world.is_alive(e1));

  auto e3 = world.create_entity();
  EXPECT_EQ(e3.id, 1u);
  EXPECT_EQ(e3.generation, 1u);
  EXPECT_TRUE(world.is_alive(e3));
  EXPECT_FALSE(world.is_alive(e1));
}

TEST(EntityRecycling, StaleHandleRejected) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  world.add_component<Position>(e1, {1.0f, 0.0f});
  world.destroy_entity(e1);

  auto e2 = world.create_entity();
  EXPECT_EQ(e2.id, 1u);
  EXPECT_NE(e2.generation, e1.generation);

  // Old handle should not affect new entity
  world.add_component<Health>(e2, {50});
  EXPECT_TRUE(world.has_component<Health>(e2));
}

TEST(EntityRecycling, MultipleRecycles) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  world.destroy_entity(e1);
  auto e2 = world.create_entity();
  EXPECT_EQ(e2.id, e1.id);
  EXPECT_GT(e2.generation, e1.generation);
  world.destroy_entity(e2);
  auto e3 = world.create_entity();
  EXPECT_EQ(e3.id, e1.id);
  EXPECT_GT(e3.generation, e2.generation);
}

TEST(ChangeTracking, MarkDirtyAndClear) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  world.add_component<Position>(e1, {1.0f, 0.0f});

  EXPECT_FALSE(world.is_dirty<Position>(e1));
  EXPECT_FALSE(world.any_dirty());

  world.mark_dirty<Position>(e1);
  EXPECT_TRUE(world.is_dirty<Position>(e1));
  EXPECT_TRUE(world.any_dirty());

  world.clear_dirties();
  EXPECT_FALSE(world.is_dirty<Position>(e1));
  EXPECT_FALSE(world.any_dirty());
}

TEST(ChangeTracking, SetComponentMarksDirty) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  world.add_component<Position>(e1, {1.0f, 0.0f});

  world.set_component<Position>(e1, {5.0f, 10.0f});
  EXPECT_TRUE(world.is_dirty<Position>(e1));
  EXPECT_FLOAT_EQ(world.get_component<Position>(e1).x, 5.0f);
}

TEST(ChangeTracking, DirtyOnlyForModifiedType) {
  omnicpp::core::World world;
  auto e1 = world.create_entity();
  world.add_component<Position>(e1, {1.0f, 0.0f});
  world.add_component<Health>(e1, {100});

  world.mark_dirty<Position>(e1);
  EXPECT_TRUE(world.is_dirty<Position>(e1));
  EXPECT_FALSE(world.is_dirty<Health>(e1));
}

TEST(ComponentPool, AcquireAndRelease) {
  omnicpp::core::ComponentPool<Position> pool(16);
  EXPECT_EQ(pool.available(), 16u);
  EXPECT_EQ(pool.capacity(), 16u);

  auto* p1 = pool.acquire();
  EXPECT_NE(p1, nullptr);
  EXPECT_EQ(pool.available(), 15u);

  pool.release(p1);
  EXPECT_EQ(pool.available(), 16u);
}

TEST(ComponentPool, ExhaustionReturnsNull) {
  omnicpp::core::ComponentPool<Position> pool(2);
  auto* p1 = pool.acquire();
  auto* p2 = pool.acquire();
  EXPECT_NE(p1, nullptr);
  EXPECT_NE(p2, nullptr);
  auto* p3 = pool.acquire();
  EXPECT_EQ(p3, nullptr);
}

TEST(ComponentPool, ReleasedSlotIsReused) {
  omnicpp::core::ComponentPool<Position> pool(1);
  auto* p1 = pool.acquire();
  EXPECT_NE(p1, nullptr);
  pool.release(p1);
  auto* p2 = pool.acquire();
  EXPECT_NE(p2, nullptr);
  EXPECT_EQ(p1, p2); // Same slot reused
}

TEST(ComponentPool, NullReleaseIsSafe) {
  omnicpp::core::ComponentPool<Position> pool(4);
  pool.release(nullptr); // Should not crash
  EXPECT_EQ(pool.available(), 4u);
}

} // namespace
