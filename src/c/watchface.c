#include <pebble.h>
#include <limits.h>

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

// --- Message keys ---------------------------------------------
#define KEY_BACKGROUND_COLOR  0
#define KEY_TEXT_COLOR        1
#define KEY_TEMPERATURE_UNIT  2
#define KEY_SHOW_DATE         3
#define KEY_HOUR_FORMAT       4
#define KEY_CALENDAR_EVENTS   5
#define KEY_WEATHER_TEMP      6
#define KEY_WEATHER_CODE      7
#define KEY_SHOW_TICKS        8
// 9-11 retired (individual cal colour keys replaced by packed string)
#define KEY_HOUR_HAND_COLOR   12
#define KEY_CAL_COLORS        13  // packed "c0,c1,c2,...,c9" palette indices

// Rendering of Screenshots
#define SCREENSHOT_MODE 0

// --- Colour palette -------------------------------------------
// Must match PALETTE in index.html exactly.
static const uint8_t s_palette_argb[16] = {
    GColorMintGreenARGB8,        //  0 Teal
    GColorBrightGreenARGB8,      //  1 Mint
    GColorCyanARGB8,             //  2 Cyan
    GColorBlueARGB8,             //  3 Blue
    GColorLavenderIndigoARGB8,   //  4 Lavender
    GColorPurpleARGB8,           //  5 Purple
    GColorMagentaARGB8,          //  6 Pink
    GColorRedARGB8,              //  7 Red
    GColorMelonARGB8,            //  8 Coral
    GColorOrangeARGB8,           //  9 Orange
    GColorYellowARGB8,           // 10 Yellow
    GColorWhiteARGB8,            // 11 White
    GColorLightGrayARGB8,        // 12 Light Gray
    GColorDarkGrayARGB8,         // 13 Dark Gray
    GColorIndigoARGB8,           // 14 Indigo
    GColorMintGreenARGB8,        // 15 Spring Green
};

static GColor palette_color(int idx) {
    if (idx < 0 || idx > 15) idx = 0;
    return (GColor){ .argb = s_palette_argb[idx] };
}

// --- Calendar config ------------------------------------------
#define MAX_CALENDARS 10
#define MAX_EVENTS    10  // max arcs on screen at once

typedef struct {
    int start_mins;
    int duration_mins;
    int cal_index;   // 0-9, indexes into s_cal_color_idx
} CalEvent;

// --- State ---------------------------------------------------
static Window *s_window;
static Layer  *s_canvas_layer;

static GColor s_bg_color   = GColorBlack;
static GColor s_text_color = GColorWhite;
static bool   s_use_fahrenheit = false;
static bool   s_show_date      = true;
static bool   s_show_ticks     = true;

// Default colours spread across the palette for visual variety
static int s_cal_color_idx[MAX_CALENDARS] = { 0, 4, 8, 2, 9, 5, 1, 7, 10, 3 };
static int s_hour_hand_color = 11;

static int s_weather_temp = INT_MIN;
static int s_weather_code = -1;

static CalEvent s_events[MAX_EVENTS];
static int      s_event_count = 0;

static GFont s_small_font;
static GFont s_date_font;

// --- Square perimeter helper ----------------------------------
static GPoint perimeter_point(GRect bounds, int inset, int angle_deg) {
    int w = bounds.size.w;
    int h = bounds.size.h;
    int perim = 2 * (w + h);
    int px = (w / 2) + (angle_deg * perim / 360);
    px = px % perim;
    if (px < 0) px += perim;
    int x, y;
    if (px < w) {
        x = px;              y = inset;
    } else if (px < w + h) {
        x = w - 1 - inset;  y = px - w;
    } else if (px < 2 * w + h) {
        x = (2 * w + h - 1) - px; y = h - 1 - inset;
    } else {
        x = inset;           y = perim - px;
    }
    return GPoint(x, y);
}

