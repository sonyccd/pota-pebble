#include <pebble.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */

#define MAX_SPOTS          30
#define NUM_BANDS          13
#define NUM_MODES          8
#define PERSIST_KEY_BANDS  1
#define PERSIST_KEY_MODES  2
#define DEFAULT_BAND_MASK  0x1FFFu   // all 13 bands enabled
#define DEFAULT_MODE_MASK  0x00FFu   // all 8 modes enabled

static const char *BAND_NAMES[NUM_BANDS] = {
  "160m", "80m", "60m", "40m", "30m", "20m",
  "17m",  "15m", "12m", "10m", "6m",  "2m", "70cm"
};
static const char *MODE_NAMES[NUM_MODES] = {
  "SSB", "CW", "FT8", "FT4", "RTTY", "AM", "FM", "Digital"
};

/* ------------------------------------------------------------------ */
/*  Data types                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
  char spot_id[16];
  char callsign[16];
  char frequency[12];  // kHz string, e.g. "14285.0"
  char mode[8];
  char park_ref[12];
  char location[64];   // park name
  char comment[64];
  uint32_t timestamp;  // Unix epoch (seconds)
} Spot;

/* ------------------------------------------------------------------ */
/*  Module state                                                        */
/* ------------------------------------------------------------------ */

/* Spot data */
static Spot    s_spots[MAX_SPOTS];
static int     s_spot_count = 0;
static Spot    s_staging[MAX_SPOTS];
static int     s_staging_total = 0;
static char    s_selected_spot_id[16];

/* Filter settings (mirrored from PKJS localStorage) */
static uint32_t s_band_mask;
static uint32_t s_mode_mask;

/* Window / layer pointers */
static Window    *s_main_window;
static MenuLayer *s_main_menu_layer;

static Window    *s_spots_window;
static MenuLayer *s_spots_menu_layer;
static Layer     *s_status_layer;
static bool       s_spots_visible   = false;
static bool       s_bt_connected    = true;
static bool       s_received_batch  = false;

static Window       *s_detail_window;
static ScrollLayer  *s_detail_scroll;
static TextLayer    *s_detail_text;
static Spot          s_detail_spot;
static char          s_detail_buf[320];

static Window    *s_settings_window;
static MenuLayer *s_settings_menu_layer;

static Window    *s_bands_window;
static MenuLayer *s_bands_menu_layer;

static Window    *s_modes_window;
static MenuLayer *s_modes_menu_layer;

/* ------------------------------------------------------------------ */
/*  AppMessage helpers                                                  */
/* ------------------------------------------------------------------ */

static void prv_send_key(uint32_t key) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_uint8(iter, key, 1);
    app_message_outbox_send();
  }
}

