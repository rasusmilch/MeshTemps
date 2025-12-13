#include "mesh_tile.h"
#include <Arduino.h> // NEW
#include <map>

extern bool g_debug_enabled; // defined in MeshTemps.ino

// Anonymous namespace for the registry backing store.
namespace {

// Layout / padding constants.
constexpr lv_coord_t kTilePadAll = 8;          // Inner padding of the tile.
constexpr lv_coord_t kTitleTopOffset = 0;      // Title Y offset from top.
constexpr lv_coord_t kAgeTopRightOffsetX = -6; // Age X offset from top-right.
constexpr lv_coord_t kAgeTopRightOffsetY = 4;  // Age Y offset from top-right.

// Bottom offsets for sensor labels.
constexpr lv_coord_t kTempBottomOffset = 0;   // Temp label from bottom.
constexpr lv_coord_t kNameBottomOffset = -32; // Name label from bottom.

static lv_color_t MakeColor(uint8_t r, uint8_t g, uint8_t b) {
  // For LVGL this is just a thin wrapper; adjust if you ever change color
  // depth.
  return lv_color_make(r, g, b);
}

std::map<String, MeshTile *> &TileRegistry() {
  // Lazy init to avoid static initialization order issues.
  static std::map<String, MeshTile *> *registry =
      new std::map<String, MeshTile *>();
  return *registry;
}

} // namespace

// ---- Static registry API ---------------------------------------------------

std::map<String, MeshTile *> &MeshTile::Registry() { return TileRegistry(); }

MeshTile *MeshTile::GetOrCreate(const String &node_key_hex, lv_obj_t *parent,
                                lv_coord_t width, lv_coord_t height) {
  std::map<String, MeshTile *> &registry = Registry();
  auto it = registry.find(node_key_hex);
  if (it != registry.end() && it->second != nullptr) {
    MeshTile *tile = it->second;
    tile->SetSize(width, height);
    return tile;
  }

  // First time for this node: allocate tile and register it.
  MeshTile *tile = new MeshTile(parent, width, height);
  tile->SetNodeKey(node_key_hex);
  registry[node_key_hex] = tile;
  return tile;
}

MeshTile *MeshTile::Find(const String &node_key_hex) {
  std::map<String, MeshTile *> &registry = Registry();
  auto it = registry.find(node_key_hex);
  if (it == registry.end()) {
    return nullptr;
  }
  return it->second;
}

void MeshTile::ForEach(VisitFunc func, void *user_data) {
  if (func == nullptr) {
    return;
  }
  std::map<String, MeshTile *> &registry = Registry();
  for (auto &entry : registry) {
    MeshTile *tile = entry.second;
    if (tile != nullptr) {
      func(tile, user_data);
    }
  }
}

void MeshTile::LoopAll(uint32_t now_ms) {
  std::map<String, MeshTile *> &registry = Registry();
  for (auto &entry : registry) {
    MeshTile *tile = entry.second;
    if (tile != nullptr) {
      tile->Loop(now_ms);
    }
  }
}

void MeshTile::Destroy(const String &node_key_hex) {
  std::map<String, MeshTile *> &registry = Registry();
  auto it = registry.find(node_key_hex);
  if (it == registry.end()) {
    return;
  }
  MeshTile *tile = it->second;
  registry.erase(it);
  if (tile != nullptr) {
    delete tile;
  }
}

void MeshTile::DestroyAll() {
  std::map<String, MeshTile *> &registry = Registry();
  for (auto &entry : registry) {
    if (entry.second != nullptr) {
      delete entry.second;
    }
  }
  registry.clear();
}

// ---- ctor / dtor -----------------------------------------------------------

MeshTile::MeshTile(lv_obj_t *parent, lv_coord_t width, lv_coord_t height) {
  InitWidgets(parent, width, height);

  // Reasonable defaults until first SetContent().
  bg_normal_ = lv_color_make(0x16, 0x3A, 0x24); // dark green
  fg_normal_ = lv_color_white();
  bg_flash_ = bg_normal_;
  fg_flash_ = fg_normal_;
  sensor_color_[0] = lv_color_white();
  sensor_color_[1] = lv_color_white();
  last_flash_active_ = false;
  flash_interval_ms_ = 0;
  last_flash_toggle_ms_ = 0;
}