// --- Proximity helpers ----------------------------------------
static bool segment_near_rect(int x0, int y0, int x1, int y1,
                               int rx, int ry, int rw, int rh,
                               int threshold) {
    int cx = rx + rw / 2, cy = ry + rh / 2;
    int hw = rw / 2 + threshold, hv = rh / 2 + threshold;
    for (int t = 0; t <= 8; t++) {
        int px = x0 + (x1 - x0) * t / 8;
        int py = y0 + (y1 - y0) * t / 8;
        int dx = px - cx; if (dx < 0) dx = -dx;
        int dy = py - cy; if (dy < 0) dy = -dy;
        if (dx <= hw && dy <= hv) return true;
    }
    return false;
}

static bool point_near_rect(int px, int py,
                             int rx, int ry, int rw, int rh,
                             int threshold) {
    int cx = rx + rw / 2, cy = ry + rh / 2;
    int dx = px - cx; if (dx < 0) dx = -dx;
    int dy = py - cy; if (dy < 0) dy = -dy;
    return (dx <= rw / 2 + threshold && dy <= rh / 2 + threshold);
}

// --- Shadow decision ------------------------------------------
static bool compute_show_shadow(GRect bounds, GPoint centre,
                                 int clock_radius, bool is_round,
                                 int hour_len, int min_len,
                                 int hour_angle, int min_angle,
                                 int wx, int wy, int ww, int wh) {
    int threshold = 18;

    GPoint tip_hour = GPoint(
        centre.x + sin_lookup(DEG_TO_TRIGANGLE(hour_angle)) * hour_len / TRIG_MAX_RATIO,
        centre.y - cos_lookup(DEG_TO_TRIGANGLE(hour_angle)) * hour_len / TRIG_MAX_RATIO
    );
    GPoint tip_min = GPoint(
        centre.x + sin_lookup(DEG_TO_TRIGANGLE(min_angle)) * min_len / TRIG_MAX_RATIO,
        centre.y - cos_lookup(DEG_TO_TRIGANGLE(min_angle)) * min_len / TRIG_MAX_RATIO
    );
    if (segment_near_rect(centre.x, centre.y, tip_hour.x, tip_hour.y,
                          wx, wy, ww, wh, threshold)) return true;
    if (segment_near_rect(centre.x, centre.y, tip_min.x, tip_min.y,
                          wx, wy, ww, wh, threshold)) return true;

    if (s_show_ticks) {
        int arc_inset = 4;
        for (int h = 0; h < 12; h++) {
            int deg = h * 30;
            int tick_len = (h % 3 == 0) ? 14 : 9;
            if (is_round) {
                int32_t trig = DEG_TO_TRIGANGLE(deg);
                int sv = sin_lookup(trig), cv = cos_lookup(trig);
                GPoint outer = GPoint(
                    centre.x + sv * clock_radius / TRIG_MAX_RATIO,
                    centre.y - cv * clock_radius / TRIG_MAX_RATIO);
                GPoint inner = GPoint(
                    centre.x + sv * (clock_radius - tick_len) / TRIG_MAX_RATIO,
                    centre.y - cv * (clock_radius - tick_len) / TRIG_MAX_RATIO);
                if (segment_near_rect(outer.x, outer.y, inner.x, inner.y,
                                      wx, wy, ww, wh, threshold)) return true;
            } else {
                GPoint outer = perimeter_point(bounds, arc_inset, deg);
                int dx = centre.x - outer.x, dy = centre.y - outer.y;
                int dist2 = dx * dx + dy * dy, dist = tick_len;
                if (dist2 > 0) {
                    int r = (dist2 > 0x10000) ? 256 : 16;
                    for (int n = 0; n < 8; n++) r = (r + dist2 / r) / 2;
                    dist = r;
                }
                GPoint inner = GPoint(
                    outer.x + (dx * tick_len * 16 / dist) / 16,
                    outer.y + (dy * tick_len * 16 / dist) / 16);
                if (segment_near_rect(outer.x, outer.y, inner.x, inner.y,
                                      wx, wy, ww, wh, threshold)) return true;
            }
        }
    }

    for (int i = 0; i < s_event_count; i++) {
        CalEvent *ev = &s_events[i];
        if (ev->duration_mins <= 0) continue;
        int start = ev->start_mins;
        int end   = ev->start_mins + ev->duration_mins;
        if (start >= 720) continue;
        if (end   >  720) end = 720;
        int start_deg = start * 360 / 720;
        int end_deg   = end   * 360 / 720;
        if (is_round) {
            int arc_radius = clock_radius + 8;
            for (int deg = start_deg; deg <= end_deg; deg += 5) {
                int px = centre.x + sin_lookup(DEG_TO_TRIGANGLE(deg)) * arc_radius / TRIG_MAX_RATIO;
                int py = centre.y - cos_lookup(DEG_TO_TRIGANGLE(deg)) * arc_radius / TRIG_MAX_RATIO;
                if (point_near_rect(px, py, wx, wy, ww, wh, threshold)) return true;
            }
        } else {
            for (int deg = start_deg; deg <= end_deg; deg += 5) {
                GPoint p = perimeter_point(bounds, 4, deg);
                if (point_near_rect(p.x, p.y, wx, wy, ww, wh, threshold)) return true;
            }
        }
    }
    return false;
}

