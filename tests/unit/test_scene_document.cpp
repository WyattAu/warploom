//! @file test_scene_document.cpp
//! @brief M1 editor-foundation proofs: byte-deterministic serialization,
//!        strict parsing with byte-offset diagnostics, full undo/redo
//!        round-trips (every apply followed by undo restores byte-identical
//!        state), registry typing, the M0→document bridge, and replay
//!        determinism (identical command sequences → identical bytes).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>

#include "warploom/core/document.hpp"
#include "warploom/core/property_registry.hpp"

namespace {

using namespace omnicpp::editor;

// ----------------------------------------------------------------------------
// Test fixtures
// ----------------------------------------------------------------------------

//! A minimal document with the singleton environment + one cube.
SceneDocument make_seed_document() {
  SceneDocument doc;
  const auto& registry = default_registry();
  const auto* env = registry.find_by_name(kTypeEnvironment);
  const auto* cube = registry.find_by_name(kTypeCube);
  SceneObject environment;
  environment.id = kEnvironmentObjectId;
  environment.type_id = env->id;
  environment.name = "environment";
  for (const auto& prop : env->properties) {
    environment.properties.emplace(prop.name, prop.default_value);
  }
  doc.next_object_id = kEnvironmentObjectId + 1U;  // ids after the singleton
  SceneObject box;
  box.id = doc.next_object_id++;
  box.type_id = cube->id;
  box.name = "crate";
  for (const auto& prop : cube->properties) {
    box.properties.emplace(prop.name, prop.default_value);
  }
  doc.objects.push_back(std::move(environment));
  doc.objects.push_back(std::move(box));
  return doc;
}

//! Builds a spawn command for object id `id` from registry defaults.
std::unique_ptr<Command> make_spawn(std::uint64_t id) {
  const auto* cube = default_registry().find_by_name(kTypeCube);
  std::map<std::string, PropValue> props;
  for (const auto& prop : cube->properties) {
    props.emplace(prop.name, prop.default_value);
  }
  return std::make_unique<SpawnObjectCommand>(id, cube->id, "spawned_" + std::to_string(id),
                                              std::move(props));
}

// ----------------------------------------------------------------------------
// Serialization
// ----------------------------------------------------------------------------

TEST(SceneDocument, SerializeIsByteDeterministic) {
  SceneDocument doc = make_seed_document();
  const std::string a = doc.to_json();
  // Same content, independently constructed document → identical bytes.
  SceneDocument doc2 = make_seed_document();
  const std::string b = doc2.to_json();
  EXPECT_EQ(a, b);
  // Serialization twice in a row is also identical.
  EXPECT_EQ(a, doc.to_json());
}

TEST(SceneDocument, RoundTripPreservesBytes) {
  SceneDocument doc = make_seed_document();
  const std::string original = doc.to_json();
  SceneDocument parsed;
  std::string error;
  ASSERT_TRUE(SceneDocument::from_json(original, parsed, error)) << error;
  EXPECT_EQ(parsed.to_json(), original);
}

TEST(SceneDocument, RoundTripPreservesAllValueTypes) {
  SceneDocument doc;
  doc.objects.push_back(SceneObject{});
  auto& object = doc.objects.back();
  object.id = 7;
  object.type_id = 2;
  object.name = "types";
  object.properties.emplace("n", PropValue::make_number(0.1 + 0.2));
  object.properties.emplace("b", PropValue::make_bool(true));
  object.properties.emplace("s", PropValue::make_string("hi \"quoted\"\n"));
  object.properties.emplace("v", PropValue::make_vec3(1.5, -2.25, 3.0));

  const std::string text = doc.to_json();
  SceneDocument parsed;
  std::string error;
  ASSERT_TRUE(SceneDocument::from_json(text, parsed, error)) << error;
  EXPECT_EQ(parsed.to_json(), text);
  const auto& props = parsed.objects[0].properties;
  EXPECT_DOUBLE_EQ(props.at("n").number, 0.1 + 0.2);
  EXPECT_TRUE(props.at("b").boolean);
  EXPECT_EQ(props.at("s").text, "hi \"quoted\"\n");
  EXPECT_DOUBLE_EQ(props.at("v").vec[0], 1.5);
  EXPECT_DOUBLE_EQ(props.at("v").vec[2], 3.0);
}

TEST(SceneDocument, RejectsMalformedWithByteOffset) {
  SceneDocument parsed;
  std::string error;
  EXPECT_FALSE(SceneDocument::from_json("{not json", parsed, error));
  EXPECT_NE(error.find("byte"), std::string::npos) << error;
  EXPECT_FALSE(SceneDocument::from_json(
      ("{\"schema_version\":" +
       std::to_string(omnicpp::editor::kDocumentSchemaVersion + 1) + "}")
          .c_str(),
      parsed, error));
  EXPECT_NE(error.find("newer than this build"), std::string::npos) << error;
  EXPECT_FALSE(
      SceneDocument::from_json("{\"schema_version\":1}", parsed, error));
  EXPECT_NE(error.find("missing required key"), std::string::npos) << error;
  // Duplicate keys rejected.
  EXPECT_FALSE(SceneDocument::from_json(
      "{\"schema_version\":1,\"schema_version\":1,\"next_object_id\":1,"
      "\"objects\":[]}",
      parsed, error));
  EXPECT_NE(error.find("duplicate"), std::string::npos) << error;
  // Unknown object-level key rejected.
  EXPECT_FALSE(SceneDocument::from_json(
      "{\"schema_version\":1,\"next_object_id\":1,\"objects\":[{\"id\":1,"
      "\"type_id\":0,\"name\":\"x\",\"properties\":{},\"evil\":1}]}",
      parsed, error));
  EXPECT_NE(error.find("unknown object key"), std::string::npos) << error;
  // Trailing garbage rejected.
  EXPECT_FALSE(SceneDocument::from_json(
      "{\"schema_version\":1,\"next_object_id\":1,\"objects\":[]} junk",
      parsed, error));
  EXPECT_NE(error.find("trailing"), std::string::npos) << error;
}

TEST(SceneDocument, StrictParserAcceptsVec3AndRejectsSwizzle) {
  const std::string good =
      "{\"schema_version\":1,\"next_object_id\":2,\"objects\":[{\"id\":1,"
      "\"type_id\":0,\"name\":\"e\",\"properties\":{\"p\":{\"type\":\"vec3\","
      "\"x\":1,\"y\":2,\"z\":3}}}]}";
  SceneDocument parsed;
  std::string error;
  ASSERT_TRUE(SceneDocument::from_json(good, parsed, error)) << error;
  EXPECT_DOUBLE_EQ(
      parsed.objects[0].properties.at("p").vec[1], 2.0);

  const std::string bad =
      "{\"schema_version\":1,\"next_object_id\":2,\"objects\":[{\"id\":1,"
      "\"type_id\":0,\"name\":\"e\",\"properties\":{\"p\":{\"type\":\"vec3\","
      "\"x\":1,\"z\":3,\"y\":2}}}]}";
  EXPECT_FALSE(SceneDocument::from_json(bad, parsed, error));
  EXPECT_NE(error.find("expected \"y\""), std::string::npos) << error;
}

// ----------------------------------------------------------------------------
// Commands + undo/redo
// ----------------------------------------------------------------------------

TEST(CommandStack, SetPropertyUndoRestoresByteIdentical) {
  SceneDocument doc = make_seed_document();
  CommandStack stack(doc);
  const std::string before = doc.to_json();

  std::string error;
  ASSERT_TRUE(stack.execute(std::make_unique<SetPropertyCommand>(
                                2, "color", PropValue::make_vec3(1.0, 0.0, 0.0)),
                            error))
      << error;
  EXPECT_NE(doc.to_json(), before);
  ASSERT_TRUE(stack.undo(error)) << error;
  EXPECT_EQ(doc.to_json(), before);
  ASSERT_TRUE(stack.redo(error)) << error;
  EXPECT_NE(doc.to_json(), before);
}

TEST(CommandStack, SetNewPropertyUndoRemovesIt) {
  SceneDocument doc = make_seed_document();
  CommandStack stack(doc);
  std::string error;
  ASSERT_TRUE(stack.execute(std::make_unique<SetPropertyCommand>(
                                2, "brand_new", PropValue::make_number(42.0)),
                            error))
      << error;
  ASSERT_TRUE(doc.find(2)->properties.contains("brand_new"));
  ASSERT_TRUE(stack.undo(error)) << error;
  EXPECT_FALSE(doc.find(2)->properties.contains("brand_new"));
}

TEST(CommandStack, SpawnDestroyRoundTripByteIdentical) {
  SceneDocument doc = make_seed_document();
  CommandStack stack(doc);
  const std::string before = doc.to_json();

  std::string error;
  ASSERT_TRUE(stack.execute(make_spawn(3), error)) << error;
  EXPECT_EQ(doc.next_object_id, 4U);
  ASSERT_TRUE(stack.execute(std::make_unique<DestroyObjectCommand>(3), error))
      << error;
  // next_object_id is a monotonic watermark: destroy does not lower it.
  EXPECT_EQ(doc.next_object_id, 4U);

  // Undo both: destroy-undo reinserts, spawn-undo lowers the watermark back
  // → byte-identical to `before` through the opposite path (exercises
  // index-restore and erase paths).
  ASSERT_TRUE(stack.undo(error)) << error;  // undo destroy
  ASSERT_TRUE(stack.undo(error)) << error;  // undo spawn
  EXPECT_EQ(doc.to_json(), before);
}

TEST(CommandStack, DestroyUndoPreservesPositionAndState) {
  SceneDocument doc = make_seed_document();
  // Make objects distinguishable.
  std::string error;
  CommandStack stack(doc);
  ASSERT_TRUE(stack.execute(std::make_unique<SetPropertyCommand>(
                                2, "color", PropValue::make_vec3(0.25, 0.5, 0.75)),
                            error))
      << error;

  // Destroy the FIRST object (environment) → index 0 captured.
  ASSERT_TRUE(stack.execute(std::make_unique<DestroyObjectCommand>(1), error))
      << error;
  ASSERT_EQ(doc.objects.size(), 1U);
  ASSERT_TRUE(stack.undo(error)) << error;
  ASSERT_EQ(doc.objects.size(), 2U);
  EXPECT_EQ(doc.objects[0].id, 1U);
  EXPECT_EQ(doc.objects[0].name, "environment");
  EXPECT_EQ(doc.objects[1].properties.at("color").vec[0], 0.25);
}

TEST(CommandStack, RedoBranchClearsOnNewExecute) {
  SceneDocument doc = make_seed_document();
  CommandStack stack(doc);
  std::string error;
  ASSERT_TRUE(stack.execute(
      std::make_unique<SetPropertyCommand>(2, "x", PropValue::make_number(1.0)),
      error));
  ASSERT_TRUE(stack.undo(error));
  EXPECT_EQ(stack.redo_count(), 1U);
  ASSERT_TRUE(stack.execute(
      std::make_unique<SetPropertyCommand>(2, "y", PropValue::make_number(2.0)),
      error));
  EXPECT_EQ(stack.redo_count(), 0U);
}

TEST(CommandStack, FailedApplyLeavesStackAndDocumentUntouched) {
  SceneDocument doc = make_seed_document();
  CommandStack stack(doc);
  const std::string before = doc.to_json();
  std::string error;
  EXPECT_FALSE(stack.execute(
      std::make_unique<SetPropertyCommand>(99, "x", PropValue::make_number(1.0)),
      error));
  EXPECT_NE(error.find("no object"), std::string::npos) << error;
  EXPECT_EQ(stack.undo_count(), 0U);
  EXPECT_EQ(doc.to_json(), before);
}

TEST(CommandStack, MacroCameraEditUndosAtomically) {
  SceneDocument doc = make_seed_document();
  const std::string before = doc.to_json();
  CommandStack stack(doc);
  std::string error;
  ASSERT_TRUE(stack.execute(std::make_unique<SetPropertiesCommand>(
                                kEnvironmentObjectId,
                                std::vector<std::pair<std::string, PropValue>>{
                                    {"camera_eye", PropValue::make_vec3(1, 2, 3)},
                                    {"camera_fov", PropValue::make_number(90.0)}}),
                            error))
      << error;
  ASSERT_TRUE(stack.undo(error)) << error;
  EXPECT_EQ(doc.to_json(), before);
}

TEST(CommandStack, ReplayIsByteDeterministic) {
  // Two independent stacks fed the identical command sequence must reach
  // byte-identical documents (the editor-replay proof).
  const auto run = [] {
    SceneDocument doc = make_seed_document();
    CommandStack stack(doc);
    std::string error;
    (void)stack.execute(std::make_unique<SetPropertyCommand>(
                            2, "color", PropValue::make_vec3(0.1, 0.2, 0.3)),
                        error);
    (void)stack.execute(make_spawn(5), error);
    (void)stack.execute(std::make_unique<SetPropertyCommand>(
                            5, "scale", PropValue::make_vec3(2, 2, 2)),
                        error);
    (void)stack.execute(std::make_unique<DestroyObjectCommand>(2), error);
    (void)stack.undo(error);
    (void)stack.execute(std::make_unique<SetPropertiesCommand>(
                            kEnvironmentObjectId,
                            std::vector<std::pair<std::string, PropValue>>{
                                {"camera_fov", PropValue::make_number(45.0)}}),
                        error);
    return doc.to_json();
  };
  EXPECT_EQ(run(), run());
}

// ----------------------------------------------------------------------------
// Property registry + bridge
// ----------------------------------------------------------------------------

TEST(PropertyRegistry, DefaultTypesRegisteredWithIds) {
  const auto& registry = default_registry();
  ASSERT_GE(registry.type_count(), 4U);
  const auto* env = registry.find_by_name(kTypeEnvironment);
  ASSERT_NE(env, nullptr);
  EXPECT_EQ(env->name, "environment");
  const auto* cube_again = registry.find_by_name(kTypeCube);
  ASSERT_NE(cube_again, nullptr);
  // Same lookup by id and by name resolves to the same type.
  EXPECT_EQ(registry.find(env->id), env);
  EXPECT_NE(registry.find(cube_again->id), nullptr);
  // Camera + sun properties exist with expected defaults.
  bool saw_fov = false;
  for (const auto& prop : env->properties) {
    if (prop.name == "camera_fov") {
      EXPECT_DOUBLE_EQ(prop.default_value.number, 60.0);
      saw_fov = true;
    }
  }
  EXPECT_TRUE(saw_fov);
}

TEST(Bridge, SpawnCubeUsesRegistryDefaultsPlusOverrides) {
  SceneDocument doc = make_seed_document();
  omnicpp::core::ControlCommand command;
  command.kind = omnicpp::core::ControlCommand::Kind::SpawnCube;
  command.numbers[0] = 1.0;
  command.numbers[1] = 2.0;
  command.numbers[2] = 3.0;
  command.numbers[3] = 0.5;  // size
  command.number_count = 4;

  auto outcome =
      bridge_control_command(command, doc, default_registry());
  ASSERT_EQ(outcome.kind, BridgeOutcome::Kind::Edited);

  CommandStack stack(doc);
  std::string error;
  ASSERT_TRUE(stack.execute(std::move(outcome.command), error)) << error;
  const SceneObject* spawned = doc.find(doc.next_object_id - 1);
  ASSERT_NE(spawned, nullptr);
  EXPECT_EQ(spawned->name, "cube_" + std::to_string(spawned->id));
  EXPECT_DOUBLE_EQ(spawned->properties.at("position").vec[0], 1.0);
  EXPECT_DOUBLE_EQ(spawned->properties.at("scale").vec[1], 0.5);
  // Untouched defaults survive: color comes from the registry.
  EXPECT_DOUBLE_EQ(spawned->properties.at("color").vec[2], 0.8);

  // Undo removes it entirely.
  ASSERT_TRUE(stack.undo(error)) << error;
  EXPECT_EQ(doc.find(spawned->id), nullptr);
}

TEST(Bridge, SetCameraIsOneAtomicEdit) {
  SceneDocument doc = make_seed_document();
  omnicpp::core::ControlCommand command;
  command.kind = omnicpp::core::ControlCommand::Kind::SetCamera;
  for (double v : {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 75.0}) {
    command.numbers[command.number_count++] = v;
  }
  auto outcome =
      bridge_control_command(command, doc, default_registry());
  ASSERT_EQ(outcome.kind, BridgeOutcome::Kind::Edited);

  CommandStack stack(doc);
  const std::string before = doc.to_json();
  std::string error;
  ASSERT_TRUE(stack.execute(std::move(outcome.command), error)) << error;
  const auto& props = doc.find(kEnvironmentObjectId)->properties;
  EXPECT_DOUBLE_EQ(props.at("camera_fov").number, 75.0);
  EXPECT_DOUBLE_EQ(props.at("camera_eye").vec[2], 3.0);
  ASSERT_TRUE(stack.undo(error)) << error;
  EXPECT_EQ(doc.to_json(), before);  // one undo step restored everything
}

TEST(Bridge, SessionCommandsProduceNoEdit) {
  SceneDocument doc = make_seed_document();
  for (const auto kind : {omnicpp::core::ControlCommand::Kind::Ping,
                          omnicpp::core::ControlCommand::Kind::Pause,
                          omnicpp::core::ControlCommand::Kind::Resume,
                          omnicpp::core::ControlCommand::Kind::Step,
                          omnicpp::core::ControlCommand::Kind::Capture}) {
    omnicpp::core::ControlCommand command;
    command.kind = kind;
    const auto outcome =
        bridge_control_command(command, doc, default_registry());
    EXPECT_EQ(outcome.kind, BridgeOutcome::Kind::SessionOnly)
        << static_cast<int>(kind);
  }
}

TEST(Bridge, RejectsInvalidPayloads) {
  SceneDocument doc = make_seed_document();
  omnicpp::core::ControlCommand command;
  command.kind = omnicpp::core::ControlCommand::Kind::SpawnCube;
  command.numbers[0] = 1.0;
  command.number_count = 1;  // position incomplete
  auto outcome = bridge_control_command(command, doc, default_registry());
  EXPECT_EQ(outcome.kind, BridgeOutcome::Kind::Rejected);

  command.kind = omnicpp::core::ControlCommand::Kind::SpawnCube;
  command.number_count = 4;
  command.numbers[3] = -1.0;  // bad size
  outcome = bridge_control_command(command, doc, default_registry());
  EXPECT_EQ(outcome.kind, BridgeOutcome::Kind::Rejected);

  command.kind = omnicpp::core::ControlCommand::Kind::SetSun;
  command.numbers[1] = std::nan("");
  command.number_count = 3;
  outcome = bridge_control_command(command, doc, default_registry());
  EXPECT_EQ(outcome.kind, BridgeOutcome::Kind::Rejected);
}

}  // namespace