static void prv_send_settings(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_uint8(iter,  MESSAGE_KEY_SETTINGS_UPDATE, 1);
    dict_write_uint32(iter, MESSAGE_KEY_BAND_MASK, s_band_mask);
    dict_write_uint32(iter, MESSAGE_KEY_MODE_MASK, s_mode_mask);
    app_message_outbox_send();
  }
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;

  /* SETTINGS_SYNC — PKJS sends current filter state on app launch */
  t = dict_find(iter, MESSAGE_KEY_SETTINGS_SYNC);
  if (t) {
    Tuple *bm = dict_find(iter, MESSAGE_KEY_BAND_MASK);
    Tuple *mm = dict_find(iter, MESSAGE_KEY_MODE_MASK);
    if (bm) {
      s_band_mask = bm->value->uint32;
      persist_write_int(PERSIST_KEY_BANDS, (int32_t)s_band_mask);
    }
    if (mm) {
      s_mode_mask = mm->value->uint32;
      persist_write_int(PERSIST_KEY_MODES, (int32_t)s_mode_mask);
    }
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Settings synced bands=%lu modes=%lu",
            (unsigned long)s_band_mask, (unsigned long)s_mode_mask);
    return;
  }

  /* SPOTS_BATCH_START — value is the total spot count */
  t = dict_find(iter, MESSAGE_KEY_SPOTS_BATCH_START);
  if (t) {
    s_staging_total = (int)t->value->int32;
    if (s_staging_total > MAX_SPOTS) s_staging_total = MAX_SPOTS;
    memset(s_staging, 0, sizeof(Spot) * s_staging_total);
    return;
  }

  /* SPOTS_BATCH_ITEM — one spot per message */
  t = dict_find(iter, MESSAGE_KEY_SPOTS_BATCH_ITEM);
  if (t) {
    Tuple *idx_t = dict_find(iter, MESSAGE_KEY_SPOT_INDEX);
    if (!idx_t) return;
    int idx = (int)idx_t->value->int32;
    if (idx < 0 || idx >= MAX_SPOTS) return;
    Spot *sp = &s_staging[idx];
    Tuple *f;
    f = dict_find(iter, MESSAGE_KEY_SPOT_ID);
    if (f) snprintf(sp->spot_id,   sizeof(sp->spot_id),   "%s", f->value->cstring);
    f = dict_find(iter, MESSAGE_KEY_SPOT_CALLSIGN);
    if (f) snprintf(sp->callsign,  sizeof(sp->callsign),  "%s", f->value->cstring);
    f = dict_find(iter, MESSAGE_KEY_SPOT_FREQ);
    if (f) snprintf(sp->frequency, sizeof(sp->frequency), "%s", f->value->cstring);
    f = dict_find(iter, MESSAGE_KEY_SPOT_MODE);
    if (f) snprintf(sp->mode,      sizeof(sp->mode),      "%s", f->value->cstring);
    f = dict_find(iter, MESSAGE_KEY_SPOT_PARK_REF);
    if (f) snprintf(sp->park_ref,  sizeof(sp->park_ref),  "%s", f->value->cstring);
    f = dict_find(iter, MESSAGE_KEY_SPOT_LOCATION);
    if (f) snprintf(sp->location,  sizeof(sp->location),  "%s", f->value->cstring);
    f = dict_find(iter, MESSAGE_KEY_SPOT_COMMENT);
    if (f) snprintf(sp->comment,   sizeof(sp->comment),   "%s", f->value->cstring);
    f = dict_find(iter, MESSAGE_KEY_SPOT_TIMESTAMP);
    if (f) sp->timestamp = f->value->uint32;
    return;
  }

  /* SPOTS_BATCH_END — swap staging → live, refresh UI */
  t = dict_find(iter, MESSAGE_KEY_SPOTS_BATCH_END);
  if (t) {
    int new_count = 0;
    Tuple *nc = dict_find(iter, MESSAGE_KEY_NEW_SPOT_COUNT);
    if (nc) new_count = (int)nc->value->int32;

    memcpy(s_spots, s_staging, sizeof(Spot) * s_staging_total);
    s_spot_count = s_staging_total;

    if (s_spots_menu_layer) {
      menu_layer_reload_data(s_spots_menu_layer);
      if (s_selected_spot_id[0] != '\0') {
        for (int i = 0; i < s_spot_count; i++) {
          if (strncmp(s_spots[i].spot_id, s_selected_spot_id,
                      sizeof(s_selected_spot_id)) == 0) {
            MenuIndex new_idx = {.section = 0, .row = (uint16_t)i};
            menu_layer_set_selected_index(s_spots_menu_layer, new_idx,
                                          MenuRowAlignCenter, false);
            break;
          }
        }
      }
    }
    if (new_count > 0) {
      vibes_short_pulse();
    }
    s_received_batch = true;
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Batch: %d spots, %d new", s_spot_count, new_count);
    return;
  }
}

/* ------------------------------------------------------------------ */
/*  Frequency formatting                                                */
/* ------------------------------------------------------------------ */

/* "14285.0" kHz -> "14.285" MHz (integer arithmetic, no float) */
static void prv_format_freq_mhz(char *buf, size_t len, const char *khz_str) {
  long khz = atol(khz_str);
  snprintf(buf, len, "%ld.%03ld", khz / 1000, khz % 1000);
}

/* ------------------------------------------------------------------ */
/*  Detail window                                                       */
/* ------------------------------------------------------------------ */

static void prv_format_detail(void) {
  time_t now = time(NULL);
  int age_min = (int)((now - (time_t)s_detail_spot.timestamp) / 60);
  if (age_min < 0) age_min = 0;
  char mhz[12];
  prv_format_freq_mhz(mhz, sizeof(mhz), s_detail_spot.frequency);
  snprintf(s_detail_buf, sizeof(s_detail_buf),
    "Call: %s\n"
    "Park: %s\n"
    "Name: %s\n"
    "Freq: %s MHz\n"
    "Mode: %s\n"
    "Cmnt: %s\n"
    "Age:  %dm ago",
    s_detail_spot.callsign,
    s_detail_spot.park_ref,
    s_detail_spot.location,
    mhz,
    s_detail_spot.mode,
    s_detail_spot.comment[0] ? s_detail_spot.comment : "(none)",
    age_min
  );
}

