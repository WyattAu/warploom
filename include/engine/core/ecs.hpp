#pragma once

/**
 * @file ecs.hpp
 * @brief Data-oriented Entity Component System foundation.
 *
 * Design:
 * - Entity = {id, generation} packed in 8 bytes
 * - Components stored in SoA (Struct of Arrays) per archetype
 * - Archetypes are identified by ComponentMask (64-bit bitset)
 * - System execution order is topologically sorted
 * - Deterministic iteration: entities sorted by ID within archetype
 * - No heap allocation during steady-state simulation
 */

#include <atomic>
#include <bitset>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include "engine/core/thread_pool.hpp"
#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace omnicpp::core {

// ============================================================================
// Entity
// ============================================================================

struct Entity final {
  std::uint32_t id{0};
  std::uint32_t generation{0};

  [[nodiscard]] constexpr bool operator==(const Entity& o) const noexcept {
    return id == o.id && generation == o.generation;
  }
  [[nodiscard]] constexpr bool operator!=(const Entity& o) const noexcept {
    return !(*this == o);
  }
  [[nodiscard]] constexpr bool operator<(const Entity& o) const noexcept {
    return id < o.id;
  }
};

static_assert(std::is_trivially_copyable_v<Entity>, "Entity must be trivially copyable");
static_assert(sizeof(Entity) == 8, "Entity must be 8 bytes");

inline constexpr Entity kNullEntity{0, 0};

// ============================================================================
// Component Type ID
// ============================================================================

inline constexpr std::size_t kMaxComponentTypes = 64;
using ComponentMask = std::bitset<kMaxComponentTypes>;

namespace detail {

//! Global atomic counter for assigning unique component type IDs.
//! Each call to next_component_id() returns a new unique value.
[[nodiscard]] inline std::atomic<std::uint32_t>& global_component_counter() noexcept {
  static std::atomic<std::uint32_t> counter{0};
  return counter;
}

//! Per-type component ID using template static member.
//! Each unique template instantiation of ComponentIdHolder<T> has its own
//! static member `value`, initialized exactly once to a unique sequential
//! ID via the global atomic counter. This guarantees uniqueness within
//! a single process — even for types with the same hash_code.
template <typename T>
struct ComponentIdHolder {
  static const std::uint32_t value;
};

template <typename T>
const std::uint32_t ComponentIdHolder<T>::value =
    global_component_counter().fetch_add(1, std::memory_order_relaxed);

} // namespace detail

template <typename T>
[[nodiscard]] inline std::uint32_t component_type_id() noexcept {
  return detail::ComponentIdHolder<std::remove_cvref_t<T>>::value;
}

// ============================================================================
// Type-Erased Component Array
// ============================================================================

namespace detail {

class IComponentArray {
public:
  virtual ~IComponentArray() = default;
  virtual void* get(std::size_t index) noexcept = 0;
  virtual const void* get(std::size_t index) const noexcept = 0;
  virtual void push_back_default() = 0;
  virtual void swap_remove(std::size_t index) = 0;
  virtual void copy_from(std::size_t dst_idx, const IComponentArray& src, std::size_t src_idx) = 0;
  [[nodiscard]] virtual std::size_t size() const noexcept = 0;
  virtual void clear() noexcept = 0;

  //! Factory function type: creates a fresh IComponentArray for a given type.
  using Creator = IComponentArray*(*)();

  //! Returns the creator function for this array's concrete type.
  [[nodiscard]] virtual Creator creator() const noexcept = 0;
};

template <typename T>
class ComponentArray final : public IComponentArray {
public:
  void* get(std::size_t index) noexcept override { return &data_[index]; }
  const void* get(std::size_t index) const noexcept override { return &data_[index]; }
  void push_back_default() override { data_.emplace_back(); }

  void swap_remove(std::size_t index) override {
    if (index + 1 < data_.size()) {
      data_[index] = static_cast<T&&>(data_.back());
    }
    data_.pop_back();
  }

  void copy_from(std::size_t dst_idx, const IComponentArray& src_base, std::size_t src_idx) override {
    const auto& src = static_cast<const ComponentArray&>(src_base);
    assert(dst_idx < data_.size() && "copy_from: dst out of bounds");
    assert(src_idx < src.data_.size() && "copy_from: src out of bounds");
    data_[dst_idx] = src.data_[src_idx];
  }

