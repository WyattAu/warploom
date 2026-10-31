  bias are now separate parameters, and the viewport's shadow pass; which had
  no bias at all; records one.
- `.clang-format` could not be parsed; `[[nodiscard]]` was unquoted, which
  YAML reads as a nested sequence; so `format-check` had never run.