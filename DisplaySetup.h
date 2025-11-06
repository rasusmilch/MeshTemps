#pragma once
#include <lvgl.h>

// Call from ROOT setup() before building your GUI
void Display_Init();

// Call each loop() on ROOT so LVGL can run timers & flushes
void Display_Loop();