// ----------------------------------------------------------------------------
// D2: interpolated timeline evaluation
// ----------------------------------------------------------------------------

namespace {

//! A two-key position track: sample 0 at x=0, sample 10 at x=100.
::omnicpp::editor::TimelineClip two_key_clip(std::uint64_t object_id) {
  ::omnicpp::editor::TimelineClip clip{};
  clip.id = 1U;
  clip.name = "move";
  clip.start_frame = 20U;
  clip.length_frames = 40U;
  clip.tracks[omnicpp::editor::track_key(object_id, "position")] =
      ::omnicpp::editor::ClipTrack{
          object_id, "position",
          {::omnicpp::editor::ClipSample{0U, PropValue::make_vec3(0, 0, 0)},
           ::omnicpp::editor::ClipSample{10U, PropValue::make_vec3(100, 0, 0)}}};
  return clip;
}

double sampled_x(const ::omnicpp::editor::TimelineClip& clip,
                 std::uint64_t object_id, std::uint64_t frame) {
  PropValue value{};
  if (!clip.evaluate(frame, object_id, "position", value)) return -1.0;
  return value.vec[0];
}

double interpolated_x(const ::omnicpp::editor::TimelineClip& clip,
                      std::uint64_t object_id, double frame_time,
                      ::omnicpp::editor::Easing easing) {
  PropValue value{};
  if (!clip.evaluate_at(frame_time, object_id, "position", easing, value)) {
    return -1.0;
  }
  return value.vec[0];
}

}  // namespace