// --- Weather icon drawing -------------------------------------

static void draw_cloud(GContext *ctx, int ox, int oy, bool shadow) {
    if (shadow) {
        graphics_context_set_fill_color(ctx, GColorBlack);
        graphics_fill_circle(ctx, GPoint(ox + 5,  oy + 13), 5);
        graphics_fill_circle(ctx, GPoint(ox + 13, oy + 13), 5);
        graphics_fill_circle(ctx, GPoint(ox + 9,  oy + 9),  6);
        graphics_fill_rect(ctx, GRect(ox + 2, oy + 10, 16, 7), 0, GCornerNone);
    }
    graphics_context_set_fill_color(ctx, GColorLightGray);
    graphics_fill_circle(ctx, GPoint(ox + 5,  oy + 13), 4);
    graphics_fill_circle(ctx, GPoint(ox + 13, oy + 13), 4);
    graphics_fill_circle(ctx, GPoint(ox + 9,  oy + 9),  5);
    graphics_fill_rect(ctx, GRect(ox + 3, oy + 11, 14, 5), 0, GCornerNone);
}

static void draw_sun(GContext *ctx, int ox, int oy, bool shadow) {
    int cx = ox + 10, cy = oy + 10;
    static const int ray_dx[8] = { 0,  3,  4,  3,  0, -3, -4, -3 };
    static const int ray_dy[8] = {-4, -3,  0,  3,  4,  3,  0, -3 };
    if (shadow) {
        graphics_context_set_stroke_color(ctx, GColorBlack);
        graphics_context_set_stroke_width(ctx, 3);
        for (int r = 0; r < 8; r++)
            graphics_draw_line(ctx,
                GPoint(cx + ray_dx[r] * 6 / 4, cy + ray_dy[r] * 6 / 4),
                GPoint(cx + ray_dx[r] * 10 / 4, cy + ray_dy[r] * 10 / 4));
        graphics_context_set_fill_color(ctx, GColorBlack);
        graphics_fill_circle(ctx, GPoint(cx, cy), 5);
    }
    graphics_context_set_stroke_color(ctx, GColorYellow);
    graphics_context_set_stroke_width(ctx, 1);
    for (int r = 0; r < 8; r++)
        graphics_draw_line(ctx,
            GPoint(cx + ray_dx[r] * 6 / 4, cy + ray_dy[r] * 6 / 4),
            GPoint(cx + ray_dx[r] * 10 / 4, cy + ray_dy[r] * 10 / 4));
    graphics_context_set_fill_color(ctx, GColorYellow);
    graphics_fill_circle(ctx, GPoint(cx, cy), 4);
}

static void draw_rain(GContext *ctx, int ox, int oy, GColor drop_color, bool shadow) {
    draw_cloud(ctx, ox, oy - 2, shadow);
    if (shadow) {
        graphics_context_set_stroke_color(ctx, GColorBlack);
        graphics_context_set_stroke_width(ctx, 3);
        graphics_draw_line(ctx, GPoint(ox + 5,  oy + 14), GPoint(ox + 4,  oy + 17));
        graphics_draw_line(ctx, GPoint(ox + 9,  oy + 14), GPoint(ox + 8,  oy + 17));
        graphics_draw_line(ctx, GPoint(ox + 13, oy + 14), GPoint(ox + 12, oy + 17));
    }
    graphics_context_set_stroke_color(ctx, drop_color);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(ox + 5,  oy + 14), GPoint(ox + 4,  oy + 17));
    graphics_draw_line(ctx, GPoint(ox + 9,  oy + 14), GPoint(ox + 8,  oy + 17));
    graphics_draw_line(ctx, GPoint(ox + 13, oy + 14), GPoint(ox + 12, oy + 17));
}