MeshTile::~MeshTile() {
  // Destroy LVGL object tree if still present.
  if (root_ != nullptr) {
    lv_obj_del(root_);
    root_ = nullptr;
  }
}

void MeshTile::InitWidgets(lv_obj_t *parent, lv_coord_t width,
                           lv_coord_t height) {
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, width, height);
  lv_obj_set_style_radius(root_, 12, 0);
  lv_obj_set_style_pad_all(root_, kTilePadAll, 0); // CHANGED: use constant.
  lv_obj_set_style_border_width(root_, 0, 0);
  lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);

  // Location / title label (top, centered).
  label_loc_ = lv_label_create(root_);
  lv_obj_set_width(label_loc_, lv_pct(100));
  lv_obj_set_style_text_align(label_loc_, LV_TEXT_ALIGN_CENTER, 0);
  // Larger font choice.
  lv_obj_set_style_text_font(label_loc_, &lv_font_montserrat_28, 0);

  // Age label: small, top-right.
  label_age_ = lv_label_create(root_);
  lv_obj_set_width(label_age_, LV_SIZE_CONTENT); // shrink to content width.
  lv_obj_set_style_text_align(label_age_, LV_TEXT_ALIGN_RIGHT, 0);

  // Up to two sensor "slots" at the bottom: name (small) above temperature.
  for (int i = 0; i < 2; ++i) {
    sensor_name_label_[i] = lv_label_create(root_);
    sensor_temp_label_[i] = lv_label_create(root_);

    lv_obj_set_style_text_align(sensor_name_label_[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(sensor_temp_label_[i], LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_set_style_text_font(sensor_temp_label_[i], &lv_font_montserrat_26,
                               0);
  }
}

void MeshTile::SetSize(lv_coord_t width, lv_coord_t height) {
  if (root_ == nullptr) {
    return;
  }
  lv_obj_set_size(root_, width, height);
  // Re-run layout with current content.
  UpdateTextsAndLayout();
}

void MeshTile::SetContent(const Content &content) {
  content_ = content;

  // Clamp sensor count to [0, 2].
  if (content_.sensor_count < 0) {
    content_.sensor_count = 0;
  } else if (content_.sensor_count > 2) {
    content_.sensor_count = 2;
  }

  // We've now got valid content for this tile.
  has_content_ = true;

  // Recompute base palette + texts/layout from the new content.
  UpdateBaseColors();
  UpdateTextsAndLayout();

  // Only repaint base colors if we are currently in the "base" phase.
  if (!last_flash_active_) {
    ApplyBaseColors();
  }
}

void MeshTile::SetAgeOnly(uint32_t age_minutes, bool is_missing,
                          bool is_stale) {
  content_.age_minutes = age_minutes;
  content_.is_missing = is_missing;
  content_.is_stale = is_stale;

  if (label_age_ != nullptr) {
    if (content_.show_age) {
      char age_buf[24];
      snprintf(age_buf, sizeof(age_buf), "Age: %lu min",
               static_cast<unsigned long>(content_.age_minutes));
      lv_label_set_text(label_age_, age_buf);
      lv_obj_clear_flag(label_age_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_align(label_age_, LV_ALIGN_TOP_RIGHT, kAgeTopRightOffsetX,
                   kAgeTopRightOffsetY);
    } else {
      lv_label_set_text(label_age_, "");
      lv_obj_add_flag(label_age_, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // Age / missing / stale affect the *base* palette, but we do not
  // forcibly leave the current flash phase.
  UpdateBaseColors();
  UpdateTextsAndLayout();

  if (!last_flash_active_) {
    ApplyBaseColors();
  }
}

void MeshTile::UpdateBaseColors() {
  // Base colors are derived from node-level flags.
  bg_normal_ = lv_color_make(0x16, 0x3A, 0x24); // dark green
  fg_normal_ = lv_color_white();

  if (content_.node_has_alert) {
    // Red for active alerts.
    bg_normal_ = lv_color_make(0xB7, 0x1C, 0x1C);
    fg_normal_ = lv_color_white();
  } else if (content_.is_stale || content_.node_has_warning ||
             content_.seq_stuck) {
    // Amber/brown for stale, warning, or sequence-stuck.
    bg_normal_ = lv_color_make(0x8A, 0x5A, 0x00);
    fg_normal_ = lv_color_white();
  } else if (content_.is_missing) {
    // Blue-ish for missing/offline.
    bg_normal_ = lv_color_make(0x20, 0x40, 0x60);
    fg_normal_ = lv_color_white();
  }

  // Flash palette: tiles with any warning/alert flash between bg_normal_/fg
  // and a light gray/black.
  if (content_.node_has_alert || content_.node_has_warning ||
      content_.seq_stuck) {
    bg_flash_ = lv_color_make(0xCC, 0xCC, 0xCC);
    fg_flash_ = lv_color_black();
  } else {
    bg_flash_ = bg_normal_;
    fg_flash_ = fg_normal_;
  }
}

void MeshTile::UpdateTextsAndLayout() {
  if (root_ == nullptr) {
    return;
  }

  // Title / location.
  if (label_loc_ != nullptr) {
    const char *title =
        content_.title.length() ? content_.title.c_str() : "(unnamed)";
    lv_label_set_text(label_loc_, title);
    lv_obj_align(label_loc_, LV_ALIGN_TOP_MID, 0, kTitleTopOffset);
  }

  // Age label (optional, top-right).
  if (label_age_ != nullptr) {
    if (content_.show_age) {
      char age_buf[24];
      snprintf(age_buf, sizeof(age_buf), "Age: %lu min",
               static_cast<unsigned long>(content_.age_minutes));
      lv_label_set_text(label_age_, age_buf);
      lv_obj_clear_flag(label_age_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_align(label_age_, LV_ALIGN_TOP_RIGHT, kAgeTopRightOffsetX,
                   kAgeTopRightOffsetY);
    } else {
      lv_label_set_text(label_age_, "");
      lv_obj_add_flag(label_age_, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // Sensor sections.
  const int count =
      (content_.sensor_count < 0)
          ? 0
          : (content_.sensor_count > 2 ? 2 : content_.sensor_count);

  // No sensors: show a faint placeholder in the first temp slot.
  if (count == 0) {
    if (sensor_temp_label_[0] != nullptr) {
      lv_label_set_text(sensor_temp_label_[0], "(no sensors)");
      lv_obj_clear_flag(sensor_temp_label_[0], LV_OBJ_FLAG_HIDDEN);
      sensor_color_[0] = lv_color_make(0xAA, 0xAA, 0xAA);
      lv_obj_set_width(sensor_temp_label_[0], lv_obj_get_width(root_) - 8);
      lv_obj_align(sensor_temp_label_[0], LV_ALIGN_BOTTOM_MID, 0,
                   kTempBottomOffset);
    }
    if (sensor_temp_label_[1] != nullptr) {
      lv_label_set_text(sensor_temp_label_[1], "");
      lv_obj_add_flag(sensor_temp_label_[1], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 2; ++i) {
      if (sensor_name_label_[i] != nullptr) {
        lv_label_set_text(sensor_name_label_[i], "");
        lv_obj_add_flag(sensor_name_label_[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
    return;
  }

  // Layout parameters for left/right positioning at the bottom.
  lv_coord_t root_w = lv_obj_get_width(root_);
  if (root_w <= 0) {
    root_w = 200; // fallback
  }
  const lv_coord_t margin = 4;
  lv_coord_t section_w;
  const bool two_sensors = (count == 2);
  if (two_sensors) {
    section_w = root_w / 2 - 2 * margin;
    if (section_w < 20) {
      section_w = root_w / 2;
    }
  } else {
    section_w = root_w - 2 * margin;
  }

  for (int i = 0; i < 2; ++i) {
    lv_obj_t *name_lbl = sensor_name_label_[i];
    lv_obj_t *temp_lbl = sensor_temp_label_[i];

    if (temp_lbl == nullptr || name_lbl == nullptr) {
      continue;
    }

    if (i >= count) {
      // Hide unused slots.
      lv_label_set_text(temp_lbl, "");
      lv_obj_add_flag(temp_lbl, LV_OBJ_FLAG_HIDDEN);

      lv_label_set_text(name_lbl, "");
      lv_obj_add_flag(name_lbl, LV_OBJ_FLAG_HIDDEN);
      continue;
    }

    const SensorView &sv = content_.sensors[i];

    // Temperature text (always shown for active sensors).
    char temp_buf[32];
    if (!sv.has_value || isnan(sv.temp_c)) {
      snprintf(temp_buf, sizeof(temp_buf), "--");
    } else {
      float temp_disp = sv.temp_c;
      char unit_ch = 'C';
      if (content_.display_fahrenheit) {
        temp_disp = temp_disp * 1.8f + 32.0f;
        unit_ch = 'F';
      }
      snprintf(temp_buf, sizeof(temp_buf), "%.1f%c",
               static_cast<double>(temp_disp), unit_ch);
    }
    lv_label_set_text(temp_lbl, temp_buf);
    lv_obj_clear_flag(temp_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(temp_lbl, section_w);

    // Optional sensor name above the temperature.
    if (content_.show_sensor_labels && sv.label.length() > 0) {
      lv_label_set_text(name_lbl, sv.label.c_str());
      lv_obj_clear_flag(name_lbl, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_width(name_lbl, section_w);
    } else {
      lv_label_set_text(name_lbl, "");
      lv_obj_add_flag(name_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    // Per-sensor color rules (applied to the temperature text).
    lv_color_t sensor_color;
    if (!sv.has_value || isnan(sv.temp_c)) {
      sensor_color = lv_color_make(0xCC, 0xCC, 0xCC);
    } else if (sv.is_alert) {
      sensor_color = lv_color_make(0xFF, 0xFF, 0x80);
    } else if (sv.is_warning) {
      sensor_color = lv_color_make(0xFF, 0xD7, 0x00);
    } else if (content_.node_has_alert) {
      sensor_color = lv_color_make(0xFF, 0xE0, 0xE0);
    } else if (content_.is_stale || content_.node_has_warning ||
               content_.seq_stuck) {
      sensor_color = lv_color_make(0xFF, 0xF7, 0xE0);
    } else if (content_.is_missing) {
      sensor_color = lv_color_make(0xB0, 0xBE, 0xC5);
    } else {
      sensor_color = lv_color_make(0xE0, 0xFF, 0xE0);
    }
    sensor_color_[i] = sensor_color;

    // Align near the bottom, one slot in the lower left and one in the
    // lower right. With a single sensor, center it.
    const lv_align_t align_base =
        two_sensors ? (i == 0 ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT)
                    : LV_ALIGN_BOTTOM_MID;

    lv_obj_align(temp_lbl, align_base, 0, kTempBottomOffset);

    if (!lv_obj_has_flag(name_lbl, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_align(name_lbl, align_base, 0, kNameBottomOffset);
    }
  }
}

void MeshTile::ApplyBaseColors() {
  if (root_ == nullptr) {
    return;
  }

  lv_obj_set_style_bg_color(root_, bg_normal_, 0);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

  if (label_loc_ != nullptr) {
    lv_obj_set_style_text_color(label_loc_, fg_normal_, 0);
  }
  if (label_age_ != nullptr) {
    lv_obj_set_style_text_color(label_age_, fg_normal_, 0);
  }

  for (int i = 0; i < 2; ++i) {
    if (sensor_temp_label_[i] != nullptr) {
      lv_obj_set_style_text_color(sensor_temp_label_[i], sensor_color_[i], 0);
    }
    if (sensor_name_label_[i] != nullptr) {
      lv_obj_set_style_text_color(sensor_name_label_[i], fg_normal_, 0);
    }
  }
}

void MeshTile::MoveToForeground() {
  if (root_ != nullptr) {
    // Reorder within the parent container so flex layout matches
    // our sorted node order.
    lv_obj_move_foreground(root_);
  }
}

void MeshTile::ApplyFlashPhase(bool flash_on) {
  if (root_ == nullptr) {
    return;
  }

  const bool flashable = content_.node_has_alert || content_.node_has_warning;
  const bool flash_active = flash_on && flashable;

  if (flash_active == last_flash_active_) {
    // Nothing to change for this tile.
    return;
  }
  last_flash_active_ = flash_active;

  if (flash_active) {
    // Flash: light gray background, black text for everything.
    lv_obj_set_style_bg_color(root_, bg_flash_, 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

    if (label_loc_ != nullptr) {
      lv_obj_set_style_text_color(label_loc_, fg_flash_, 0);
    }
    if (label_age_ != nullptr) {
      lv_obj_set_style_text_color(label_age_, fg_flash_, 0);
    }
    for (int i = 0; i < 2; ++i) {
      if (sensor_temp_label_[i] != nullptr) {
        lv_obj_set_style_text_color(sensor_temp_label_[i], fg_flash_, 0);
      }
      if (sensor_name_label_[i] != nullptr) {
        lv_obj_set_style_text_color(sensor_name_label_[i], fg_flash_, 0);
      }
    }
  } else {
    // Return to normal per-tile colors.
    ApplyBaseColors();
  }
}

void MeshTile::SetFlashIntervalMs(uint32_t interval_ms) {
  flash_interval_ms_ = interval_ms;
  last_flash_toggle_ms_ = 0;
  flash_on_ = false;
  UpdateFlashAppearance();
}

void MeshTile::EnsureChildIndex(uint32_t desired_index) {
  if (root_ == nullptr) {
    return;
  }

  lv_obj_t *parent = lv_obj_get_parent(root_);
  if (parent == nullptr) {
    return;
  }

  uint32_t current_index = lv_obj_get_index(root_);
  if (current_index == desired_index) {
    // Already in the right spot; avoid LVGL churn.
    return;
  }

  lv_obj_move_to_index(root_, static_cast<lv_coord_t>(desired_index));
}

void MeshTile::UpdateFlashAppearance() {
  if (root_ == nullptr) {
    return;
  }

  const bool has_severity = content_.node_has_alert ||
                            content_.node_has_warning || content_.seq_stuck;

  // If there's nothing to flash or flashing is disabled, show the base palette
  // computed in UpdateBaseColors() via ApplyBaseColors().
  if (!has_severity || flash_interval_ms_ == 0U) {
    ApplyBaseColors();
    return;
  }

  // Flashing enabled:
  // - flash_on_ == false  -> base colors
  // - flash_on_ == true   -> bright highlight
  if (!flash_on_) {
    // "Off" phase of the flash: use normal per-tile colors.
    ApplyBaseColors();
    return;
  }

  // "On" phase of the flash: bright alert / warning.
  lv_color_t bg = bg_normal_;
  lv_color_t fg = fg_normal_;

  if (content_.node_has_alert) {
    bg = MakeColor(0xFF, 0x40, 0x40); // bright red
    fg = lv_color_black();
  } else {
    // Warning / seq_stuck: bright amber.
    bg = MakeColor(0xFF, 0xA0, 0x40);
    fg = lv_color_black();
  }

  lv_obj_set_style_bg_color(root_, bg, 0);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

  if (label_loc_ != nullptr) {
    lv_obj_set_style_text_color(label_loc_, fg, 0);
  }
  if (label_age_ != nullptr) {
    lv_obj_set_style_text_color(label_age_, fg, 0);
  }

  for (int i = 0; i < 2; ++i) {
    if (sensor_temp_label_[i] != nullptr) {
      lv_obj_set_style_text_color(sensor_temp_label_[i], fg, 0);
    }
    if (sensor_name_label_[i] != nullptr) {
      lv_obj_set_style_text_color(sensor_name_label_[i], fg, 0);
    }
  }
}

void MeshTile::Loop(uint32_t now_ms) {
  if (!has_content_ || root_ == nullptr) {
    return;
  }

  const bool has_severity = content_.node_has_alert ||
                            content_.node_has_warning || content_.seq_stuck;

  // If there is nothing to flash, make sure we are in steady appearance.
  if (!has_severity || flash_interval_ms_ == 0U) {
    if (flash_on_) {
      flash_on_ = false;
      UpdateFlashAppearance();
    }
    return;
  }

  if (last_flash_toggle_ms_ == 0U) {
    // First time: arm the timer but do not toggle yet.
    last_flash_toggle_ms_ = now_ms;
    return;
  }

  const uint32_t delta = now_ms - last_flash_toggle_ms_;
  if (delta < flash_interval_ms_) {
    return;
  }

  // Toggle flash state and repaint.
  flash_on_ = !flash_on_;
  last_flash_toggle_ms_ = now_ms;

  // if (g_debug_enabled) {
  //   Serial.printf("[FLASH] now=%lu ms interval=%lu ms delta=%lu ms state=%s\n",
  //                 static_cast<unsigned long>(now_ms),
  //                 static_cast<unsigned long>(flash_interval_ms_),
  //                 static_cast<unsigned long>(delta), flash_on_ ? "ON" : "OFF");
  // }

  UpdateFlashAppearance();
}