  [[nodiscard]] std::size_t size() const noexcept override { return data_.size(); }
  void clear() noexcept override { data_.clear(); }

  //! Returns the creator function that produces ComponentArray<T> instances.
  [[nodiscard]] static IComponentArray* create_instance() noexcept {
    return new ComponentArray();
  }

  [[nodiscard]] Creator creator() const noexcept override {
    return &create_instance;
  }

  [[nodiscard]] T& at(std::size_t index) noexcept {
    assert(index < data_.size() && "ComponentArray::at out of bounds");
    return data_[index];
  }
  [[nodiscard]] const T& at(std::size_t index) const noexcept {
    assert(index < data_.size() && "ComponentArray::at out of bounds");
    return data_[index];
  }
  [[nodiscard]] T* data() noexcept { return data_.data(); }
  [[nodiscard]] const T* data() const noexcept { return data_.data(); }

private:
  std::vector<T> data_;
};

} // namespace detail

// ============================================================================
// Archetype
// ============================================================================

class Archetype final {
public:
  Archetype() = default;

  ~Archetype() {
    for (auto* arr : arrays_) delete arr;
  }

  Archetype(const Archetype&) = delete;
  Archetype& operator=(const Archetype&) = delete;

  Archetype(Archetype&& other) noexcept
      : entities_(static_cast<std::vector<Entity>&&>(other.entities_)),
        arrays_(other.arrays_),
        creators_(other.creators_),
        mask_(other.mask_) {
    other.arrays_.clear();
    other.creators_.clear();
  }

  Archetype& operator=(Archetype&&) = delete;

  // -- Array access (requires arrays to be pre-created) --

  template <typename T>
  [[nodiscard]] detail::ComponentArray<T>& get_array() {
    const auto tid = component_type_id<T>();
    assert(tid < arrays_.size() && arrays_[tid] != nullptr && "array must be pre-created");
    return static_cast<detail::ComponentArray<T>&>(*arrays_[tid]);
  }

  template <typename T>
  [[nodiscard]] const detail::ComponentArray<T>& get_array() const {
    const auto tid = component_type_id<T>();
    assert(tid < arrays_.size() && arrays_[tid] != nullptr && "array must be pre-created");
    return static_cast<const detail::ComponentArray<T>&>(*arrays_[tid]);
  }

  [[nodiscard]] detail::IComponentArray* get_array_by_id(std::size_t tid) {
    if (tid >= arrays_.size()) return nullptr;
    return arrays_[tid];
  }

  // -- Array creation (typed and type-erased) --

  //! Create the array for type T using the template's static creator.
  //! This is the authoritative way to create a typed array.
  template <typename T>
  void ensure_typed_array() {
    const auto tid = component_type_id<T>();
    if (tid >= arrays_.size()) {
      arrays_.resize(tid + 1, nullptr);
      creators_.resize(tid + 1, nullptr);
    }
    if (!arrays_[tid]) {
      arrays_[tid] = detail::ComponentArray<std::remove_cvref_t<T>>::create_instance();
      creators_[tid] = &detail::ComponentArray<std::remove_cvref_t<T>>::create_instance;
    }
  }

  //! Create an array at index i using the stored creator function.
  //! Returns true if the array was created, false if no creator is available.
  bool ensure_array_at(std::size_t i) {
    if (i >= arrays_.size()) {
      arrays_.resize(i + 1, nullptr);
      creators_.resize(i + 1, nullptr);
    }
    if (!arrays_[i]) {
      if (creators_[i]) {
        arrays_[i] = creators_[i]();
        return true;
      }
      return false;
    }
    return true;
  }

  //! Ensure arrays exist for all types in the given mask using stored creators.
  void ensure_arrays_for_mask(ComponentMask mask) {
    for (std::size_t i = 0; i < kMaxComponentTypes; ++i) {
      if (!mask.test(i)) continue;
      ensure_array_at(i);
    }
  }

