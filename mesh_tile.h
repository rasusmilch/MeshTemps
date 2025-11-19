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

    // True if the node's sequence number has not advanced for an
    // extended period while messages are still being received.
    bool seq_stuck = false;

    // Display units.
    bool display_fahrenheit = true;

    // Up to two sensors per tile.
    int sensor_count = 0;
    SensorView sensors[2];

    // UI toggles from root.
    bool show_sensor_labels = true;  // show/hide sensor name text
    bool show_age = true;            // show/hide "Age: N min"
  };

  MeshTile(lv_obj_t* parent,
           lv_coord_t width,
           lv_coord_t height);

  // Resize tile when layout changes.
  void SetSize(lv_coord_t width, lv_coord_t height);

  // Full content update (title, age, sensors, colors/highlights).
  void SetContent(const Content& content);

  // Cheap "age only" update used on minute ticks.
  void SetAgeOnly(uint32_t age_minutes,
                  bool is_missing,
                  bool is_stale);

  // Set flashing interval for warning/alert tiles (0 = off).
  void SetFlashIntervalMs(uint32_t interval_ms);

  lv_obj_t* root() const { return root_; }

  // Per-tile time-based update. Call once per loop() with millis().
  void Loop(uint32_t now_ms);

 private:
  void InitWidgets(lv_obj_t* parent,
                   lv_coord_t width,
                   lv_coord_t height);
  void UpdateBaseColors();
  void UpdateTextsAndLayout();
  void ApplyBaseColors();
  void ApplyFlashPhase(bool flash_on);

  lv_obj_t* root_ = nullptr;
  lv_obj_t* label_loc_ = nullptr;
  lv_obj_t* label_age_ = nullptr;

  // Per-sensor name + temperature labels (two "slots" per tile).
  lv_obj_t* sensor_name_label_[2] = {nullptr, nullptr};
  lv_obj_t* sensor_temp_label_[2] = {nullptr, nullptr};

  // Cached colors for current content.
  lv_color_t bg_normal_;
  lv_color_t fg_normal_;
  lv_color_t bg_flash_;
  lv_color_t fg_flash_;
  lv_color_t sensor_color_[2];

  Content content_;

  bool last_flash_active_ = false;
  uint32_t flash_interval_ms_ = 0;
  uint32_t last_flash_toggle_ms_ = 0;
};

#endif  // MESH_TILE_H_