static void prv_detail_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  prv_format_detail();
  s_detail_scroll = scroll_layer_create(bounds);
  scroll_layer_set_click_config_onto_window(s_detail_scroll, window);
  s_detail_text = text_layer_create(GRect(4, 4, bounds.size.w - 8, 1000));
  text_layer_set_text(s_detail_text, s_detail_buf);
  text_layer_set_font(s_detail_text, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_detail_text, GTextOverflowModeWordWrap);
  GSize content = text_layer_get_content_size(s_detail_text);
  int text_h = content.h + 4;
  layer_set_frame(text_layer_get_layer(s_detail_text),
                  GRect(4, 4, bounds.size.w - 8, text_h));
  scroll_layer_set_content_size(s_detail_scroll, GSize(bounds.size.w, text_h + 24));
  scroll_layer_add_child(s_detail_scroll, text_layer_get_layer(s_detail_text));
  layer_add_child(root, scroll_layer_get_layer(s_detail_scroll));
}

static void prv_detail_window_unload(Window *window) {
  text_layer_destroy(s_detail_text);
  scroll_layer_destroy(s_detail_scroll);
  window_destroy(window);
  s_detail_window = NULL;
}

static void prv_push_detail_window(int spot_idx) {
  s_detail_spot = s_spots[spot_idx];
  s_detail_window = window_create();
  window_set_window_handlers(s_detail_window, (WindowHandlers) {
    .load   = prv_detail_window_load,
    .unload = prv_detail_window_unload,
  });
  window_stack_push(s_detail_window, true);
}

/* ------------------------------------------------------------------ */
/*  Shared checkbox row renderer                                        */
/* ------------------------------------------------------------------ */

static void prv_draw_checkbox_row(GContext *ctx, const Layer *cell_layer,
                                   const char *label, bool checked) {
  GRect bounds = layer_get_bounds(cell_layer);
  bool hi = menu_cell_layer_is_highlighted(cell_layer);
  GColor fg = hi ? GColorWhite : GColorBlack;

  int16_t cb_size = 14;
  int16_t cb_x = bounds.size.w - cb_size - 6;
  int16_t cb_y = (bounds.size.h - cb_size) / 2;
  GRect cb = GRect(cb_x, cb_y, cb_size, cb_size);
  graphics_context_set_stroke_color(ctx, fg);
  graphics_draw_rect(ctx, cb);
  if (checked) {
    GRect inner = GRect(cb_x + 3, cb_y + 3, cb_size - 6, cb_size - 6);
    graphics_context_set_fill_color(ctx, fg);
    graphics_fill_rect(ctx, inner, 0, GCornerNone);
  }

  GRect text_rect = GRect(6, 0, cb_x - 10, bounds.size.h);
  graphics_context_set_text_color(ctx, fg);
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     text_rect, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
}

/* ------------------------------------------------------------------ */
/*  Bands window                                                        */
/* ------------------------------------------------------------------ */

static uint16_t prv_bands_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return NUM_BANDS;
}

static void prv_bands_draw_row(GContext *ctx, const Layer *cell_layer,
                                MenuIndex *idx, void *context) {
  int i = (int)idx->row;
  prv_draw_checkbox_row(ctx, cell_layer, BAND_NAMES[i], (s_band_mask >> i) & 1);
}

static void prv_bands_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  s_band_mask ^= (1u << idx->row);
  persist_write_int(PERSIST_KEY_BANDS, (int32_t)s_band_mask);
  prv_send_settings();
  menu_layer_reload_data(ml);
}

static void prv_bands_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_bands_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_bands_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_bands_get_num_rows,
    .draw_row     = prv_bands_draw_row,
    .select_click = prv_bands_select,
  });
  menu_layer_set_click_config_onto_window(s_bands_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_bands_menu_layer));
}

static void prv_bands_window_unload(Window *window) {
  menu_layer_destroy(s_bands_menu_layer);
  s_bands_menu_layer = NULL;
  window_destroy(window);
  s_bands_window = NULL;
}

static void prv_push_bands_window(void) {
  s_bands_window = window_create();
  window_set_window_handlers(s_bands_window, (WindowHandlers) {
    .load   = prv_bands_window_load,
    .unload = prv_bands_window_unload,
  });
  window_stack_push(s_bands_window, true);
}

/* ------------------------------------------------------------------ */
/*  Modes window                                                        */
/* ------------------------------------------------------------------ */

static uint16_t prv_modes_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return NUM_MODES;
}