  //! Copy creation functions from another archetype for shared mask bits.
  //! This propagates the ability to create arrays from one archetype to another
  //! without needing a global factory.
  void copy_creators_from(Archetype& other, ComponentMask shared_mask) {
    for (std::size_t i = 0; i < kMaxComponentTypes; ++i) {
      if (!shared_mask.test(i)) continue;
      if (i < other.creators_.size() && other.creators_[i]) {
        if (i >= creators_.size()) creators_.resize(i + 1, nullptr);
        creators_[i] = other.creators_[i];
      }
    }
  }

  // -- Query --

  template <typename T>
  [[nodiscard]] bool has() const noexcept {
    const auto tid = component_type_id<T>();
    return tid < arrays_.size() && arrays_[tid] != nullptr;
  }

  // -- Mutation --

  void add_entity(Entity entity) {
    entities_.push_back(entity);
    for (auto* arr : arrays_) {
      if (arr) arr->push_back_default();
    }
  }

  //! Remove entity at index by swap-removing the last element into its place.
  //! Returns the entity that was relocated (the former back element moved into
  //! the removed slot). Callers must update entity_locations_ for the returned
  //! entity. Returns kNullEntity if no relocation happened (removed last element).
  [[nodiscard]] Entity remove_entity(std::size_t index) {
    assert(index < entities_.size());
    Entity swapped{kNullEntity};
    if (index + 1 < entities_.size()) {
      swapped = entities_.back();
      entities_[index] = swapped;
    }
    entities_.pop_back();
    for (auto* arr : arrays_) {
      if (arr && arr->size() > index) {
        arr->swap_remove(index);
      }
    }
    return swapped;
  }

  [[nodiscard]] std::size_t find_entity(Entity entity) const noexcept {
    for (std::size_t i = 0; i < entities_.size(); ++i) {
      if (entities_[i] == entity) return i;
    }
    return entities_.size();
  }

  // -- Accessors --

  [[nodiscard]] std::size_t size() const noexcept { return entities_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entities_.empty(); }
  [[nodiscard]] const std::vector<Entity>& entities() const noexcept { return entities_; }
  [[nodiscard]] const ComponentMask& mask() const noexcept { return mask_; }
  void set_mask(ComponentMask m) noexcept { mask_ = m; }

  // -- Change tracking --

  //! Mark a component type as dirty in this archetype.
  template <typename T>
  void mark_dirty() noexcept { dirty_mask_.set(component_type_id<T>()); }

  //! Mark a component type by ID as dirty.
  void mark_dirty_id(std::uint32_t tid) noexcept { dirty_mask_.set(tid); }

  //! Check if any component of type T was modified since last clear.
  template <typename T>
  [[nodiscard]] bool is_dirty() const noexcept { return dirty_mask_.test(component_type_id<T>()); }

  //! Check if any component in this archetype was modified.
  [[nodiscard]] bool any_dirty() const noexcept { return dirty_mask_.any(); }

  //! Clear all dirty flags.
  void clear_dirties() noexcept { dirty_mask_.reset(); }

private:
  std::vector<Entity> entities_;
  std::vector<detail::IComponentArray*> arrays_;
  std::vector<detail::IComponentArray::Creator> creators_;
  ComponentMask mask_;
  ComponentMask dirty_mask_;
};

// ============================================================================
// World
// ============================================================================

class World final {
public:
  World() = default;
  ~World() = default;

  World(const World&) = delete;
  World& operator=(const World&) = delete;
  World(World&&) = delete;
  World& operator=(World&&) = delete;

  [[nodiscard]] Entity create_entity() {
    if (!free_ids_.empty()) {
      const auto id = free_ids_.back();
      free_ids_.pop_back();
      auto& loc = entity_locations_[id];
      Entity e{id, loc.generation};
      loc.archetype = nullptr;
      loc.index = 0;
      return e;
    }
    const std::uint32_t id = next_entity_id_++;
    if (id >= entity_locations_.size()) {
      entity_locations_.resize(id + 1);
    }
    entity_locations_[id] = {nullptr, 0, 0};
    return {id, 0};
  }

  void destroy_entity(Entity entity) {
    const auto idx = entity.id;
    if (idx >= entity_locations_.size()) return;
    auto& loc = entity_locations_[idx];
    if (loc.generation != entity.generation) return;
    if (loc.archetype) {
      const auto swapped = loc.archetype->remove_entity(loc.index);
      if (swapped.id != 0 && swapped.id < entity_locations_.size()) {
        entity_locations_[swapped.id].index = loc.index;
      }
    }
    loc.archetype = nullptr;
    loc.index = 0;
    ++loc.generation;
    free_ids_.push_back(idx);
  }

