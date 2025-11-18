#include "mesh_tile.h"

MeshTile::MeshTile(lv_obj_t* parent,
                   lv_coord_t width,
                   lv_coord_t height) {
  InitWidgets(parent, width, height);

  // Reasonable defaults until first SetContent().
  bg_normal_ = lv_color_make(0x16, 0x3A, 0x24);  // dark green
  fg_normal_ = lv_color_white();
  bg_flash_ = bg_normal_;
  fg_flash_ = fg_normal_;
  sensor_color_[0] = lv_color_white();
  sensor_color_[1] = lv_color_white();
  last_flash_active_ = false;
  flash_interval_ms_ = 0;
  last_flash_toggle_ms_ = 0;
}

void MeshTile::InitWidgets(lv_obj_t* parent,
                           lv_coord_t width,
                           lv_coord_t height) {
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, width, height);
  lv_obj_set_style_radius(root_, 12, 0);
  lv_obj_set_style_pad_all(root_, 6, 0);
  lv_obj_set_style_border_width(root_, 0, 0);
  lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);

  // Location / title label (top).
  label_loc_ = lv_label_create(root_);
  lv_obj_set_width(label_loc_, lv_pct(100));
  lv_obj_set_style_text_align(label_loc_, LV_TEXT_ALIGN_CENTER, 0);
  // Larger font choice.
  lv_obj_set_style_text_font(label_loc_, &lv_font_montserrat_28, 0);

  // Age label (below title).
  label_age_ = lv_label_create(root_);
  lv_obj_set_width(label_age_, lv_pct(100));
  lv_obj_set_style_text_align(label_age_, LV_TEXT_ALIGN_CENTER, 0);

  // Up to two sensor "slots" at the bottom: name (small) above temperature.
  for (int i = 0; i < 2; ++i) {
    sensor_name_label_[i] = lv_label_create(root_);
    sensor_temp_label_[i] = lv_label_create(root_);

    lv_obj_set_style_text_align(sensor_name_label_[i],
                                LV_TEXT_ALIGN_CENTER,
                                0);
    lv_obj_set_style_text_align(sensor_temp_label_[i],
                                LV_TEXT_ALIGN_CENTER,
                                0);

    lv_obj_set_style_text_font(sensor_temp_label_[i], 
                                &lv_font_montserrat_26, 
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

void MeshTile::SetContent(const Content& content) {
  content_ = content;

  // Clamp sensor count to [0, 2].
  if (content_.sensor_count < 0) {
    content_.sensor_count = 0;
  } else if (content_.sensor_count > 2) {
    content_.sensor_count = 2;
  }

  // Recompute base palette + texts/layout from the new content.
  UpdateBaseColors();
  UpdateTextsAndLayout();

  // Only repaint base colors if we are currently in the "base" phase.
  if (!last_flash_active_) {
    ApplyBaseColors();
  }
}

void MeshTile::SetAgeOnly(uint32_t age_minutes,
                          bool is_missing,
                          bool is_stale) {
  content_.age_minutes = age_minutes;
  content_.is_missing = is_missing;
  content_.is_stale = is_stale;

  if (label_age_ != nullptr) {
    if (content_.show_age) {
      char age_buf[24];
      snprintf(age_buf,
               sizeof(age_buf),
               "Age: %lu min",
               static_cast<unsigned long>(content_.age_minutes));
      lv_label_set_text(label_age_, age_buf);
      lv_obj_clear_flag(label_age_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_align(label_age_, LV_ALIGN_TOP_MID, 0, 20);
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
  bg_normal_ = lv_color_make(0x16, 0x3A, 0x24);  // dark green
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
    const char* title =
        content_.title.length() ? content_.title.c_str() : "(unnamed)";
    lv_label_set_text(label_loc_, title);
    lv_obj_align(label_loc_, LV_ALIGN_TOP_MID, 0, 0);
  }

  // Age label (optional).
  if (label_age_ != nullptr) {
    if (content_.show_age) {
      char age_buf[24];
      snprintf(age_buf,
               sizeof(age_buf),
               "Age: %lu min",
               static_cast<unsigned long>(content_.age_minutes));
      lv_label_set_text(label_age_, age_buf);
      lv_obj_clear_flag(label_age_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_align(label_age_, LV_ALIGN_TOP_MID, 0, 20);
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
      lv_obj_align(sensor_temp_label_[0], LV_ALIGN_BOTTOM_MID, 0, -8);
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
    root_w = 200;  // fallback
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
    lv_obj_t* name_lbl = sensor_name_label_[i];
    lv_obj_t* temp_lbl = sensor_temp_label_[i];

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

    const SensorView& sv = content_.sensors[i];

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
      snprintf(temp_buf,
               sizeof(temp_buf),
               "%.1f%c",
               static_cast<double>(temp_disp),
               unit_ch);
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

    const lv_coord_t temp_y_offset = -8;
    const lv_coord_t name_y_offset = -28;

    lv_obj_align(temp_lbl, align_base, 0, temp_y_offset);

    if (!lv_obj_has_flag(name_lbl, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_align(name_lbl, align_base, 0, name_y_offset);
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

void MeshTile::ApplyFlashPhase(bool flash_on) {
  if (root_ == nullptr) {
    return;
  }

  const bool flashable =
      content_.node_has_alert || content_.node_has_warning;
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

  if (flash_interval_ms_ == 0U) {
    // Flashing disabled: reset phase and ensure base colors are visible.
    last_flash_toggle_ms_ = 0U;
    if (last_flash_active_) {
      ApplyFlashPhase(false);
    }
  }
}

// Per-tile time-based update. Call this once per main loop with millis().
void MeshTile::Loop(uint32_t now_ms) {
  const bool flashable =
      content_.node_has_alert || content_.node_has_warning;

  // If this tile should not flash, or flashing is disabled, stay in base
  // colors and clear timing state.
  if (!flashable || flash_interval_ms_ == 0U) {
    if (last_flash_active_) {
      ApplyFlashPhase(false);
    }
    last_flash_toggle_ms_ = 0U;
    return;
  }

  if (last_flash_toggle_ms_ == 0U) {
    // First time we notice this tile is flashable; start the timer but do not
    // immediately flip phase.
    last_flash_toggle_ms_ = now_ms;
    return;
  }

  const uint32_t elapsed_ms = now_ms - last_flash_toggle_ms_;
  if (elapsed_ms < flash_interval_ms_) {
    return;
  }

  // Time to toggle phase.
  last_flash_toggle_ms_ = now_ms;
  const bool next_phase = !last_flash_active_;
  ApplyFlashPhase(next_phase);
}