TEST(SceneDocument, StepModeAtWholeFramesReproducesEvaluateExactly) {
  // The property that keeps existing recordings and replays valid: a whole
  // frame in Step mode must be indistinguishable from evaluate(). If it drifts
  // by even a float ULP, every previously recorded replay would re-run
  // differently from its original take.
  const auto clip = two_key_clip(1U);
  for (std::uint64_t frame = 18; frame <= 62; ++frame) {
    const double stepped = interpolated_x(clip, 1U, static_cast<double>(frame),
                                          omnicpp::editor::Easing::Step);
    const double held = sampled_x(clip, 1U, frame);
    ASSERT_EQ(stepped >= 0.0, held >= 0.0) << "disagreement on range at " << frame;
    EXPECT_DOUBLE_EQ(sampled_x(clip, 1U, frame), stepped)
        << "Step mode drifted from evaluate() at frame " << frame;
  }
}

TEST(SceneDocument, LinearInterpolationIsMonotonicBetweenKeys) {
  const auto clip = two_key_clip(1U);
  // Sample 0 sits at clip offset 0, sample 10 at offset 10; x runs 0 -> 100.
  EXPECT_NEAR(interpolated_x(clip, 1U, 20.0, omnicpp::editor::Easing::Linear),
              0.0, 1e-4);
  EXPECT_NEAR(interpolated_x(clip, 1U, 25.0, omnicpp::editor::Easing::Linear),
              50.0, 1e-4);
  EXPECT_NEAR(interpolated_x(clip, 1U, 30.0, omnicpp::editor::Easing::Linear),
              100.0, 1e-4);
  // Beyond the last key the final value holds rather than extrapolating.
  EXPECT_NEAR(interpolated_x(clip, 1U, 45.0, omnicpp::editor::Easing::Linear),
              100.0, 1e-4);

  double previous = -1.0;
  for (int i = 0; i <= 100; ++i) {
    const double x = interpolated_x(clip, 1U,
                                    20.0 + 10.0 * static_cast<double>(i) / 100.0,
                                    omnicpp::editor::Easing::Linear);
    ASSERT_GE(x, previous - 1e-6) << "linear interpolation went backwards";
    previous = x;
  }
}