static void prv_modes_draw_row(GContext *ctx, const Layer *cell_layer,
                                MenuIndex *idx, void *context) {
  int i = (int)idx->row;
  prv_draw_checkbox_row(ctx, cell_layer, MODE_NAMES[i], (s_mode_mask >> i) & 1);
}

static void prv_modes_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  s_mode_mask ^= (1u << idx->row);
  persist_write_int(PERSIST_KEY_MODES, (int32_t)s_mode_mask);
  prv_send_settings();
  menu_layer_reload_data(ml);
}

static void prv_modes_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_modes_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_modes_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_modes_get_num_rows,
    .draw_row     = prv_modes_draw_row,
    .select_click = prv_modes_select,
  });
  menu_layer_set_click_config_onto_window(s_modes_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_modes_menu_layer));
}

static void prv_modes_window_unload(Window *window) {
  menu_layer_destroy(s_modes_menu_layer);
  s_modes_menu_layer = NULL;
  window_destroy(window);
  s_modes_window = NULL;
}

static void prv_push_modes_window(void) {
  s_modes_window = window_create();
  window_set_window_handlers(s_modes_window, (WindowHandlers) {
    .load   = prv_modes_window_load,
    .unload = prv_modes_window_unload,
  });
  window_stack_push(s_modes_window, true);
}

/* ------------------------------------------------------------------ */
/*  Settings window                                                     */
/* ------------------------------------------------------------------ */

static uint16_t prv_settings_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return 2;
}

static void prv_settings_draw_row(GContext *ctx, const Layer *cell_layer,
                                   MenuIndex *idx, void *context) {
  switch (idx->row) {
    case 0: menu_cell_basic_draw(ctx, cell_layer, "Bands", NULL, NULL); break;
    case 1: menu_cell_basic_draw(ctx, cell_layer, "Modes", NULL, NULL); break;
  }
}

static void prv_settings_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  switch (idx->row) {
    case 0: prv_push_bands_window(); break;
    case 1: prv_push_modes_window(); break;
  }
}

static void prv_settings_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_settings_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_settings_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_settings_get_num_rows,
    .draw_row     = prv_settings_draw_row,
    .select_click = prv_settings_select,
  });
  menu_layer_set_click_config_onto_window(s_settings_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_settings_menu_layer));
}

static void prv_settings_window_unload(Window *window) {
  menu_layer_destroy(s_settings_menu_layer);
  s_settings_menu_layer = NULL;
  window_destroy(window);
  s_settings_window = NULL;
}

static void prv_push_settings_window(void) {
  s_settings_window = window_create();
  window_set_window_handlers(s_settings_window, (WindowHandlers) {
    .load   = prv_settings_window_load,
    .unload = prv_settings_window_unload,
  });
  window_stack_push(s_settings_window, true);
}

/* ------------------------------------------------------------------ */
/*  Bluetooth connection handling                                       */
/* ------------------------------------------------------------------ */

static void prv_status_layer_draw(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "No phone",
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     bounds, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

static void prv_bt_handler(bool connected) {
  s_bt_connected = connected;
  if (s_status_layer) {
    layer_set_hidden(s_status_layer, connected);
  }
  /* Resume polling when the phone reconnects while Spots is on screen */
  if (connected && s_spots_visible) {
    prv_send_key(MESSAGE_KEY_POLL_START);
  }
}

/* ------------------------------------------------------------------ */
/*  Spots window                                                        */
/* ------------------------------------------------------------------ */

static uint16_t prv_spots_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return (uint16_t)(s_spot_count > 0 ? s_spot_count : 1);
}

static void prv_spots_draw_row(GContext *ctx, const Layer *cell_layer,
                               MenuIndex *idx, void *context) {
  if (s_spot_count == 0) {
    const char *sub = s_received_batch ? "Check filters" : "Fetching...";
    menu_cell_basic_draw(ctx, cell_layer, "No spots", sub, NULL);
    return;
  }
  int i = (int)idx->row;
  if (i >= s_spot_count) return;
  char title[32], sub[20], mhz[12];
  prv_format_freq_mhz(mhz, sizeof(mhz), s_spots[i].frequency);
  snprintf(title, sizeof(title), "%s  %s", s_spots[i].callsign, s_spots[i].park_ref);
  snprintf(sub,   sizeof(sub),   "%s  %s", mhz, s_spots[i].mode);
  menu_cell_basic_draw(ctx, cell_layer, title, sub, NULL);
}

