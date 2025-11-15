#ifndef MESH_TILE_H_
#define MESH_TILE_H_

#include <Arduino.h>
#include <lvgl.h>

// A self-contained LVGL "status tile" widget used to display a single node.
//
// This class owns a root lv_obj_t and its child labels. It does not know
// anything about JSON, mesh, or thresholds. The caller supplies a Content
// snapshot, and the tile renders it (layout, text, colors, flashing).
class MeshTile {
 public:
  // Description of a single sensor line rendered on the tile.
  struct SensorView {
    String label;        // e.g. "Rack Inlet"
    float temp_c = NAN;  // raw °C value (tile will format using units flag)
    bool has_value = false;
    bool is_alert = false;
    bool is_warning = false;
  };

  // Snapshot of everything the tile needs to render itself.
  struct Content {
    String title;              // top line, e.g. "Server Room"
    uint32_t age_minutes = 0;  // "Age: N min"

    bool node_has_alert = false;
    bool node_has_warning = false;
    bool is_missing = false;
    bool is_stale = false;

    bool display_fahrenheit = true;  // true = °F, false = °C

    SensorView sensors[2];
    int sensor_count = 0;  // used entries in sensors[]
  };

  // Create a tile rooted under |parent| with a fixed size.
  //
  // Caller must ensure LVGL is initialized and any locking is handled before
  // constructing or calling methods on this object.
  MeshTile(lv_obj_t* parent, lv_coord_t width, lv_coord_t height);

  // Non-copyable (lv_obj_t owns resources).
  MeshTile(const MeshTile&) = delete;
  MeshTile& operator=(const MeshTile&) = delete;

  // Root LVGL object for this tile.
  lv_obj_t* root() const { return root_; }

  // Resize tile (e.g. after display rotation or resolution change).
  // Layout is recomputed using the current Content.
  void SetSize(lv_coord_t width, lv_coord_t height);

  // Provide a new content snapshot. The tile stores this and re-renders itself.
  //
  // This rewrites header, age, sensor lines, colors, and layout. It does not
  // perform any LVGL locking.
  void SetContent(const Content& content);

  // Cheap age-only update (avoid rebuilding sensor text elsewhere).
  //
  // This updates:
  //   - stored age minutes
  //   - missing/stale flags
  //   - age label text
  //   - base colors (bg/fg + sensor colors)
  //
  // It does *not* change sensor labels or severity flags; those remain from
  // the last SetContent() call.
  void SetAgeOnly(uint32_t age_minutes,
                  bool is_missing,
                  bool is_stale);

  // Apply the current global flash phase. |flash_on| should toggle at the
  // desired rate; the tile will only actually flash if it has alerts or
  // warnings.
  //
  // This does *not* manage timers; historically the caller drove it with a
  // global phase. In the new design, prefer calling Loop() with per-tile
  // timers instead.
  void ApplyFlashPhase(bool flash_on);

  // Set per-tile flash cadence (milliseconds). 0 disables flashing for this
  // tile even if it has alerts or warnings.
  void SetFlashIntervalMs(uint32_t interval_ms);
  uint32_t flash_interval_ms() const { return flash_interval_ms_; }

  // Call this once per main loop with millis(). The tile uses its own
  // flash interval and internal state to decide if it needs to repaint
  // itself (flash vs base colors).
  void Loop(uint32_t now_ms);

 private:
  void InitWidgets(lv_obj_t* parent, lv_coord_t width, lv_coord_t height);
  void UpdateBaseColors();
  void UpdateTextsAndLayout();
  void ApplyBaseColors();

  lv_obj_t* root_ = nullptr;
  lv_obj_t* label_loc_ = nullptr;
  lv_obj_t* label_age_ = nullptr;
  lv_obj_t* sensor_label_[2] = {nullptr, nullptr};

  Content content_{};

  lv_color_t bg_normal_;
  lv_color_t fg_normal_;
  lv_color_t bg_flash_;
  lv_color_t fg_flash_;
  lv_color_t sensor_color_[2];

  // True if the tile is currently using the "flash" palette; false means
  // it is using the base palette.
  bool last_flash_active_ = false;

  // Per-tile timing state for flashing.
  uint32_t flash_interval_ms_ = 0;       // 0 = no flashing
  uint32_t last_flash_toggle_ms_ = 0;    // millis() when phase last toggled
};

#endif  // MESH_TILE_H_