TEST(SceneDocument, EasingEndpointsAreExactAndOrderingDiffers) {
  const auto clip = two_key_clip(1U);
  // Every mode must land exactly on the keys, or a clip stops short of its own
  // final recorded value.
  for (const auto easing : {omnicpp::editor::Easing::Linear,
                            omnicpp::editor::Easing::SmoothStep,
                            omnicpp::editor::Easing::EaseIn,
                            omnicpp::editor::Easing::EaseOut}) {
    EXPECT_NEAR(interpolated_x(clip, 1U, 20.0, easing), 0.0, 1e-6);
    EXPECT_NEAR(interpolated_x(clip, 1U, 30.0, easing), 100.0, 1e-6);
  }
  // Ease-in sits below linear at the midpoint; ease-out above it. If that
  // inverted, the names would be lying.
  const double linear = interpolated_x(clip, 1U, 25.0, omnicpp::editor::Easing::Linear);
  const double ease_in = interpolated_x(clip, 1U, 25.0, omnicpp::editor::Easing::EaseIn);
  const double ease_out = interpolated_x(clip, 1U, 25.0, omnicpp::editor::Easing::EaseOut);
  EXPECT_LT(ease_in, linear);
  EXPECT_GT(ease_out, linear);
}

TEST(SceneDocument, InterpolationRespectsTheClipRangeAndMissingTracks) {
  const auto clip = two_key_clip(1U);
  // Same half-open span as evaluate(): before the start and at/after the end.
  PropValue value{};
  EXPECT_FALSE(clip.evaluate_at(19.5, 1U, "position",
                                omnicpp::editor::Easing::Linear, value));
  EXPECT_FALSE(clip.evaluate_at(60.0, 1U, "position",
                                omnicpp::editor::Easing::Linear, value))
      << "the clip covers [20, 60), so 60 is outside";
  // Before the clip's first sample there is nothing to report.
  EXPECT_FALSE(clip.evaluate_at(20.0, 99U, "position",
                                omnicpp::editor::Easing::Linear, value));
  EXPECT_FALSE(clip.evaluate_at(25.0, 1U, "nonexistent",
                                omnicpp::editor::Easing::Linear, value));
}

