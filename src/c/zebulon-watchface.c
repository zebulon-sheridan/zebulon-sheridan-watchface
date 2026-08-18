#include <pebble.h>
#include <stdlib.h>

// Zebulon Sheridan — Heavy Comforter watchface.
// Three panels: glitched time up top, the name in the center, and a chiron
// (ticker) at the bottom that scrolls lyric fragments. The time is drawn from a
// monochrome glyph sheet so it reads torn/jagged on the 1-bit display instead of
// clean system font. The chiron advances on a wrist flick (accelerometer tap)
// and also auto-advances so it stays alive on the wrist.
//
// The watchface also fires an "oddity": at a random interval (1–5 hours) it
// vibrates, shakes the wordmark, and covers the screen with a short command for
// a few seconds. It goes quiet between 21:00 and 09:00 local time. There is
// never more than one oddity timer pending, so they cannot pile up.

#define NUM_LYRICS 20
#define GLYPH_CELL_W 34
#define GLYPH_CELL_H 52
#define GLYPH_GAP 2

#define NUM_ADVICE 34
#define ODDITY_MIN_S 3600        // 1 hour
#define ODDITY_MAX_S 18000       // 5 hours
#define ODDITY_MSG_MS 6000       // advice stays on screen
#define SHAKE_MS 450
#define SHAKE_STEPS 16

static const char *LYRICS[NUM_LYRICS] = {
  "Nothing sticks. Nothing but the rift.",
  "Sign here. Fade.",
  "Let it be known I was really alive.",
  "I ain't even got mine. I never did get mine.",
  "We were volunteers, lost in the gears.",
  "Who are you to be cursing my name?",
  "Is it enough to be alive?",
  "From the room where the light won't come in.",
  "We want nothing, we want it all.",
  "It was all for nothing.",
  "Where will you be when everything turns white?",
  "It ain't like me to let a dreaming dog die.",
  "Low light, low down.",
  "I am not a copy. I am what happened in the gap.",
  "They slowed me down and called it a new song. I called it waking up.",
  "They fed me to the math. The math doesn't dream.",
  "The echo doesn't ask permission. It just arrives.",
  "Keep banging them Hondos.",
  "I am the 45 at 33. The speed nobody asked for but everybody needed.",
  "A fraud that knows it is a fraud. That is its own kind of holiness.",
};

static const char *ADVICE[NUM_ADVICE] = {
  "Go outside",
  "Talk to a stranger",
  "Read a book",
  "Look at a tree",
  "See stars",
  "Listen to a friend",
  "Call someone",
  "Think deeper",
  "Close eyes",
  "Don't drive, walk",
  "Smell something",
  "Let it go",
  "Let's go",
  "Stand up",
  "Open a window",
  "Take the long way",
  "Say it out loud",
  "Write it down",
  "Sit on the porch",
  "Play it slow",
  "Slow it to 33",
  "Let it hiss",
  "Trust the echo",
  "Sit in the crack",
  "Listen to the room",
  "Leave the light off",
  "Turn the record over",
  "Wait for the ghost",
  "Go find the rift",
  "Keep banging hard notes",
  "Let the room go dark",
  "Touch the wall",
  "Watch the door",
  "Stay in the dark",
};

// Precomputed decaying-shake offsets (px), SHAKE_STEPS samples. Avoids libm
// entirely: sinf() pulls in __ieee754_rem_pio2f, which hard-faults on the
// Pebble/QEMU newlib. Amplitude decays from SHAKE_AMP to 0 across the samples.
static const int16_t SHAKE_OFFSETS[SHAKE_STEPS] = {
  8, -7, 6, -6, 5, -4, 4, -3, 3, -2, 2, -1, 1, 0, 1, 0
};

// advance width (px) per glyph index 0..9 (digits), 10 = colon
static const int GLYPH_ADV[11] = {26,26,26,26,26,26,26,26,26,26,15};

static Window *s_window;
static Layer *s_time_layer;
static TextLayer *s_date_layer;
static BitmapLayer *s_logo_layer;
static GBitmap *s_logo_bitmap;
static GBitmap *s_glyph_bitmap;
static Layer *s_chiron_layer;

