//! @file command_recorder.cpp
//! @brief warploom-replay-v1 writer (docs/replay-format.md is the contract).

#include "warploom/core/command_recorder.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "warploom/core/replay_scrubber.hpp"

namespace warploom::editor {

namespace {

using CK = ::warploom::core::ControlCommand::Kind;

//! The recorded-kinds set: every mutating kind + scrub + pause/resume/step.
//! Queries, host-mirrored visual state, and the capture commands themselves
//! are excluded (docs/replay-format.md, "Recorded kinds").
[[nodiscard]] bool is_recorded(CK kind) noexcept {
  switch (kind) {
    // v1.9: host-owned visual stack selection, deliberately not recorded
    // (same class as SetCamera/SetSun).
    case CK::SetRenderMode:
    case CK::GetRenderMode:
    case CK::SetExposure:
    case CK::SetBloom:
      return false;
    case CK::SpawnCube:
    case CK::SetProperty:
    case CK::DestroyObject:
    case CK::Undo:
    case CK::Redo:
    case CK::Select:
    case CK::NodeAdd:
    case CK::NodeRemove:
    case CK::LinkNodes:
    case CK::UnlinkNodes:
    case CK::SetNodeParam:
    case CK::SetNodePosition:
    case CK::BindNodeProperty:
    case CK::UnbindNodeProperty:
    case CK::Pause:
    case CK::Resume:
    case CK::Step:
    case CK::ScrubStart:
    case CK::ScrubTo:
    case CK::ClipAdd:
    case CK::ClipRemove:
    case CK::ClipMove:
    case CK::ClipRecord:
    case CK::ClipRecordStop:
    case CK::ClipPlay:
    case CK::ClipStop:
      return true;
    default:
      return false;
  }
}

//! JSON string escaping for the replay file: the two mandatory escapes plus
//! \n (command payloads can carry it) — same discipline as the protocol's
//! own json_escape.
void append_json_string(const std::string& value, std::string& out) {
  out += '"';
  for (const char c : value) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  out += '"';
}

}  // namespace

bool CommandRecorder::start(std::uint64_t frame, const SceneDocument& doc,
                            const std::string& scene, std::string& error) {
  if (active_) {
    error = "capture already active";
    return false;
  }
  active_ = true;
  frame_ = frame;
  scene_ = scene;
  log_.clear();
  checkpoints_.clear();
  embed_checkpoint(doc, frame);
  return true;
}

void CommandRecorder::record(const ::warploom::core::ControlCommand& command) {
  if (!active_ || !is_recorded(command.kind)) {
    return;
  }
  if (command.kind == CK::Step) {
    const std::uint64_t ticks =
        command.number_count > 0U
            ? static_cast<std::uint64_t>(command.numbers[0])
            : 1U;
    frame_ += ticks;  // logical frame advances before stamping (spec)
    if (ticks == 0U) {
      return;  // a zero-tick step has no effect; don't record it
    }
  }
  log_.push_back(RecordedCommand{frame_, command});
}

void CommandRecorder::embed_checkpoint(const SceneDocument& doc,
                                       std::uint64_t frame) {
  CheckpointLine line;
  line.frame = frame;
  line.json = doc.to_json();
  line.hash = fnv1a64(line.json.data(), line.json.size());
  checkpoints_.push_back(std::move(line));
}

bool CommandRecorder::stop(const std::string& path, const SceneDocument& doc,
                           std::string& error) {
  if (!active_) {
    error = "no capture active";
    return false;
  }
  if (path.empty()) {
    error = "stop_capture needs \"path\"";
    return false;
  }
  embed_checkpoint(doc, frame_);

  std::string out;
  out.reserve(4096U + log_.size() * 64U);
  // Header.
  out += "{\"record\":\"header\",\"schema_version\":1,"
         "\"format\":\"warploom-replay-v1\",\"scene\":";
  append_json_string(scene_, out);
  out += ",\"created_unix\":0}\n";

  // Interleave by logical frame: commands and checkpoint markers in file
  // order — checkpoints carry the frame they were embedded at, commands
  // their arrival stamp; ties resolve commands-first at the same frame
  // (both orders reconstruct identical state; see spec).
  std::size_t ci = 0;
  std::size_t li = 0;
  std::size_t seq = 0;
  while (li < log_.size() || ci < checkpoints_.size()) {
    const bool take_cmd =
        ci >= checkpoints_.size() ||
        (li < log_.size() &&
         (log_[li].frame <= checkpoints_[ci].frame));
    if (take_cmd) {
      const RecordedCommand& r = log_[li++];
      out += "{\"record\":\"cmd\",\"frame\":";
      out += std::to_string(r.frame);
      out += ",\"seq\":";
      out += std::to_string(seq++);
      out += ",\"cmd\":";
      append_json_string(
          ::warploom::core::ControlCommand::kind_name(r.command.kind), out);
      bool any_num = false;
      for (std::uint32_t i = 0; i < r.command.number_count; ++i) {
        // Both operands must be const char* — a char/string-literal mix
        // would resolve the ternary to bool (pointer->bool) and corrupt
        // the record.
        out += any_num ? "," : ",\"n\":[";
        any_num = true;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", r.command.numbers[i]);
        out += buf;
      }
      if (any_num) out += ']';
      if (!r.command.text.empty()) {
        out += ",\"t\":";
        append_json_string(r.command.text, out);
      }
      if (!r.command.text2.empty()) {
        out += ",\"t2\":";
        append_json_string(r.command.text2, out);
      }
      if (!r.command.text3.empty()) {
        out += ",\"t3\":";
        append_json_string(r.command.text3, out);
      }
      out += "}\n";
    } else {
      const CheckpointLine& c = checkpoints_[ci++];
      out += "{\"record\":\"ckpt\",\"frame\":";
      out += std::to_string(c.frame);
      out += ",\"hash\":\"";
      out += std::to_string(c.hash);
      out += "\"}\n";
      out += c.json;
      out += '\n';
    }
  }

  // End record (required; a file without it is truncated and rejected).
  out += "{\"record\":\"end\",\"frame\":";
  out += std::to_string(frame_);
  out += ",\"commands\":";
  out += std::to_string(log_.size());
  out += ",\"checkpoints\":";
  out += std::to_string(checkpoints_.size());
  out += "}\n";

  // Atomic write: tmp in the same directory + rename; 0600 (G1 discipline).
  const std::string tmp = path + ".tmp." + std::to_string(::getpid());
  {
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      error = "capture: cannot open \"" + tmp + "\": " +
              std::strerror(errno);
      return false;
    }
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    file.close();
    if (!file.good()) {
      error = "capture: write failed for \"" + tmp + "\": " +
              std::strerror(errno);
      std::remove(tmp.c_str());
      return false;
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    error =
        "capture: rename failed: " + std::string(std::strerror(errno));
    std::remove(tmp.c_str());
    return false;
  }
  (void)::chmod(path.c_str(), 0600);

  active_ = false;
  log_.clear();
  checkpoints_.clear();
  return true;
}

}  // namespace warploom::editor