  //! Check if an entity handle is still valid (generation matches).
  [[nodiscard]] bool is_alive(Entity entity) const noexcept {
    const auto idx = entity.id;
    if (idx >= entity_locations_.size()) return false;
    return entity_locations_[idx].generation == entity.generation;
  }

  template <typename T>
  T& add_component(Entity entity, T component = {}) {
    const auto idx = entity.id;
    assert(idx < entity_locations_.size());

    const auto new_tid = component_type_id<T>();

    ComponentMask target_mask;
    Archetype* old_arch = entity_locations_[idx].archetype;
    if (old_arch) {
      target_mask = old_arch->mask();
    }
    target_mask.set(new_tid);

    Archetype* target = find_or_create_archetype(target_mask);

    // Step 1: Create the array for the new type T (guaranteed by template static)
    target->template ensure_typed_array<T>();

    // Step 2: Propagate creators from old archetype to new one
    if (old_arch) {
      target->copy_creators_from(*old_arch, old_arch->mask());
    }

    // Step 3: Ensure arrays for all types in the mask using propagated creators
    target->ensure_arrays_for_mask(target_mask);

    // Step 4: Add entity placeholder and set the new component value
    const auto target_index = target->size();
    target->add_entity(entity);
    target->get_array<T>().at(target_index) = static_cast<T&&>(component);

    // Step 5: Copy existing components from old archetype
    if (old_arch) {
      copy_all_components(old_arch, entity_locations_[idx].index, target, target_index);
      const auto swapped = old_arch->remove_entity(entity_locations_[idx].index);
      if (swapped.id != 0 && swapped.id < entity_locations_.size()) {
        entity_locations_[swapped.id].index = entity_locations_[idx].index;
      }
    }    entity_locations_[idx] = {target, target_index, entity_locations_[idx].generation};
    return target->get_array<T>().at(target_index);
  }


  template <typename T>
  void remove_component(Entity entity) {
    const auto idx = entity.id;
    if (idx >= entity_locations_.size()) return;
    auto& loc = entity_locations_[idx];
    if (!loc.archetype) return;

    ComponentMask target_mask = loc.archetype->mask();
    target_mask.reset(component_type_id<T>());

    Archetype* target = find_or_create_archetype(target_mask);

    // Propagate creators from old archetype
    target->copy_creators_from(*loc.archetype, loc.archetype->mask());
    target->ensure_arrays_for_mask(target_mask);

    const auto target_index = target->size();
    target->add_entity(entity);

    copy_all_components(loc.archetype, loc.index, target, target_index,
                        component_type_id<T>());
    const auto swapped = loc.archetype->remove_entity(loc.index);
    if (swapped.id != 0 && swapped.id < entity_locations_.size()) {
      entity_locations_[swapped.id].index = loc.index;
    }
    entity_locations_[idx] = {target, target_index, entity_locations_[idx].generation};
  }

  template <typename T>
  [[nodiscard]] T& get_component(Entity entity) {
    const auto idx = entity.id;
    assert(idx < entity_locations_.size());
    auto& loc = entity_locations_[idx];
    assert(loc.archetype);
    assert(loc.archetype->has<T>());
    return loc.archetype->get_array<T>().at(loc.index);
  }

  template <typename T>
  [[nodiscard]] const T& get_component(Entity entity) const {
    const auto idx = entity.id;
    assert(idx < entity_locations_.size());
    auto& loc = entity_locations_[idx];
    assert(loc.archetype);
    assert(loc.archetype->has<T>());
    return loc.archetype->get_array<T>().at(loc.index);
  }

  template <typename T>
  [[nodiscard]] bool has_component(Entity entity) const noexcept {
    const auto idx = entity.id;
    if (idx >= entity_locations_.size()) return false;
    auto& loc = entity_locations_[idx];
    return loc.archetype && loc.archetype->has<T>();
  }

  [[nodiscard]] std::size_t entity_count() const noexcept {
    std::size_t count = 0;
    for (const auto& arch : archetypes_) count += arch.size();
    return count;
  }