TEST(SceneDocument, NonNumericChannelsHoldTheEarlierSample) {
  // There is no meaningful midpoint between two strings or two booleans, so
  // they must hold rather than blend into something that was never recorded.
  ::omnicpp::editor::TimelineClip clip{};
  clip.id = 2U;
  clip.start_frame = 0U;
  clip.length_frames = 10U;
  clip.tracks[omnicpp::editor::track_key(1U, "name")] =
      ::omnicpp::editor::ClipTrack{
          1U, "name",
          {::omnicpp::editor::ClipSample{0U, PropValue::make_string("a")},
           ::omnicpp::editor::ClipSample{10U, PropValue::make_string("b")}}};
  PropValue value{};
  ASSERT_TRUE(clip.evaluate_at(5.0, 1U, "name", omnicpp::editor::Easing::Linear,
                               value));
  EXPECT_EQ(value.type, PropValue::Type::String);
  EXPECT_EQ(value.text, "a");

  ::omnicpp::editor::TimelineClip flags{};
  flags.id = 3U;
  flags.start_frame = 0U;
  flags.length_frames = 10U;
  flags.tracks[omnicpp::editor::track_key(1U, "visible")] =
      ::omnicpp::editor::ClipTrack{
          1U, "visible",
          {::omnicpp::editor::ClipSample{0U, PropValue::make_bool(true)},
           ::omnicpp::editor::ClipSample{10U, PropValue::make_bool(false)}}};
  ASSERT_TRUE(flags.evaluate_at(5.0, 1U, "visible",
                                omnicpp::editor::Easing::Linear, value));
  EXPECT_EQ(value.type, PropValue::Type::Bool);
  EXPECT_TRUE(value.boolean);
}
