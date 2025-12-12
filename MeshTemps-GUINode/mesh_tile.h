#ifndef MESH_TILE_H_
#define MESH_TILE_H_

#include <Arduino.h>
#include <lvgl.h>
#include <map>

// Forward declaration so we can use it in Content.
class MeshTile {
 public:
  struct SensorView {
    String label;
    float temp_c = NAN;
    bool has_value = false;
    bool is_alert = false;
    bool is_warning = false;
  };

  struct Content {
    String title;

    // Node UI state.
    uint32_t age_minutes = 0;
    bool is_missing = false;
    bool is_stale = false;
    bool seq_stuck = false;

    bool node_has_alert = false;
    bool node_has_warning = false;

    // Display options.
    bool display_fahrenheit = true;
    bool show_sensor_labels = true;
    bool show_age = true;

    // Up to 2 sensors.
    int sensor_count = 0;
    SensorView sensors[2];
  };

  // Visitor function type for iterating all tiles.
  using VisitFunc = void (*)(MeshTile* tile, void* user_data);

  // ---- Static registry API (MeshTile is the sole owner of tiles) ----

  // Get or create the tile for a given node key (8-hex String).
  // Creates the LVGL object under |parent| if needed, and ensures size.
  static MeshTile* GetOrCreate(const String& node_key_hex,
                               lv_obj_t* parent,
                               lv_coord_t width,
                               lv_coord_t height);

  // Find an existing tile by node key; returns nullptr if not present.
  static MeshTile* Find(const String& node_key_hex);

  // Visit all tiles currently registered.
  static void ForEach(VisitFunc func, void* user_data);

  // Destroy tile for a given node, deleting its LVGL object.
  static void Destroy(const String& node_key_hex);

  // Destroy all tiles and clear registry.
  static void DestroyAll();

  // Iterate all tiles and advance their per-tile timers (flashing, etc.).
  static void LoopAll(uint32_t now_ms);


  // ---- Per-tile API ----

  MeshTile(lv_obj_t* parent, lv_coord_t width, lv_coord_t height);
  ~MeshTile();

  // Node key this tile represents (canonical 8-hex, e.g. "10000001").
  const String& node_key_hex() const { return node_key_hex_; }

  // Resize tile when layout changes.
  void SetSize(lv_coord_t width, lv_coord_t height);

  // Full content update (title, sensors, flags, etc.).
  void SetContent(const Content& content);

  // Cheap age-only update.
  void SetAgeOnly(uint32_t age_minutes, bool is_missing, bool is_stale);

  // Set flashing interval (0 = no flash).
  void SetFlashIntervalMs(uint32_t interval_ms);

  // Per-frame time-based behavior (flashing, etc.).
  void Loop(uint32_t now_ms);

  // Reorder within parent to the given child index if needed.
  // Uses lv_obj_move_to_index() but only when the index actually changes.
  void EnsureChildIndex(uint32_t desired_index);

  // Optional access to root LVGL object if needed.
  lv_obj_t* root() const { return root_; }

 private:
  // Internal registry.
  static std::map<String, MeshTile*>& Registry();

  // Set node key after construction.
  void SetNodeKey(const String& node_key_hex) { node_key_hex_ = node_key_hex; }

  void InitWidgets(lv_obj_t* parent, lv_coord_t width, lv_coord_t height);
  void UpdateBaseColors();
  void UpdateTextsAndLayout();
  void ApplyBaseColors();
  void MoveToForeground();

  // Legacy global flash hook (if something still calls ApplyFlashPhase).
  void ApplyFlashPhase(bool flash_on);

  // New per-tile flashing implementation.
  void UpdateFlashAppearance();

  String node_key_hex_;  // 8-hex node id this tile is tied to.

  lv_obj_t* root_ = nullptr;
  lv_obj_t* label_loc_ = nullptr;
  lv_obj_t* label_age_ = nullptr;
  lv_obj_t* sensor_name_label_[2] = {nullptr, nullptr};
  lv_obj_t* sensor_temp_label_[2] = {nullptr, nullptr};

  Content content_;

  lv_color_t bg_normal_;
  lv_color_t fg_normal_;
  lv_color_t bg_flash_;
  lv_color_t fg_flash_;
  lv_color_t sensor_color_[2];

  // State for content + flashing.
  bool has_content_ = false;
  bool last_flash_active_ = false;
  uint32_t flash_interval_ms_ = 0;
  uint32_t last_flash_toggle_ms_ = 0;
  bool flash_on_ = false;
};

#endif  // MESH_TILE_H_