static void draw_snow(GContext *ctx, int ox, int oy, bool shadow) {
    draw_cloud(ctx, ox, oy - 2, shadow);
    int cx = ox + 9, cy = oy + 16;
    if (shadow) {
        graphics_context_set_fill_color(ctx, GColorBlack);
        static const int sx[9] = {0,0,0,-3,3,-2,2,-2,2};
        static const int sy[9] = {0,-3,3,0,0,-2,-2,2,2};
        for (int d = 0; d < 9; d++)
            graphics_fill_circle(ctx, GPoint(cx + sx[d], cy + sy[d]), 2);
    }
    graphics_context_set_fill_color(ctx, GColorCyan);
    static const int sx[9] = {0,0,0,-3,3,-2,2,-2,2};
    static const int sy[9] = {0,-3,3,0,0,-2,-2,2,2};
    for (int d = 0; d < 9; d++)
        graphics_fill_circle(ctx, GPoint(cx + sx[d], cy + sy[d]), 1);
}

static void draw_thunder(GContext *ctx, int ox, int oy, bool shadow) {
    draw_cloud(ctx, ox, oy - 2, shadow);
    if (shadow) {
        graphics_context_set_stroke_color(ctx, GColorBlack);
        graphics_context_set_stroke_width(ctx, 4);
        graphics_draw_line(ctx, GPoint(ox + 11, oy + 12), GPoint(ox + 8,  oy + 16));
        graphics_draw_line(ctx, GPoint(ox + 8,  oy + 16), GPoint(ox + 11, oy + 16));
        graphics_draw_line(ctx, GPoint(ox + 11, oy + 16), GPoint(ox + 7,  oy + 20));
    }
    graphics_context_set_stroke_color(ctx, GColorYellow);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(ox + 11, oy + 12), GPoint(ox + 8,  oy + 16));
    graphics_draw_line(ctx, GPoint(ox + 8,  oy + 16), GPoint(ox + 11, oy + 16));
    graphics_draw_line(ctx, GPoint(ox + 11, oy + 16), GPoint(ox + 7,  oy + 20));
}

static void draw_fog(GContext *ctx, int ox, int oy, bool shadow) {
    if (shadow) {
        graphics_context_set_stroke_color(ctx, GColorBlack);
        graphics_context_set_stroke_width(ctx, 4);
        graphics_draw_line(ctx, GPoint(ox + 2, oy + 7),  GPoint(ox + 18, oy + 7));
        graphics_draw_line(ctx, GPoint(ox + 2, oy + 11), GPoint(ox + 18, oy + 11));
        graphics_draw_line(ctx, GPoint(ox + 2, oy + 15), GPoint(ox + 18, oy + 15));
    }
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(ox + 2, oy + 7),  GPoint(ox + 18, oy + 7));
    graphics_draw_line(ctx, GPoint(ox + 2, oy + 11), GPoint(ox + 18, oy + 11));
    graphics_draw_line(ctx, GPoint(ox + 2, oy + 15), GPoint(ox + 18, oy + 15));
}