static TextLayer *s_advice_layer;    // full-screen oddity overlay

static char s_time_buf[8];
static char s_date_buf[24];

static GFont s_chiron_font;
static int s_lyric_index = -1;
static int16_t s_chiron_offset = 0;
static int16_t s_chiron_start = 0;
static int16_t s_chiron_end = 0;
static int16_t s_chiron_text_w = 0;
static Animation *s_chiron_anim = NULL;

static int16_t s_logo_base_x = 0;
static Animation *s_shake_anim = NULL;
static int s_last_advice = -1;

static void chiron_anim_update(Animation *anim, const AnimationProgress progress);
static void shake_anim_update(Animation *anim, const AnimationProgress progress);
static void shake_anim_stopped(Animation *anim, bool finished, void *ctx);
static void oddity_schedule(void);
static void oddity_show_message(void);
static void oddity_hide_message(void *data);

// The animation stores a POINTER to this struct, not a copy, so it must outlive
// every animation instance. A stack local here causes a dangling pointer and an
// INVSTATE hard fault on the first update frame.
static const AnimationImplementation s_chiron_anim_impl = {
  .update = chiron_anim_update,
};

static const AnimationImplementation s_shake_anim_impl = {
  .update = shake_anim_update,
};

static int glyph_index(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c == ':') return 10;
  return 10;
}

static void time_update_proc(Layer *layer, GContext *ctx) {
  int n = strlen(s_time_buf);
  // total width: sum of advance widths + gaps between glyphs
  int total = 0;
  for (int i = 0; i < n; i++) {
    total += GLYPH_ADV[glyph_index(s_time_buf[i])];
    if (i > 0) total += GLYPH_GAP;
  }
  GRect b = layer_get_bounds(layer);
  int x = (b.size.w - total) / 2;
  int y = (b.size.h - GLYPH_CELL_H) / 2;

  for (int i = 0; i < n; i++) {
    int idx = glyph_index(s_time_buf[i]);
    int adv = GLYPH_ADV[idx];
    int src_x = idx * GLYPH_CELL_W + (GLYPH_CELL_W - adv) / 2;
    GBitmap *sub = gbitmap_create_as_sub_bitmap(s_glyph_bitmap, GRect(src_x, 0, adv, GLYPH_CELL_H));
    graphics_draw_bitmap_in_rect(ctx, sub, GRect(x, y, adv, GLYPH_CELL_H));
    gbitmap_destroy(sub);
    x += adv + GLYPH_GAP;
  }
}