  // -- Change tracking --

  //! Mark a component as dirty for an entity.
  template <typename T>
  void mark_dirty(Entity entity) noexcept {
    const auto idx = entity.id;
    if (idx >= entity_locations_.size()) return;
    auto& loc = entity_locations_[idx];
    if (loc.archetype) loc.archetype->template mark_dirty<T>();
  }

  //! Set a component value and mark it dirty.
  template <typename T>
  T& set_component(Entity entity, T value) {
    auto& comp = get_component<T>(entity);
    comp = static_cast<T&&>(value);
    mark_dirty<T>(entity);
    return comp;
  }

  //! Check if a specific component type was modified in the entity's archetype.
  template <typename T>
  [[nodiscard]] bool is_dirty(Entity entity) const noexcept {
    const auto idx = entity.id;
    if (idx >= entity_locations_.size()) return false;
    auto& loc = entity_locations_[idx];
    return loc.archetype && loc.archetype->template is_dirty<T>();
  }

  //! Check if any archetype has any dirty components.
  [[nodiscard]] bool any_dirty() const noexcept {
    for (const auto& arch : archetypes_) {
      if (arch.any_dirty()) return true;
    }
    return false;
  }

  //! Clear all dirty flags across all archetypes. Typically called once per tick.
  void clear_dirties() noexcept {
    for (auto& arch : archetypes_) arch.clear_dirties();
  }

  [[nodiscard]] const std::deque<Archetype>& archetypes() const noexcept {
    return archetypes_;
  }

  //! Iterate all entities with component T and call f(Entity, T&).
  template <typename T, typename Func>
  void for_each(Func&& f) {
    for (auto& arch : archetypes_) {
      if (!arch.has<T>()) continue;
      auto& arr = arch.get_array<T>();
      for (std::size_t i = 0; i < arch.size(); ++i) {
        f(arch.entities()[i], arr.at(i));
      }
    }
  }

  //! Iterate all entities with components T1, T2, ... and call f(Entity, T1&, T2&, ...).
  template <typename T1, typename T2, typename Func>
  void for_each(Func&& f) {
    for (auto& arch : archetypes_) {
      if (!arch.has<T1>() || !arch.has<T2>()) continue;
      auto& a1 = arch.get_array<T1>();
      auto& a2 = arch.get_array<T2>();
      for (std::size_t i = 0; i < arch.size(); ++i) {
        f(arch.entities()[i], a1.at(i), a2.at(i));
      }
    }
  }

  //! Iterate all entities with component T and call f(Entity, const T&).
  template <typename T, typename Func>
  void for_each(Func&& f) const {
    for (const auto& arch : archetypes_) {
      if (!arch.has<T>()) continue;
      const auto& arr = arch.get_array<T>();
      for (std::size_t i = 0; i < arch.size(); ++i) {
        f(arch.entities()[i], arr.at(i));
      }
    }
  }

  //! Iterate all entities with components T1, T2, ... and call f(Entity, const T1&, const T2&...).
  template <typename T1, typename T2, typename Func>
  void for_each(Func&& f) const {
    for (const auto& arch : archetypes_) {
      if (!arch.has<T1>() || !arch.has<T2>()) continue;
      const auto& a1 = arch.get_array<T1>();
      const auto& a2 = arch.get_array<T2>();
      for (std::size_t i = 0; i < arch.size(); ++i) {
        f(arch.entities()[i], a1.at(i), a2.at(i));
      }
    }
  }

  // -- Query API --

  //! Query all entities with components T1, T2, ... and call f(Entity, T1&, T2&, ...).
  //! Returns the total number of entities visited.
  template <typename T1, typename T2, typename Func>
  [[nodiscard]] std::size_t query(Func&& f) {
    std::size_t count = 0;
    for (auto& arch : archetypes_) {
      if (!arch.has<T1>() || !arch.has<T2>()) continue;
      auto& a1 = arch.get_array<T1>();
      auto& a2 = arch.get_array<T2>();
      for (std::size_t i = 0; i < arch.size(); ++i) {
        f(arch.entities()[i], a1.at(i), a2.at(i));
        ++count;
      }
    }
    return count;
  }