static void prv_spots_selection_changed(MenuLayer *ml, MenuIndex new_idx,
                                        MenuIndex old_idx, void *ctx) {
  int i = (int)new_idx.row;
  if (i < s_spot_count) {
    snprintf(s_selected_spot_id, sizeof(s_selected_spot_id), "%s", s_spots[i].spot_id);
  }
}

static void prv_spots_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (s_spot_count == 0) return;
  int i = (int)idx->row;
  if (i >= s_spot_count) return;
  prv_push_detail_window(i);
}

static void prv_spots_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_spots_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_spots_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows      = prv_spots_get_num_rows,
    .draw_row          = prv_spots_draw_row,
    .select_click      = prv_spots_select,
    .selection_changed = prv_spots_selection_changed,
  });
  menu_layer_set_click_config_onto_window(s_spots_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_spots_menu_layer));

  /* Status banner — sits on top of the menu, hidden while connected */
  s_status_layer = layer_create(GRect(0, 0, bounds.size.w, 16));
  layer_set_update_proc(s_status_layer, prv_status_layer_draw);
  layer_set_hidden(s_status_layer, s_bt_connected);
  layer_add_child(root, s_status_layer);
}

static void prv_spots_window_unload(Window *window) {
  layer_destroy(s_status_layer);
  s_status_layer = NULL;
  menu_layer_destroy(s_spots_menu_layer);
  s_spots_menu_layer = NULL;
  window_destroy(window);
  s_spots_window = NULL;
}

static void prv_spots_window_appear(Window *window) {
  s_spots_visible = true;
  prv_send_key(MESSAGE_KEY_POLL_START);
}

static void prv_spots_window_disappear(Window *window) {
  s_spots_visible = false;
  prv_send_key(MESSAGE_KEY_POLL_STOP);
}

static void prv_push_spots_window(void) {
  s_spots_window = window_create();
  window_set_window_handlers(s_spots_window, (WindowHandlers) {
    .load      = prv_spots_window_load,
    .unload    = prv_spots_window_unload,
    .appear    = prv_spots_window_appear,
    .disappear = prv_spots_window_disappear,
  });
  window_stack_push(s_spots_window, true);
}

/* ------------------------------------------------------------------ */
/*  Main window                                                         */
/* ------------------------------------------------------------------ */

static uint16_t prv_main_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return 2;
}

static void prv_main_draw_row(GContext *ctx, const Layer *cell_layer,
                              MenuIndex *idx, void *context) {
  switch (idx->row) {
    case 0: menu_cell_basic_draw(ctx, cell_layer, "Spots",    "Live POTA spots", NULL); break;
    case 1: menu_cell_basic_draw(ctx, cell_layer, "Settings", "Bands & modes",   NULL); break;
  }
}

static void prv_main_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  switch (idx->row) {
    case 0: prv_push_spots_window();    break;
    case 1: prv_push_settings_window(); break;
  }
}

static void prv_main_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_main_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_main_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_main_get_num_rows,
    .draw_row     = prv_main_draw_row,
    .select_click = prv_main_select,
  });
  menu_layer_set_click_config_onto_window(s_main_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_main_menu_layer));
}

static void prv_main_window_unload(Window *window) {
  menu_layer_destroy(s_main_menu_layer);
}

/* ------------------------------------------------------------------ */
/*  Init / deinit                                                       */
/* ------------------------------------------------------------------ */

static void prv_init(void) {
  /* Read filter mirrors; PKJS will send SETTINGS_SYNC shortly after launch
     to authoritative-overwrite these from localStorage */
  s_band_mask = persist_exists(PERSIST_KEY_BANDS)
    ? (uint32_t)persist_read_int(PERSIST_KEY_BANDS) : DEFAULT_BAND_MASK;
  s_mode_mask = persist_exists(PERSIST_KEY_MODES)
    ? (uint32_t)persist_read_int(PERSIST_KEY_MODES) : DEFAULT_MODE_MASK;

  s_bt_connected = connection_service_peek_pebble_app_connection();
  connection_service_subscribe((ConnectionHandlers){
    .pebble_app_connection_handler = prv_bt_handler
  });

  app_message_register_inbox_received(prv_inbox_received);
  app_message_open(512, 64);

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load   = prv_main_window_load,
    .unload = prv_main_window_unload,
  });
  window_stack_push(s_main_window, true);
}

static void prv_deinit(void) {
  connection_service_unsubscribe();
  window_destroy(s_main_window);
}

int main(void) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "POTAPebble starting");
  prv_init();
  app_event_loop();
  prv_deinit();
}