static void draw_partly_cloudy(GContext *ctx, int ox, int oy, bool shadow) {
    int sx = ox + 5, sy = oy + 5;
    static const int ray_dx[8] = { 0,  2,  3,  2,  0, -2, -3, -2 };
    static const int ray_dy[8] = {-3, -2,  0,  2,  3,  2,  0, -2 };
    if (shadow) {
        graphics_context_set_stroke_color(ctx, GColorBlack);
        graphics_context_set_stroke_width(ctx, 3);
        for (int r = 0; r < 8; r++)
            graphics_draw_line(ctx,
                GPoint(sx + ray_dx[r] * 4 / 3, sy + ray_dy[r] * 4 / 3),
                GPoint(sx + ray_dx[r] * 6 / 3, sy + ray_dy[r] * 6 / 3));
        graphics_context_set_fill_color(ctx, GColorBlack);
        graphics_fill_circle(ctx, GPoint(sx, sy), 4);
    }
    graphics_context_set_stroke_color(ctx, GColorYellow);
    graphics_context_set_stroke_width(ctx, 1);
    for (int r = 0; r < 8; r++)
        graphics_draw_line(ctx,
            GPoint(sx + ray_dx[r] * 4 / 3, sy + ray_dy[r] * 4 / 3),
            GPoint(sx + ray_dx[r] * 6 / 3, sy + ray_dy[r] * 6 / 3));
    graphics_context_set_fill_color(ctx, GColorYellow);
    graphics_fill_circle(ctx, GPoint(sx, sy), 3);
    draw_cloud(ctx, ox + 2, oy + 3, shadow);
}

static void draw_weather_icon(GContext *ctx, int code, int ox, int oy, bool shadow) {
    if (code == 0)                      draw_sun(ctx, ox, oy, shadow);
    else if (code >= 1  && code <= 3)   draw_partly_cloudy(ctx, ox, oy, shadow);
    else if (code == 45 || code == 48)  draw_fog(ctx, ox, oy, shadow);
    else if (code >= 51 && code <= 67)  draw_rain(ctx, ox, oy, code >= 56 ? GColorCyan : GColorBlue, shadow);
    else if (code >= 71 && code <= 77)  draw_snow(ctx, ox, oy, shadow);
    else if (code >= 80 && code <= 82)  draw_rain(ctx, ox, oy, GColorBlue, shadow);
    else if (code >= 85 && code <= 86)  draw_snow(ctx, ox, oy, shadow);
    else if (code >= 95 && code <= 99)  draw_thunder(ctx, ox, oy, shadow);
}