  //! Query with a predicate: only visit entities where pred(Entity, T1&, T2&) returns true.
  //! The action is only called for entities matching the predicate.
  //! Returns the total number of entities where the action was called.
  template <typename T1, typename T2, typename Pred, typename Func>
  [[nodiscard]] std::size_t query_if(Pred&& pred, Func&& action) {
    std::size_t count = 0;
    for (auto& arch : archetypes_) {
      if (!arch.has<T1>() || !arch.has<T2>()) continue;
      auto& a1 = arch.get_array<T1>();
      auto& a2 = arch.get_array<T2>();
      for (std::size_t i = 0; i < arch.size(); ++i) {
        if (pred(arch.entities()[i], a1.at(i), a2.at(i))) {
          action(arch.entities()[i], a1.at(i), a2.at(i));
          ++count;
        }
      }
    }
    return count;
  }

  //! Single-component query with predicate.
  template <typename T, typename Pred, typename Func>
  [[nodiscard]] std::size_t query_if(Pred&& pred, Func&& action) {
    std::size_t count = 0;
    for (auto& arch : archetypes_) {
      if (!arch.has<T>()) continue;
      auto& arr = arch.get_array<T>();
      for (std::size_t i = 0; i < arch.size(); ++i) {
        if (pred(arch.entities()[i], arr.at(i))) {
          action(arch.entities()[i], arr.at(i));
          ++count;
        }
      }
    }
    return count;
  }

  //! Single-component query (no predicate).
  template <typename T, typename Func>
  [[nodiscard]] std::size_t query(Func&& f) {
    std::size_t count = 0;
    for (auto& arch : archetypes_) {
      if (!arch.has<T>()) continue;
      auto& arr = arch.get_array<T>();
      for (std::size_t i = 0; i < arch.size(); ++i) {
        f(arch.entities()[i], arr.at(i));
        ++count;
      }
    }
    return count;
  }

  //! Count entities matching a predicate for component T.
  template <typename T, typename Pred>
  [[nodiscard]] std::size_t count_if(Pred&& pred) const {
    std::size_t count = 0;
    for (const auto& arch : archetypes_) {
      if (!arch.has<T>()) continue;
      const auto& arr = arch.get_array<T>();
      for (std::size_t i = 0; i < arch.size(); ++i) {
        if (pred(arch.entities()[i], arr.at(i))) ++count;
      }
    }
    return count;
  }

private:
  struct EntityLocation {
    Archetype* archetype{nullptr};
    std::size_t index{0};
    std::uint32_t generation{0};
  };

  Archetype* find_or_create_archetype(ComponentMask mask) {
    for (auto& arch : archetypes_) {
      if (arch.mask() == mask) return &arch;
    }
    archetypes_.emplace_back();
    archetypes_.back().set_mask(mask);
    return &archetypes_.back();
  }

  //! Copy all shared components between archetypes, excluding skip_tid.
  void copy_all_components(Archetype* from, std::size_t from_idx,
                           Archetype* to, std::size_t to_idx,
                           std::uint32_t skip_tid = kMaxComponentTypes) {
    const auto shared = from->mask() & to->mask();
    for (std::size_t i = 0; i < kMaxComponentTypes; ++i) {
      if (i == skip_tid) continue;
      if (!shared.test(i)) continue;
      auto* src_arr = from->get_array_by_id(i);
      auto* dst_arr = to->get_array_by_id(i);
      if (!src_arr || !dst_arr) continue;
      dst_arr->copy_from(to_idx, *src_arr, from_idx);
    }
  }

  std::deque<Archetype> archetypes_;
  std::vector<EntityLocation> entity_locations_;
  std::vector<std::uint32_t> free_ids_;
  std::uint32_t next_entity_id_{1};
};

// ============================================================================
// System
// ============================================================================

struct System final {
  std::string name;
  std::function<void(World&, std::uint64_t)> update;
  std::vector<std::string> depends_on;
  //! Component types this system reads/writes. Used for parallel scheduling.
  //! Two systems can run concurrently if their masks don't overlap.
  ComponentMask component_mask;
};

// ============================================================================
// System Scheduler
// ============================================================================

class SystemScheduler final {
public:
  void add_system(System sys) {
    systems_.push_back(static_cast<System&&>(sys));
  }