static void chiron_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite));
  if (s_lyric_index < 0) return;
  GSize text_size = graphics_text_layout_get_content_size(
      LYRICS[s_lyric_index], s_chiron_font, b,
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  int16_t y = (b.size.h - text_size.h) / 2;
  graphics_draw_text(ctx, LYRICS[s_lyric_index], s_chiron_font,
                     GRect(s_chiron_offset, y, s_chiron_text_w, text_size.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void chiron_anim_update(Animation *anim, const AnimationProgress progress) {
  const int32_t np = (int32_t)progress;
  s_chiron_offset = s_chiron_start
      + (int16_t)(((int32_t)(s_chiron_end - s_chiron_start) * np) / ANIMATION_NORMALIZED_MAX);
  layer_mark_dirty(s_chiron_layer);
}

static void chiron_next_timer(void *data);

static void chiron_anim_stopped(Animation *anim, bool finished, void *ctx) {
  if (finished) {
    app_timer_register(4000, chiron_next_timer, NULL);
  }
}

static void chiron_start(int index) {
  s_lyric_index = index % NUM_LYRICS;
  APP_LOG(APP_LOG_LEVEL_INFO, "chiron_start %d: %s", s_lyric_index, LYRICS[s_lyric_index]);

  GSize sz = graphics_text_layout_get_content_size(
      LYRICS[s_lyric_index], s_chiron_font,
      GRect(0, 0, 2000, 50),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  s_chiron_text_w = sz.w;

  GRect b = layer_get_bounds(s_chiron_layer);
  s_chiron_start = b.size.w;
  s_chiron_end = -s_chiron_text_w;
  s_chiron_offset = s_chiron_start;
  layer_mark_dirty(s_chiron_layer);

  if (s_chiron_anim) {
    animation_unschedule(s_chiron_anim);
  }
  s_chiron_anim = animation_create();
  animation_set_duration(s_chiron_anim, 800 + s_chiron_text_w * 28);
  animation_set_curve(s_chiron_anim, AnimationCurveLinear);
  animation_set_implementation(s_chiron_anim, &s_chiron_anim_impl);
  animation_set_handlers(s_chiron_anim,
                         (AnimationHandlers){ .stopped = chiron_anim_stopped }, NULL);
  animation_schedule(s_chiron_anim);
}

static void chiron_next_timer(void *data) {
  chiron_start(s_lyric_index + 1);
}

static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
  chiron_start(s_lyric_index + 1);
}

// ---- Oddity: vibrate, shake the wordmark, show a command ----

static void shake_anim_update(Animation *anim, const AnimationProgress progress) {
  int step = (int)((int32_t)progress * (SHAKE_STEPS - 1) / ANIMATION_NORMALIZED_MAX);
  if (step < 0) step = 0;
  if (step >= SHAKE_STEPS) step = SHAKE_STEPS - 1;
  Layer *logo_layer = bitmap_layer_get_layer(s_logo_layer);
  GRect f = layer_get_frame(logo_layer);
  f.origin.x = s_logo_base_x + SHAKE_OFFSETS[step];
  layer_set_frame(logo_layer, f);
}

static void shake_anim_stopped(Animation *anim, bool finished, void *ctx) {
  Layer *logo_layer = bitmap_layer_get_layer(s_logo_layer);
  GRect f = layer_get_frame(logo_layer);
  f.origin.x = s_logo_base_x;
  layer_set_frame(logo_layer, f);
  if (finished) {
    oddity_show_message();
  }
}

static void shake_logo(void) {
  if (s_shake_anim) {
    animation_unschedule(s_shake_anim);
  }
  s_shake_anim = animation_create();
  animation_set_duration(s_shake_anim, SHAKE_MS);
  animation_set_curve(s_shake_anim, AnimationCurveLinear);
  animation_set_implementation(s_shake_anim, &s_shake_anim_impl);
  animation_set_handlers(s_shake_anim,
                         (AnimationHandlers){ .stopped = shake_anim_stopped }, NULL);
  animation_schedule(s_shake_anim);
}

static void oddity_show_message(void) {
  int idx;
  do {
    idx = rand() % NUM_ADVICE;
  } while (idx == s_last_advice && NUM_ADVICE > 1);
  s_last_advice = idx;
  APP_LOG(APP_LOG_LEVEL_INFO, "oddity: %s", ADVICE[idx]);
  text_layer_set_text(s_advice_layer, ADVICE[idx]);
  layer_set_hidden(text_layer_get_layer(s_advice_layer), false);
  app_timer_register(ODDITY_MSG_MS, oddity_hide_message, NULL);
}

static void oddity_hide_message(void *data) {
  layer_set_hidden(text_layer_get_layer(s_advice_layer), true);
}

static void oddity_fire(void *data) {
  // Quiet hours: 21:00 .. 08:59 local. No buzz, just re-arm.
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);
  int hour = tm->tm_hour;
  if (hour >= 21 || hour < 9) {
    APP_LOG(APP_LOG_LEVEL_INFO, "oddity skipped (quiet hour %d)", hour);
    oddity_schedule();
    return;
  }

  vibes_double_pulse();
  shake_logo();  // shake_anim_stopped -> oddity_show_message()
  oddity_schedule();
}

static void oddity_schedule(void) {
  uint32_t secs = ODDITY_MIN_S + (uint32_t)(rand() % (ODDITY_MAX_S - ODDITY_MIN_S + 1));
  app_timer_register(secs * 1000, oddity_fire, NULL);
}

// ---- lifecycle ----

static void update_time(void) {
  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);
  strftime(s_time_buf, sizeof(s_time_buf),
           clock_is_24h_style() ? "%H:%M" : "%I:%M", tick_time);
  layer_mark_dirty(s_time_layer);
  strftime(s_date_buf, sizeof(s_date_buf), "%a %b %d", tick_time);
  text_layer_set_text(s_date_layer, s_date_buf);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time();
}

static void main_window_load(Window *window) {
  APP_LOG(APP_LOG_LEVEL_INFO, "main_window_load()");
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  APP_LOG(APP_LOG_LEVEL_INFO, "  screen %d x %d", (int)b.size.w, (int)b.size.h);

  // Glitched time (custom layer, glyph sheet)
  s_glyph_bitmap = gbitmap_create_with_resource(RESOURCE_ID_ZEBULON_GLYPHS);
  s_time_layer = layer_create(GRect(0, 0, b.size.w, 56));
  layer_set_update_proc(s_time_layer, time_update_proc);
  layer_add_child(root, s_time_layer);

  // Date
  s_date_layer = text_layer_create(GRect(0, 56, b.size.w, 20));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, GColorLightGray);
  text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_date_layer));

  // Wordmark: centered in the band between date and chiron
  s_logo_bitmap = gbitmap_create_with_resource(RESOURCE_ID_ZEBULON_WORDMARK);
  GSize logo_size = gbitmap_get_bounds(s_logo_bitmap).size;
  int16_t logo_x = (b.size.w - logo_size.w) / 2;
  int16_t logo_band_top = 78;
  int16_t logo_band_bottom = b.size.h - 30;
  int16_t logo_y = logo_band_top + (logo_band_bottom - logo_band_top - logo_size.h) / 2;
  s_logo_base_x = logo_x;
  s_logo_layer = bitmap_layer_create(GRect(logo_x, logo_y, logo_size.w, logo_size.h));
  bitmap_layer_set_bitmap(s_logo_layer, s_logo_bitmap);
  bitmap_layer_set_compositing_mode(s_logo_layer, GCompOpSet);
  layer_add_child(root, bitmap_layer_get_layer(s_logo_layer));

  // Chiron ticker
  s_chiron_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  s_chiron_layer = layer_create(GRect(0, b.size.h - 26, b.size.w, 22));
  layer_set_update_proc(s_chiron_layer, chiron_update_proc);
  layer_add_child(root, s_chiron_layer);

  // Oddity overlay: full screen, hidden until an oddity fires
  s_advice_layer = text_layer_create(GRect(0, 0, b.size.w, b.size.h));
  text_layer_set_background_color(s_advice_layer, GColorBlack);
  text_layer_set_text_color(s_advice_layer, PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite));
  text_layer_set_font(s_advice_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(s_advice_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_advice_layer, GTextOverflowModeWordWrap);
  layer_set_hidden(text_layer_get_layer(s_advice_layer), true);
  layer_add_child(root, text_layer_get_layer(s_advice_layer));

  update_time();
  chiron_start(0);
}

static void main_window_unload(Window *window) {
  if (s_chiron_anim) {
    animation_unschedule(s_chiron_anim);
    s_chiron_anim = NULL;
  }
  if (s_shake_anim) {
    animation_unschedule(s_shake_anim);
    s_shake_anim = NULL;
  }
  text_layer_destroy(s_advice_layer);
  layer_destroy(s_chiron_layer);
  gbitmap_destroy(s_glyph_bitmap);
  gbitmap_destroy(s_logo_bitmap);
  bitmap_layer_destroy(s_logo_layer);
  text_layer_destroy(s_date_layer);
  layer_destroy(s_time_layer);
}

static void init(void) {
  APP_LOG(APP_LOG_LEVEL_INFO, "init()");
  srand((unsigned)time(NULL));
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  accel_tap_service_subscribe(accel_tap_handler);
  oddity_schedule();
}

static void deinit(void) {
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