// --- Canvas draw ---------------------------------------------
static void canvas_update_proc(Layer *layer, GContext *ctx) {
    GRect   bounds = layer_get_bounds(layer);
    GPoint  centre = GPoint(bounds.size.w / 2, bounds.size.h / 2);

    graphics_context_set_fill_color(ctx, s_bg_color);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    time_t     now_t = time(NULL);
    struct tm *now   = localtime(&now_t);
    #if SCREENSHOT_MODE
      now->tm_hour = 10;
      now->tm_min  = 10;
      now->tm_sec  = 0;
    #endif
    
    // Add Fake events for emulator
    #if SCREENSHOT_MODE
        APP_LOG(APP_LOG_LEVEL_ERROR, "SCREENSHOT MODE ON");
    #else
        APP_LOG(APP_LOG_LEVEL_ERROR, "SCREENSHOT MODE OFF");
    #endif
    #if SCREENSHOT_MODE
      s_event_count = 2;
  
      // 1:00 → 2:30 (Blue)
      s_events[0].start_mins    = 60;
      s_events[0].duration_mins = 90;
      s_events[0].cal_index     = 0;
  
      // 3:00 → 4:00 (Red)
      s_events[1].start_mins    = 180;
      s_events[1].duration_mins = 60;
      s_events[1].cal_index     = 1;
  
      s_cal_color_idx[0] = 3; // Blue
      s_cal_color_idx[1] = 7; // Red
    #endif

    int hour_angle = (now->tm_hour % 12) * 30 + now->tm_min / 2;
    int min_angle  = now->tm_min * 6 + now->tm_sec / 10;

    int clock_radius = MIN(bounds.size.w, bounds.size.h) / 2 - 10;
    bool is_round = (bounds.size.w == bounds.size.h);

    int hour_len, min_len;
    if (is_round) {
        hour_len = clock_radius * 7 / 10;
        min_len  = clock_radius * 19 / 20;
    } else {
        hour_len = clock_radius * 9 / 10;
        min_len  = clock_radius;
    }

    int weather_x = centre.x + (bounds.size.w - centre.x) / 2;
    int weather_y = centre.y - 10;
    int wx = weather_x - 20, wy = weather_y, ww = 60, wh = 40;

    bool show_shadow = compute_show_shadow(
        bounds, centre, clock_radius, is_round,
        hour_len, min_len, hour_angle, min_angle,
        wx, wy, ww, wh);

    // -- Hour tick marks ---------------------------------------
    // Do not show tick marks on emulator
    #if SCREENSHOT_MODE
        bool show_ticks = false;
    #else
        bool show_ticks = s_show_ticks;
    #endif
    if (show_ticks) {
        int arc_inset = 4;
        graphics_context_set_stroke_color(ctx, GColorWhite);
        for (int h = 0; h < 12; h++) {
            int deg = h * 30;
            bool is_cardinal = (h % 3 == 0);
            int tick_len   = is_cardinal ? 18 : 12;
            int tick_width = is_cardinal ? 4  : 2;
            graphics_context_set_stroke_width(ctx, tick_width);
            if (is_round) {
                int32_t trig = DEG_TO_TRIGANGLE(deg);
                int sv = sin_lookup(trig), cv = cos_lookup(trig);
                GPoint outer = GPoint(
                    centre.x + sv * clock_radius / TRIG_MAX_RATIO,
                    centre.y - cv * clock_radius / TRIG_MAX_RATIO);
                GPoint inner = GPoint(
                    centre.x + sv * (clock_radius - tick_len) / TRIG_MAX_RATIO,
                    centre.y - cv * (clock_radius - tick_len) / TRIG_MAX_RATIO);
                graphics_draw_line(ctx, outer, inner);
            } else {
                GPoint outer = perimeter_point(bounds, arc_inset, deg);
                int dx = centre.x - outer.x, dy = centre.y - outer.y;
                int dist2 = dx * dx + dy * dy, dist = tick_len;
                if (dist2 > 0) {
                    int r = (dist2 > 0x10000) ? 256 : 16;
                    for (int n = 0; n < 8; n++) r = (r + dist2 / r) / 2;
                    dist = r;
                }
                GPoint inner = GPoint(
                    outer.x + (dx * tick_len * 16 / dist) / 16,
                    outer.y + (dy * tick_len * 16 / dist) / 16);
                graphics_draw_line(ctx, outer, inner);
            }
        }
    }

    // -- Calendar arc ring -------------------------------------
    int arc_stroke = 8;
    for (int i = 0; i < s_event_count; i++) {
        CalEvent *ev = &s_events[i];
        if (ev->duration_mins <= 0) continue;
        int start = ev->start_mins;
        int end   = ev->start_mins + ev->duration_mins;
        if (start >= 720) continue;
        if (end   >  720) end = 720;
        int start_deg = start * 360 / 720;
        int end_deg   = end   * 360 / 720;
        int ci = ev->cal_index;
        if (ci < 0 || ci >= MAX_CALENDARS) ci = 0;
        GColor color = palette_color(s_cal_color_idx[ci]);
        graphics_context_set_stroke_color(ctx, color);
        graphics_context_set_stroke_width(ctx, arc_stroke);
        if (is_round) {
            int arc_radius = clock_radius + arc_stroke;
            for (int deg = start_deg; deg < end_deg; deg += 3) {
                int d2 = (deg + 3 < end_deg) ? deg + 3 : end_deg;
                GPoint p1 = GPoint(
                    centre.x + sin_lookup(DEG_TO_TRIGANGLE(deg)) * arc_radius / TRIG_MAX_RATIO,
                    centre.y - cos_lookup(DEG_TO_TRIGANGLE(deg)) * arc_radius / TRIG_MAX_RATIO);
                GPoint p2 = GPoint(
                    centre.x + sin_lookup(DEG_TO_TRIGANGLE(d2)) * arc_radius / TRIG_MAX_RATIO,
                    centre.y - cos_lookup(DEG_TO_TRIGANGLE(d2)) * arc_radius / TRIG_MAX_RATIO);
                graphics_draw_line(ctx, p1, p2);
            }
        } else {
            int inset = arc_stroke / 2;
            for (int deg = start_deg; deg < end_deg; deg += 1) {
                int d2 = (deg + 1 < end_deg) ? deg + 1 : end_deg;
                GPoint p1 = perimeter_point(bounds, inset, deg);
                GPoint p2 = perimeter_point(bounds, inset, d2);
                graphics_draw_line(ctx, p1, p2);
            }
        }
    }

    // -- Hour hand ---------------------------------------------
    graphics_context_set_stroke_color(ctx, palette_color(s_hour_hand_color));
    graphics_context_set_stroke_width(ctx, 5);
    GPoint tip_hour = GPoint(
        centre.x + sin_lookup(DEG_TO_TRIGANGLE(hour_angle)) * hour_len / TRIG_MAX_RATIO,
        centre.y - cos_lookup(DEG_TO_TRIGANGLE(hour_angle)) * hour_len / TRIG_MAX_RATIO);
    graphics_draw_line(ctx, centre, tip_hour);

    // -- Minute hand -------------------------------------------
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, 3);
    GPoint tip_min = GPoint(
        centre.x + sin_lookup(DEG_TO_TRIGANGLE(min_angle)) * min_len / TRIG_MAX_RATIO,
        centre.y - cos_lookup(DEG_TO_TRIGANGLE(min_angle)) * min_len / TRIG_MAX_RATIO);
    graphics_draw_line(ctx, centre, tip_min);

    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, GRect(centre.x - 3, centre.y - 3, 6, 6), 0, GCornerNone);

    // -- Weather -----------------------------------------------
    char weather_str[32] = "--\xc2\xb0" "C";
    if (s_weather_temp != INT_MIN) {
        int  temp = s_weather_temp;
        char unit = s_use_fahrenheit ? 'F' : 'C';
        snprintf(weather_str, sizeof(weather_str), "%d\xc2\xb0%c", temp, unit);
    }
    if (show_shadow) {
        static const int offsets[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
        for (int o = 0; o < 4; o++) {
            graphics_context_set_text_color(ctx, GColorBlack);
            graphics_draw_text(ctx, weather_str, s_small_font,
                GRect(weather_x - 20 + offsets[o][0],
                      weather_y      + offsets[o][1], 60, 20),
                GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
        }
    }
    graphics_context_set_text_color(ctx, s_text_color);
    graphics_draw_text(ctx, weather_str, s_small_font,
        GRect(weather_x - 20, weather_y, 60, 20),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    if (s_weather_code >= 0)
        draw_weather_icon(ctx, s_weather_code, weather_x - 10, weather_y + 18, show_shadow);

    // -- Date -------------------------------------------------
    if (s_show_date) {
        static const char *days[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
        char date_str[16];
          #if SCREENSHOT_MODE
            s_show_ticks = false;
          #endif

        if (s_show_ticks || is_round) {
            // Compact: day-of-month only, placed halfway between
            // centre and the inner edge of the tick ring / screen edge
            // at the 6 o'clock position.
            snprintf(date_str, sizeof(date_str), "%d", now->tm_mday);

            // Determine the bottom reference point:
            //   Round watch  → inner tip of the 6 o'clock cardinal tick
            //   Square watch → bottom perimeter minus the arc inset
            int bottom_edge;
            if (is_round) {
                // Cardinal tick length is 18px; inner tip is clock_radius - 18 below centre
                int tick_len_cardinal = 18;
                bottom_edge = centre.y + (clock_radius - tick_len_cardinal);
            } else {
                // Perimeter at 6 o'clock for square is bounds bottom minus the 4px arc inset
                int arc_inset = 4;
                bottom_edge = bounds.size.h - 1 - arc_inset;
            }

            // Midpoint between centre and that inner edge
            int date_y_mid = (centre.y + bottom_edge) / 2;

            // Centre the text vertically on that midpoint
            // GOTHIC_24_BOLD renders at ~28px tall
            int font_h = 28;
            int date_y = date_y_mid - font_h / 2;

            if (show_shadow) {
                static const int offsets[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
                for (int o = 0; o < 4; o++) {
                    graphics_context_set_text_color(ctx, GColorBlack);
                    graphics_draw_text(ctx, date_str, s_date_font,
                        GRect(offsets[o][0], date_y + offsets[o][1],
                              bounds.size.w, font_h),
                        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
                }
            }
            graphics_context_set_text_color(ctx, s_text_color);
            graphics_draw_text(ctx, date_str, s_date_font,
                GRect(0, date_y, bounds.size.w, font_h),
                GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

        } else {
            // No ticks, square watch: full "Day Mon DD" label at the bottom
            snprintf(date_str, sizeof(date_str), "%s %s %02d",
                days[now->tm_wday], months[now->tm_mon], now->tm_mday);
            graphics_draw_text(ctx, date_str, s_date_font,
                GRect(0, bounds.size.h - 24, bounds.size.w, 24),
                GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
        }
    }
}

// --- Tick handler ---------------------------------------------
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    layer_mark_dirty(s_canvas_layer);
}

// --- Parse helper ---------------------------------------------
static int parse_int(const char **p) {
    int val = 0;
    while (**p >= '0' && **p <= '9') {
        val = val * 10 + (**p - '0');
        (*p)++;
    }
    return val;
}

// --- AppMessage -----------------------------------------------
static void inbox_received_handler(DictionaryIterator *iter, void *context) {
    Tuple *tu = dict_find(iter, KEY_TEMPERATURE_UNIT);
    if (tu) s_use_fahrenheit = (tu->value->int32 != 0);

    Tuple *sd = dict_find(iter, KEY_SHOW_DATE);
    if (sd) s_show_date = (sd->value->int32 != 0);

    Tuple *st = dict_find(iter, KEY_SHOW_TICKS);
    if (st) s_show_ticks = (st->value->int32 != 0);

    Tuple *hh = dict_find(iter, KEY_HOUR_HAND_COLOR);
    if (hh) s_hour_hand_color = (int)hh->value->int32;

    // Calendar colours packed as "c0,c1,c2,...,c9"
    Tuple *cc = dict_find(iter, KEY_CAL_COLORS);
    if (cc) {
        const char *p = cc->value->cstring;
        for (int i = 0; i < MAX_CALENDARS; i++) {
            s_cal_color_idx[i] = parse_int(&p);
            if (*p == ',') p++;
        }
    }

    Tuple *ce = dict_find(iter, KEY_CALENDAR_EVENTS);
    if (ce) {
        s_event_count = 0;
        const char *p = ce->value->cstring;
        while (*p && s_event_count < MAX_EVENTS) {
            int sm = parse_int(&p); if (*p == ',') p++;
            int dm = parse_int(&p); if (*p == ',') p++;
            int ci = parse_int(&p);
            s_events[s_event_count].start_mins    = sm;
            s_events[s_event_count].duration_mins = dm;
            s_events[s_event_count].cal_index     = ci;
            s_event_count++;
            if (*p == '|') p++; else break;
        }
    }

    Tuple *wt = dict_find(iter, KEY_WEATHER_TEMP);
    if (wt) s_weather_temp = wt->value->int32;

    Tuple *wc = dict_find(iter, KEY_WEATHER_CODE);
    if (wc) s_weather_code = wc->value->int32;

    layer_mark_dirty(s_canvas_layer);
}

static void inbox_dropped_handler(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped: %d", (int)reason);
}

// --- Window lifecycle -----------------------------------------
static void window_load(Window *window) {
    Layer *root   = window_get_root_layer(window);
    GRect  bounds = layer_get_bounds(root);
    s_canvas_layer = layer_create(bounds);
    layer_set_update_proc(s_canvas_layer, canvas_update_proc);
    layer_add_child(root, s_canvas_layer);
}

static void window_unload(Window *window) {
    layer_destroy(s_canvas_layer);
}

// --- Init / Deinit -------------------------------------------
static void init(void) {
    s_small_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
    s_date_font  = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers){
        .load   = window_load,
        .unload = window_unload
    });
    window_stack_push(s_window, true);
    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
    app_message_register_inbox_received(inbox_received_handler);
    app_message_register_inbox_dropped(inbox_dropped_handler);
    app_message_open(512, 512);
}

static void deinit(void) {
    tick_timer_service_unsubscribe();
    window_destroy(s_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
    return 0;
}