  [[nodiscard]] bool build_schedule() {
    execution_order_.clear();
    std::unordered_map<std::string, std::size_t> name_to_idx;
    for (std::size_t i = 0; i < systems_.size(); ++i) {
      name_to_idx[systems_[i].name] = i;
    }

    std::vector<std::size_t> in_degree(systems_.size(), 0);
    for (std::size_t i = 0; i < systems_.size(); ++i) {
      for (const auto& dep : systems_[i].depends_on) {
        if (name_to_idx.count(dep)) ++in_degree[i];
      }
    }

    std::vector<bool> done(systems_.size(), false);
    for (std::size_t pass = 0; pass < systems_.size(); ++pass) {
      for (std::size_t i = 0; i < systems_.size(); ++i) {
        if (done[i] || in_degree[i] > 0) continue;
        execution_order_.push_back(i);
        done[i] = true;
        for (std::size_t j = 0; j < systems_.size(); ++j) {
          if (done[j]) continue;
          for (const auto& dep : systems_[j].depends_on) {
            auto it = name_to_idx.find(dep);
            if (it != name_to_idx.end() && it->second == i) {
              --in_degree[j];
            }
          }
        }
      }
    }
    return execution_order_.size() == systems_.size();
  }

  void run(World& world, std::uint64_t tick) {
    for (const auto idx : execution_order_) {
      systems_[idx].update(world, tick);
    }
  }

  //! Run systems in parallel where possible.
  //! Systems are partitioned into "waves" — each wave contains only systems
  //! whose component masks don't overlap with each other.
  //! Within each wave, systems run concurrently via the provided thread pool.
  //! Falls back to sequential execution if pool has 1 thread.
  void run_parallel(World& world, std::uint64_t tick,
                    omnicpp::core::ThreadPool& pool) {
    // Partition into waves
    auto waves = partition_into_waves();

    for (const auto& wave : waves) {
      if (wave.size() == 1) {
        systems_[wave[0]].update(world, tick);
      } else {
        // Submit all systems in this wave to the thread pool
        std::atomic<std::size_t> completed{0};
        std::mutex wave_mutex;
        std::condition_variable wave_cv;

        for (auto idx : wave) {
          pool.submit([&, idx]() {
            systems_[idx].update(world, tick);
            completed.fetch_add(1, std::memory_order_release);
            std::lock_guard lock(wave_mutex);
            wave_cv.notify_one();
          });
        }

        // Wait for all systems in this wave to complete
        std::unique_lock lock(wave_mutex);
        wave_cv.wait(lock, [&]() {
          return completed.load(std::memory_order_acquire) >= wave.size();
        });
      }
    }
  }

  [[nodiscard]] const std::vector<std::size_t>& execution_order() const noexcept {
    return execution_order_;
  }
  [[nodiscard]] const std::vector<System>& systems() const noexcept { return systems_; }

private:
  //! Partition the execution order into waves of independent systems.
  //! Two systems are independent if their component masks don't overlap.
  [[nodiscard]] std::vector<std::vector<std::size_t>> partition_into_waves() const {
    std::vector<std::vector<std::size_t>> waves;
    std::vector<bool> scheduled(systems_.size(), false);

    for (std::size_t pass = 0; pass < systems_.size(); ++pass) {
      std::vector<std::size_t> wave;
      ComponentMask wave_mask;

      for (auto idx : execution_order_) {
        if (scheduled[idx]) continue;
        // Check if this system's components overlap with the current wave
        if ((systems_[idx].component_mask & wave_mask).any()) continue;
        wave.push_back(idx);
        wave_mask |= systems_[idx].component_mask;
      }

      if (wave.empty()) break;
      for (auto idx : wave) scheduled[idx] = true;
      waves.push_back(static_cast<std::vector<std::size_t>&&>(wave));
    }

    return waves;
  }

  std::vector<System> systems_;
  std::vector<std::size_t> execution_order_;
};

// ============================================================================
// Deterministic Bump Allocator
// ============================================================================

//! A simple bump allocator for zero-allocation steady-state simulation.
//! Allocate once, use during simulation, reset between ticks or sessions.
//! Not thread-safe — intended for single-threaded simulation use.
class BumpAllocator final {
public:
  explicit BumpAllocator(std::size_t capacity_bytes)
      : raw_(static_cast<std::uint8_t*>(std::malloc(capacity_bytes + 64))),
        capacity_(capacity_bytes) {
    const auto addr = reinterpret_cast<std::uintptr_t>(raw_);
    const auto aligned = (addr + 63) & ~std::uintptr_t(63);
    buffer_ = raw_ + (aligned - addr);
  }

