#pragma once

//! @file widget.hpp
//! @brief M2 UI toolkit foundation: retained widget tree + layout + paint.
//!
//! Design (mirrors the engine's data-oriented core):
//!   - Widgets live in a flat arena (`WidgetTree`); hierarchy is expressed
//!     with parent/first-child/next-sibling indices. Handles are plain
//!     indices — stable across unrelated inserts, cache-friendly to walk,
//!     and trivially serializable. No shared_ptr cycles, no incomplete-type
//!     vector pitfalls.
//!   - `compute_layout` is a pure function of (tree, viewport): stack
//!     layout (vertical/horizontal), padding, spacing, fixed sizes, flex
//!     grow, and cross-axis alignment. Deterministic: the same tree always
//!     produces the same rects.
//!   - `paint` emits a flat `PaintList` (rects + text runs) in root-first
//!     draw order. The list is renderer-agnostic: the software rasterizer
//!     consumes it for golden-image tests, the Vulkan viewport consumes it
//!     for live rendering. Text is monospace with caller-supplied metrics
//!     (the engine ships a bitmap font raster in M3; shaping is out of
//!     scope by design).
//!
//! Contracts (machine-checked in test_ui_widget.cpp):
//!   - layout then paint never reads uninitialized rects: every visible
//!     widget has w,h >= 0 computed by `compute_layout`.
//!   - remove() unlinks without shifting arena slots (indices stay valid).
//!   - Paint order is parent-before-child, children in insertion order.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace warploom::ui {

//! Sentinel widget handle.
inline constexpr std::uint32_t kInvalidWidget = 0xFFFFFFFFu;

enum class WidgetKind : std::uint8_t {
  Container,  //!< pure layout node (no own paint output)
  Panel,      //!< filled rectangle, optional border
  Label,      //!< single-line text
  Button,     //!< panel + centered label
  Checkbox,   //!< toggle square + label to the right
};

enum class LayoutMode : std::uint8_t {
  Stack,     //!< children laid out along one axis (direction)
  Absolute,  //!< children keep their own x/y/w/h (free canvas placement);
             //!< layout recurses but never repositions them
  Flow,      //!< wraps; reserved so the enum is stable
};

enum class StackDirection : std::uint8_t { Vertical, Horizontal };

//! Cross-axis alignment within the parent's content box.
enum class Align : std::uint8_t { Start, Center, End, Stretch };

//! Monospace text metrics (pixels). Defaults match the M3 bitmap font.
struct TextMetrics final {
  float char_width{8.0f};
  float line_height{16.0f};
};

//! One widget. Kind-specific fields are documented per kind; layout fields
//! apply to every kind (every widget is a layout node).
struct Widget final {
  WidgetKind kind{WidgetKind::Container};
  std::string name{};  //!< lookup key (find_by_name), need not be unique
  std::string text{};  //!< Label/Button text, Checkbox label

  std::uint32_t color{0xFFE0E0E0};      //!< fill/foreground, 0xAARRGGBB
  std::uint32_t text_color{0xFF101010};  //!< Label/Button/Checkbox text
  std::uint32_t border_color{0x00000000};  //!< 0 alpha = no border
  float border_width{1.0f};

  // --- layout inputs ---
  LayoutMode layout{LayoutMode::Stack};
  StackDirection direction{StackDirection::Vertical};
  float padding{0.0f};
  float spacing{0.0f};
  float fixed_w{0.0f};  //!< 0 = auto (content/flex determined)
  float fixed_h{0.0f};
  float flex_grow{0.0f};  //!< share of parent's leftover main-axis space
  Align cross_align{Align::Start};

  // --- state ---
  bool visible{true};
  bool checked{false};   //!< Checkbox
  float value{0.0f};     //!< Slider (M3); normalized 0..1

  // --- computed by compute_layout (not user input) ---
  float x{0.0f};
  float y{0.0f};
  float w{0.0f};
  float h{0.0f};
  std::uint32_t parent{kInvalidWidget};
  std::uint32_t first_child{kInvalidWidget};
  std::uint32_t next_sibling{kInvalidWidget};
  std::uint32_t last_child{kInvalidWidget};  //!< O(1) append

  [[nodiscard]] bool is_container() const noexcept {
    return kind == WidgetKind::Container;
  }
};

//! Flat widget arena. The root is the slot created by the constructor; the
//! tree is exactly the root's subtree (orphan slots from remove() are dead).
class WidgetTree final {
 public:
  WidgetTree();

  //! Appends `widget` as the last child of `parent`. Returns its handle.
  [[nodiscard]] std::uint32_t add(Widget widget, std::uint32_t parent);

  //! Unlinks `handle` from its parent/siblings (subtree stays attached to
  //! it; arena slot is kept so other handles never shift). False when the
  //! handle is invalid or is the root.
  [[nodiscard]] bool remove(std::uint32_t handle);

  [[nodiscard]] Widget& get(std::uint32_t handle) {
    return widgets_[handle];
  }
  [[nodiscard]] const Widget& get(std::uint32_t handle) const {
    return widgets_[handle];
  }

  [[nodiscard]] std::uint32_t root() const noexcept { return 0U; }
  [[nodiscard]] std::size_t size() const noexcept { return widgets_.size(); }
  [[nodiscard]] std::size_t alive_count() const;  //!< reachable from root

  //! First widget (pre-order) whose name matches, or kInvalidWidget.
  [[nodiscard]] std::uint32_t find_by_name(std::string_view name) const;

  //! Pre-order visitation: calls f(handle, widget) for every reachable
  //! widget, parent before children, children in insertion order.
  template <typename Func>
  void for_each(Func&& f) const {
    visit(root(), f);
  }

 private:
  template <typename Func>
  void visit(std::uint32_t handle, Func& f) const {
    f(handle, widgets_[handle]);
    for (std::uint32_t c = widgets_[handle].first_child; c != kInvalidWidget;
         c = widgets_[c].next_sibling) {
      visit(c, f);
    }
  }

  std::vector<Widget> widgets_{};
};

//! Computes x/y/w/h for every widget in the tree against a viewport of
//! (width, height). The root fills the viewport. Must run before paint.
void compute_layout(WidgetTree& tree, float width, float height,
                    const TextMetrics& metrics = {});

// ============================================================================
// Paint output
// ============================================================================

struct PaintRect final {
  float x{0.0f};
  float y{0.0f};
  float w{0.0f};
  float h{0.0f};
  std::uint32_t color{0xFFE0E0E0};
  std::uint32_t border_color{0x00000000};
  float border_width{1.0f};
};

struct PaintText final {
  float x{0.0f};
  float y{0.0f};  //!< top of the line box
  std::string text{};
  std::uint32_t color{0xFFE0E0E0};
};

//! Flat, ordered draw list (rects in paint order, then text runs — text is
//! drawn after rects so labels are never overdrawn by sibling panels).
struct PaintList final {
  std::vector<PaintRect> rects{};
  std::vector<PaintText> texts{};

  void clear() {
    rects.clear();
    texts.clear();
  }
};

//! Emits paint output for every visible widget (root-first). Panels/buttons/
//! checkboxes emit rects; labels/buttons/checkboxes emit text.
void paint(const WidgetTree& tree, PaintList& out,
           const TextMetrics& metrics = {});

}  // namespace warploom::ui
