#include <lvgl.h>
#include "/home/rob/Arduino/libraries/ESP32_Display_Panel/src/board/esp_panel_board.hpp"
#include <esp_display_panel.hpp>

// ---- Select Waveshare 7" preset ----
#include <board/supported/waveshare/BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7.h>

// LVGL glue from the library (copy both files next to your sketch):
//   Arduino/libraries/ESP32_Display_Panel/test_apps/gui/lvgl_v8_port/main/lvgl_v8_port.h
//   Arduino/libraries/ESP32_Display_Panel/test_apps/gui/lvgl_v8_port/main/lvgl_v8_port.cpp
#include "lvgl_v8_port.h"

using esp_panel::board::Board;
using esp_panel::board::config_t;
namespace ws7 = esp_panel::board::supported::waveshare::ESP32_S3_TOUCH_LCD_7;

static Board board;
static esp_panel::drivers::LCD*   lcd = nullptr;
static esp_panel::drivers::Touch* tp  = nullptr;

void Display_Init() {
  config_t cfg = ws7::default_config();  // <- correct namespace + type
  ESP_ERROR_CHECK(board.begin(&cfg));
  lcd = board.getLCD();
  tp  = board.getTouch();
  ESP_ERROR_CHECK(lvgl_port_init(lcd, tp));

  // If you want a sanity label before your GUI:
  // lv_obj_t* l = lv_label_create(lv_scr_act());
  // lv_label_set_text(l, "Waveshare 7\" READY");
  // lv_obj_align(l, LV_ALIGN_TOP_LEFT, 6, 6);
}

void Display_Loop() {
  // Let LVGL handle timers/animations/input
  lv_timer_handler();   // call once per loop (≈5–20ms cadence is fine)
}