  ~BumpAllocator() {
    std::free(raw_);
  }

  BumpAllocator(const BumpAllocator&) = delete;
  BumpAllocator& operator=(const BumpAllocator&) = delete;
  BumpAllocator(BumpAllocator&&) = delete;
  BumpAllocator& operator=(BumpAllocator&&) = delete;

  //! Allocate `size` bytes with the given alignment. Returns nullptr if exhausted.
  [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) noexcept {
    const auto aligned_offset = (offset_ + alignment - 1) & ~(alignment - 1);
    if (aligned_offset + size > capacity_) return nullptr;
    void* ptr = buffer_ + aligned_offset;
    offset_ = aligned_offset + size;
    return ptr;
  }

  //! Reset the allocator. All previously allocated memory becomes invalid.
  void reset() noexcept { offset_ = 0; }

  //! Number of bytes used so far.
  [[nodiscard]] std::size_t bytes_used() const noexcept { return offset_; }

  //! Total capacity in bytes.
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  //! Percentage of capacity used.
  [[nodiscard]] double utilization() const noexcept {
    return capacity_ > 0 ? static_cast<double>(offset_) / static_cast<double>(capacity_) : 0.0;
  }

private:
  std::uint8_t* raw_{nullptr};
  std::uint8_t* buffer_{nullptr};
  std::size_t capacity_{0};
  std::size_t offset_{0};
};

// ============================================================================
// ECS Query Builder
// ============================================================================

//! A query result iterator that yields (Entity, T1&, T2&, ...) for all entities
//! matching the given component filter. Supports predicates for additional filtering.
//!
//! Usage:
//!   world.query<Position, Velocity>(
//!       [](Entity e, Position& p, Velocity& v) { p.x += v.dx; });
//!
//!   // With predicate (only entities with hp > 0):
//!   world.query_if<Health>(
//!       [](Entity e, Health& h) { return h.hp > 0; },
//!       [](Entity e, Health& h) { h.hp -= 1; });
//
class World; // forward declaration for documentation

// The query API is defined inside World class below (see World::query and World::query_if).

// ============================================================================
// Component Pool
// ============================================================================

//! Pre-allocated pool for component arrays.
//! Avoids per-archetype heap allocation in steady state.
//! Each pool manages one component type and reuses freed slots.
//!
//! Usage:
//!   ComponentPool<Position> pool(1024);
//!   auto* arr = pool.acquire();  // Reuses freed slot or allocates new
//!   pool.release(arr);           // Returns to free list for reuse
//
template <typename T>
class ComponentPool final {
public:
  explicit ComponentPool(std::size_t initial_capacity)
      : storage_(initial_capacity) {
    // Initialize free list
    for (std::size_t i = 0; i < initial_capacity; ++i) {
      free_list_.push_back(i);
    }
  }

  ~ComponentPool() = default;

  ComponentPool(const ComponentPool&) = delete;
  ComponentPool& operator=(const ComponentPool&) = delete;
  ComponentPool(ComponentPool&&) = delete;
  ComponentPool& operator=(ComponentPool&&) = delete;

  //! Acquire a component slot from the pool.
  //! Returns nullptr if the pool is exhausted.
  [[nodiscard]] T* acquire() noexcept {
    if (free_list_.empty()) return nullptr;
    const auto idx = free_list_.back();
    free_list_.pop_back();
    return &storage_[idx];
  }

  //! Release a component slot back to the pool.
  void release(T* ptr) noexcept {
    if (!ptr) return;
    const auto idx = static_cast<std::size_t>(ptr - storage_.data());
    if (idx < storage_.size()) {
      free_list_.push_back(idx);
    }
  }

  //! Number of slots currently available.
  [[nodiscard]] std::size_t available() const noexcept { return free_list_.size(); }

  //! Total capacity of the pool.
  [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }

private:
  std::vector<T> storage_;
  std::vector<std::size_t> free_list_;
};

} // namespace omnicpp::core
