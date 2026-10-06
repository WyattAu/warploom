//! @file input_state.cpp
//! @brief Virtual input driver script loading (kept out of the header so the
//!        JSONL parser stays out of hot paths).

#include "warploom/core/input_state.hpp"

#include "warploom/core/numeric_cast.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace warploom::core {

namespace {

//! Minimal extraction helpers for flat JSON event objects. Input scripts are
//! flat one-line objects written by tools/humans, so targeted field scans
//! keep this dependency-free and strict about the exact accepted shape.

[[nodiscard]] bool find_number(const std::string& line, const char* key,
                               double& out) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const std::size_t key_pos = line.find(needle);
  if (key_pos == std::string::npos) return false;
  const std::size_t colon = line.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return false;
  try {
    std::size_t consumed = 0;
    out = std::stod(line.substr(colon + 1), &consumed);
    return consumed > 0U;
  } catch (const std::exception&) {
    return false;
  }
}

[[nodiscard]] bool find_string(const std::string& line, const char* key,
                               std::string& out) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const std::size_t key_pos = line.find(needle);
  if (key_pos == std::string::npos) return false;
  const std::size_t colon = line.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return false;
  const std::size_t open = line.find('"', colon + 1);
  if (open == std::string::npos) return false;
  const std::size_t close = line.find('"', open + 1);
  if (close == std::string::npos) return false;
  out = line.substr(open + 1, close - open - 1);
  return true;
}

}  // namespace

bool VirtualInputDriver::load_script(const std::string& path,
                                     std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "cannot open input script " + path;
    return false;
  }
  std::size_t line_number = 0;
  for (std::string line; std::getline(file, line);) {
    ++line_number;
    const auto first_char =
        line.find_first_not_of(" \t\r\n");
    if (first_char == std::string::npos || line[first_char] == '#') {
      continue;
    }
    Event event;
    double tick = 0.0;
    std::uint64_t parsed_tick = 0;
    if (!find_number(line, "tick", tick) ||
        !::warploom::core::checked_double_to_uint64(tick, parsed_tick)) {
      error = "input script line " + std::to_string(line_number) +
              ": tick must be a non-negative integer";
      return false;
    }
    event.tick = parsed_tick;
    std::string action;
    std::string axis;
    const bool has_action = find_string(line, "action", action);
    const bool has_axis = find_string(line, "axis", axis);
    if (has_action == has_axis) {
      error = "input script line " + std::to_string(line_number) +
              ": exactly one of \"action\" or \"axis\" is required";
      return false;
    }
    double value = 0.0;
    if (!find_number(line, "value", value)) {
      error = "input script line " + std::to_string(line_number) +
              ": value must be a number";
      return false;
    }
    event.value = static_cast<float>(value);
    if (has_action) {
      if (action.empty()) {
        error = "input script line " + std::to_string(line_number) +
                ": action name must not be empty";
        return false;
      }
      event.name = action;
      event.is_axis = false;
    } else {
      if (axis.empty()) {
        error = "input script line " + std::to_string(line_number) +
                ": axis name must not be empty";
        return false;
      }
      event.name = axis;
      event.is_axis = true;
    }
    events_.push_back(std::move(event));
  }
  sorted_ = false;
  return true;
}

}  // namespace warploom::core
