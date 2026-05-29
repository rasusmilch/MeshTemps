#ifndef MESHTEMPS_GUI_NODE_CHART_POINT_H_
#define MESHTEMPS_GUI_NODE_CHART_POINT_H_

#include <Arduino.h>
#include <math.h>

// Small POD used for chart downsampling.
// x_hours is hours since start of the plotted range.
struct ChartPoint {
  double x_hours = 0.0;
  float temp_display = NAN;
};

#endif  // MESHTEMPS_GUI_NODE_CHART_POINT_H_
