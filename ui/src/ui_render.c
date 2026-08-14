#include "ui_internal.h"

/* Rendering is single-threaded. Keeping the active language here lets the
 * low-level text primitives form one localization boundary for every visible
 * string without threading a language argument through drawing geometry. */
static calf_language_t g_render_language = CALF_LANGUAGE_ENGLISH;


static void fill_rect(uint32_t *pixels, int stride, rect_t rectangle, uint32_t color)
{
    int x0 = rectangle.x < 0 ? 0 : rectangle.x;
    int y0 = rectangle.y < 0 ? 0 : rectangle.y;
    int x1 = rectangle.x + rectangle.w;
    int y1 = rectangle.y + rectangle.h;
    int x;
    int y;
    if(x1 > CALF_UI_WIDTH) x1 = CALF_UI_WIDTH;
    if(y1 > CALF_UI_HEIGHT) y1 = CALF_UI_HEIGHT;
    for(y = y0; y < y1; ++y)
        for(x = x0; x < x1; ++x)
            pixels[y * stride + x] = color;
}

static int text_width(const char *text, int scale)
{
    text = calf_ui_translate(g_render_language, text);
    return calf_font_text_width(text, scale);
}

static void draw_text(uint32_t *pixels, int stride, int x, int y,
                      const char *text, int scale, uint32_t color)
{
    text = calf_ui_translate(g_render_language, text);
    calf_font_draw(pixels, stride, x, y, text, scale, color);
}

static void draw_text_centered(uint32_t *pixels, int stride, rect_t rectangle,
                               const char *text, int scale, uint32_t color)
{
    int width = text_width(text, scale);
    int height = calf_font_text_height(scale);
    draw_text(pixels, stride, rectangle.x + (rectangle.w - width) / 2,
              rectangle.y + (rectangle.h - height) / 2, text, scale, color);
}

static void draw_line(uint32_t *pixels, int stride, int x0, int y0,
                      int x1, int y1, uint32_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for(;;) {
        int twice;
        fill_rect(pixels, stride, (rect_t){x0, y0, 1, 1}, color);
        if(x0 == x1 && y0 == y1) break;
        twice = error * 2;
        if(twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if(twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void draw_circle_outline(uint32_t *pixels, int stride,
                                int center_x, int center_y, int radius,
                                uint32_t color)
{
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while(x >= y) {
        fill_rect(pixels, stride,
                  (rect_t){center_x + x, center_y + y, 2, 2}, color);
        fill_rect(pixels, stride,
                  (rect_t){center_x + y, center_y + x, 2, 2}, color);
        fill_rect(pixels, stride,
                  (rect_t){center_x - y - 1, center_y + x, 2, 2}, color);
        fill_rect(pixels, stride,
                  (rect_t){center_x - x - 1, center_y + y, 2, 2}, color);
        fill_rect(pixels, stride,
                  (rect_t){center_x - x - 1, center_y - y - 1, 2, 2}, color);
        fill_rect(pixels, stride,
                  (rect_t){center_x - y - 1, center_y - x - 1, 2, 2}, color);
        fill_rect(pixels, stride,
                  (rect_t){center_x + y, center_y - x - 1, 2, 2}, color);
        fill_rect(pixels, stride,
                  (rect_t){center_x + x, center_y - y - 1, 2, 2}, color);
        ++y;
        if(error < 0) error += 2 * y + 1;
        else {
            --x;
            error += 2 * (y - x) + 1;
        }
    }
}

static void draw_button(uint32_t *pixels, int stride, rect_t rectangle,
                        const char *label, int selected, int danger)
{
    uint32_t border = danger ? 0xffff5a62u : (selected ? 0xff55e6b5u : 0xffd8e1e8u);
    uint32_t fill = danger ? 0xd0a11f2bu : (selected ? 0xd0145947u : 0xc0202832u);
    int scale = 4;
    while(scale > 2 && text_width(label, scale) > rectangle.w - 20) --scale;
    fill_rect(pixels, stride, rectangle, border);
    fill_rect(pixels, stride,
              (rect_t){rectangle.x + 3, rectangle.y + 3,
                       rectangle.w - 6, rectangle.h - 6}, fill);
    draw_text_centered(pixels, stride, rectangle, label, scale, 0xffffffffu);
}

static void draw_charging_bolt(uint32_t *pixels, int stride)
{
    static const uint16_t rows[] = {
        0x01eu, 0x03cu, 0x078u, 0x070u, 0x0e0u, 0x1feu, 0x01cu,
        0x038u, 0x030u, 0x060u, 0x0c0u, 0x080u, 0x100u,
    };
    size_t row;
    for(row = 0; row < ARRAY_SIZE(rows); ++row) {
        int column;
        for(column = 0; column < 9; ++column) {
            if(rows[row] & (1u << (8 - column)))
                fill_rect(pixels, stride,
                          (rect_t){694 + column * 2,
                                   23 + (int)row * 2, 2, 2},
                          0xffffe49au);
        }
    }
}

static void draw_battery(uint32_t *pixels, int stride,
                         int capacity, int usb_power)
{
    const rect_t body = {686, 17, 90, 38};
    const rect_t inside = {690, 21, 82, 30};
    int level = capacity;
    int charging;
    int full;
    uint32_t border;
    uint32_t fill;
    char label[8];

    if(level < 0) level = 0;
    if(level > 100) level = 100;
    full = capacity >= 100;
    charging = usb_power > 0 && !full;
    border = full ? 0xff55e68au : (charging ? 0xffffc14du : 0xffd8e1e8u);
    fill = full ? 0xff238a50u : (charging ? 0xff8a5a00u : 0xff286da8u);

    fill_rect(pixels, stride, body, border);
    fill_rect(pixels, stride, inside, 0xff111820u);
    if(capacity >= 0 && level > 0)
        fill_rect(pixels, stride,
                  (rect_t){inside.x, inside.y,
                           inside.w * level / 100, inside.h}, fill);
    fill_rect(pixels, stride, (rect_t){776, 28, 8, 16}, border);

    if(full) {
        text_copy(label, sizeof(label), "FULL");
        draw_text_centered(pixels, stride, inside, label, 2, 0xffffffffu);
    }
    else {
        label[0] = '\0';
        if(capacity < 0) text_copy(label, sizeof(label), "--");
        else {
            append_uint(label, sizeof(label), (unsigned)level);
            append_text(label, sizeof(label), "%");
        }
        draw_text_centered(pixels, stride,
                           charging ? (rect_t){714, 21, 58, 30} : inside,
                           label, 2, 0xffffffffu);
        if(charging) draw_charging_bolt(pixels, stride);
    }
}

static void draw_wifi_connected(uint32_t *pixels, int stride)
{
    const uint32_t color = 0xff55e6b5u;
    static const int outer[][4] = {
        {654, 26, 658, 22}, {658, 22, 664, 19},
        {664, 19, 670, 22}, {670, 22, 674, 26},
    };
    static const int middle[][4] = {
        {658, 33, 661, 30}, {661, 30, 664, 29},
        {664, 29, 667, 30}, {667, 30, 670, 33},
    };
    size_t index;
    for(index = 0; index < ARRAY_SIZE(outer); ++index) {
        draw_line(pixels, stride, outer[index][0], outer[index][1],
                  outer[index][2], outer[index][3], color);
        draw_line(pixels, stride, outer[index][0], outer[index][1] + 1,
                  outer[index][2], outer[index][3] + 1, color);
    }
    for(index = 0; index < ARRAY_SIZE(middle); ++index) {
        draw_line(pixels, stride, middle[index][0], middle[index][1],
                  middle[index][2], middle[index][3], color);
        draw_line(pixels, stride, middle[index][0], middle[index][1] + 1,
                  middle[index][2], middle[index][3] + 1, color);
    }
    fill_rect(pixels, stride, (rect_t){662, 39, 4, 4}, color);
}

static void draw_focus_frame(uint32_t *pixels, int stride, rect_t rectangle)
{
    const int width = 5;
    const uint32_t color = 0xffffd166u;
    fill_rect(pixels, stride,
              (rect_t){rectangle.x, rectangle.y, rectangle.w, width}, color);
    fill_rect(pixels, stride,
              (rect_t){rectangle.x, rectangle.y + rectangle.h - width,
                       rectangle.w, width}, color);
    fill_rect(pixels, stride,
              (rect_t){rectangle.x, rectangle.y, width, rectangle.h}, color);
    fill_rect(pixels, stride,
              (rect_t){rectangle.x + rectangle.w - width, rectangle.y,
                       width, rectangle.h}, color);
}

static void draw_top_bar(const calf_ui_t *ui, uint32_t *pixels, int stride,
                         const char *title, int back)
{
    char status[64];
    size_t duration_used = 0;
    rect_t title_area;
    rect_t status_area;
    int title_scale = 4;
    int status_scale = 3;
    int minimum_status_scale;
    if(ui->status.recording) {
        title_area = back ? (rect_t){140, 0, 340, 72}
                          : (rect_t){150, 0, 330, 72};
        status_area = (rect_t){486, 0, 164, 72};
        minimum_status_scale = 3;
    }
    else {
        title_area = back ? (rect_t){140, 0, 390, 72}
                          : (rect_t){150, 0, 380, 72};
        status_area = (rect_t){540, 0, 108, 72};
        minimum_status_scale = 2;
    }
    status[0] = '\0';
    fill_rect(pixels, stride, (rect_t){0, 0, 800, 72}, 0xd0182028u);
    if(back) draw_button(pixels, stride, (rect_t){12, 10, 112, 58}, "BACK", 0, 0);
    while(title_scale > 2 &&
          text_width(title, title_scale) > title_area.w - 12)
        --title_scale;
    draw_text_centered(pixels, stride, title_area,
                       title, title_scale, 0xffffffffu);

    if(!ui->status.online) append_text(status, sizeof(status), "OFFLINE");
    else if(ui->status.recording) {
        unsigned seconds = ui->status.recording_seconds >= 0
                               ? (unsigned)ui->status.recording_seconds : 0;
        unsigned hours = seconds / 3600u;
        unsigned minutes = (seconds / 60u) % 60u;
        if(hours != 0) {
            append_uint(status, sizeof(status), hours);
            append_text(status, sizeof(status), ":");
            append_padded_uint(status, sizeof(status), &duration_used,
                               minutes, 2);
        }
        else {
            append_text(status, sizeof(status), "REC ");
            append_padded_uint(status, sizeof(status), &duration_used,
                               minutes, 2);
        }
        append_text(status, sizeof(status), ":");
        append_padded_uint(status, sizeof(status), &duration_used,
                           seconds % 60u, 2);
    }
    else if(ui->status.streaming > 0) append_text(status, sizeof(status), "LIVE");
    else if(ui->status.playback > 0) append_text(status, sizeof(status), "PLAY");
    else append_text(status, sizeof(status), "READY");
    while(status_scale > minimum_status_scale &&
          text_width(status, status_scale) > status_area.w - 8)
        --status_scale;
    draw_text_centered(pixels, stride, status_area,
                       status, status_scale,
                       (ui->status.recording || ui->status.streaming > 0 ||
                        ui->status.playback > 0) ? 0xffff6269u : 0xffffffffu);
    if(ui->wifi_current_ssid[0] != '\0' && ui->wifi_ip_address[0] != '\0')
        draw_wifi_connected(pixels, stride);
    draw_battery(pixels, stride, ui->status.battery_percent,
                 ui->status.usb_power);
}

static void draw_main_value(uint32_t *pixels, int stride, rect_t area,
                            const char *caption, const char *value)
{
    int value_scale = 4;
    while(value_scale > 2 && text_width(value, value_scale) > area.w - 12)
        --value_scale;
    draw_text_centered(pixels, stride,
                       (rect_t){area.x, 5, area.w, 20},
                       caption, 2, 0xffbfcbd5u);
    draw_text_centered(pixels, stride,
                       (rect_t){area.x, 25, area.w, 40},
                       value, value_scale, 0xffffffffu);
}

static void draw_histogram_panel(uint32_t *pixels, int stride,
                                 const uint32_t *bins, int valid,
                                 const char *unavailable);

static void draw_capture_sequence(const calf_ui_t *ui, uint32_t *pixels,
                                  int stride)
{
    char timing[32];
    char shots[32];
    int burst = ui->drive_mode_known &&
                calf_drive_mode_is_burst((size_t)ui->drive_mode_index);
    const char *title = ui->capture_sequence_interval
                            ? "INTERVAL ACTIVE"
                            : burst ? "BURST ACTIVE" : "SELF TIMER";
    timing[0] = '\0';
    shots[0] = '\0';
    if(ui->capture_sequence_remaining_seconds == 0)
        append_text(timing, sizeof(timing), "NOW");
    else {
        append_uint(timing, sizeof(timing),
                    (unsigned)ui->capture_sequence_remaining_seconds);
        append_text(timing, sizeof(timing), " SEC");
    }
    append_text(shots, sizeof(shots), "SHOTS ");
    append_uint(shots, sizeof(shots), ui->capture_sequence_shot_count);
    fill_rect(pixels, stride, (rect_t){214, 126, 372, 218}, 0xf010171eu);
    fill_rect(pixels, stride, (rect_t){218, 130, 364, 210}, 0xff263440u);
    draw_text_centered(pixels, stride, (rect_t){230, 142, 340, 38},
                       title, 3, 0xffffd166u);
    draw_text_centered(pixels, stride, (rect_t){230, 190, 340, 70},
                       timing, 6, 0xffffffffu);
    draw_text_centered(pixels, stride, (rect_t){230, 274, 340, 30},
                       ui->capture_sequence_interval || burst
                           ? shots : "ONE PHOTO",
                       2, 0xff55e6b5u);
    draw_text_centered(pixels, stride, (rect_t){230, 308, 340, 24},
                       "SHUTTER OR BACK TO CANCEL", 2, 0xffbfcbd5u);
}

static void draw_motion_reticle(const calf_ui_t *ui, uint32_t *pixels,
                                int stride)
{
    const int center_x = 400;
    const int center_y = 240;
    int motion = ui->motion_score;
    int level_x = ui->level_x;
    int level_y = ui->level_y;
    int absolute_x;
    int absolute_y;
    int extent;
    int dot_x;
    int dot_y;
    int radius;
    int steady = ui->motion_stable_samples >= 3;
    int is_level;
    uint32_t motion_color;
    uint32_t dot_color;
    if(!ui->motion_valid && !ui->level_valid) return;
    if(ui->motion_calibration_samples < 5) {
        draw_line(pixels, stride, center_x - 58, center_y,
                  center_x - 26, center_y, 0x90ffffffu);
        draw_line(pixels, stride, center_x + 26, center_y,
                  center_x + 58, center_y, 0x90ffffffu);
        draw_circle_outline(pixels, stride, center_x, center_y, 18,
                            0xffbfcbd5u);
        draw_text_centered(pixels, stride,
                           (rect_t){center_x - 100, center_y + 42, 200, 20},
                           "CALIBRATING", 2, 0xffbfcbd5u);
        return;
    }
    if(motion < 0) motion = 0;
    if(motion > 100) motion = 100;
    if(level_x > 15) level_x -= 15;
    else if(level_x < -15) level_x += 15;
    else level_x = 0;
    if(level_y > 15) level_y -= 15;
    else if(level_y < -15) level_y += 15;
    else level_y = 0;
    if(level_x < -350) level_x = -350;
    if(level_x > 350) level_x = 350;
    if(level_y < -350) level_y = -350;
    if(level_y > 350) level_y = 350;
    dot_x = level_x * 40 / 350;
    dot_y = level_y * 40 / 350;
    absolute_x = dot_x < 0 ? -dot_x : dot_x;
    absolute_y = dot_y < 0 ? -dot_y : dot_y;
    extent = absolute_x > absolute_y
                 ? absolute_x + absolute_y * 3 / 8
                 : absolute_y + absolute_x * 3 / 8;
    if(extent > 40) {
        dot_x = dot_x * 40 / extent;
        dot_y = dot_y * 40 / extent;
    }
    dot_x += center_x;
    dot_y += center_y;
    radius = 12 + motion * 28 / 100;
    is_level = ui->level_valid && ui->level_x >= -25 && ui->level_x <= 25 &&
               ui->level_y >= -25 && ui->level_y <= 25;
    motion_color = steady ? 0xff55e6b5u
                          : (motion < 55 ? 0xffffd166u : 0xffff6269u);
    dot_color = is_level ? 0xff55e6b5u : 0xffffd166u;

    draw_line(pixels, stride, center_x - 76, center_y,
              center_x - 28, center_y, 0xc0ffffffu);
    draw_line(pixels, stride, center_x + 28, center_y,
              center_x + 76, center_y, 0xc0ffffffu);
    draw_line(pixels, stride, center_x, center_y - 50,
              center_x, center_y - 28, 0xa0ffffffu);
    draw_line(pixels, stride, center_x, center_y + 28,
              center_x, center_y + 50, 0xa0ffffffu);
    fill_rect(pixels, stride,
              (rect_t){center_x - 3, center_y - 3, 7, 7}, 0xd010171eu);
    draw_circle_outline(pixels, stride, center_x, center_y, radius,
                        motion_color);
    if(ui->level_valid) {
        fill_rect(pixels, stride,
                  (rect_t){dot_x - 5, dot_y - 5, 11, 11}, 0xe010171eu);
        fill_rect(pixels, stride,
                  (rect_t){dot_x - 3, dot_y - 3, 7, 7}, dot_color);
    }
}

static void draw_main(const calf_ui_t *ui, uint32_t *pixels, int stride)
{
    static const char *const compact_white_balance[] = {
        "AUTO", "DAY", "CLOUD", "SHADOW", "FLUOR", "TUNGST",
    };
    const char *exposure_value;
    const char *iso_value;
    const char *white_balance_value;
    const char *ev_value;
    const rect_t settings = main_button_cell(0);
    const rect_t zoom = main_button_cell(1);
    const rect_t histogram = main_button_cell(2);
    const rect_t mode_or_record = main_button_cell(3);
    const rect_t record = main_button_cell(4);
    int value_width = ui->status.recording ? 121 : 133;
    exposure_value = ui->exposure_known
                         ? k_exposures[ui->exposure_index].label : "?";
    iso_value = ui->iso_known ? k_isos[ui->iso_index].label : "?";
    white_balance_value =
        ui->white_balance_known && ui->white_balance_index >= 0 &&
        ui->white_balance_index < (int)ARRAY_SIZE(compact_white_balance)
            ? compact_white_balance[ui->white_balance_index] : "?";
    ev_value = ui->ev_known && ui->ev_index >= 0 &&
                       ui->ev_index < (int)ARRAY_SIZE(k_ev_values)
                   ? k_ev_values[ui->ev_index].label : "?";
    draw_top_bar(ui, pixels, stride, "", 0);
    fill_rect(pixels, stride,
              (rect_t){0, 4, value_width, 64}, 0x9028323cu);
    fill_rect(pixels, stride,
              (rect_t){value_width, 4, value_width, 64}, 0x9028323cu);
    fill_rect(pixels, stride,
              (rect_t){value_width * 2, 4, value_width, 64}, 0x9028323cu);
    fill_rect(pixels, stride,
              (rect_t){value_width * 3, 4, value_width, 64}, 0x9028323cu);
    draw_main_value(pixels, stride, (rect_t){0, 0, value_width, 72},
                    "WB", white_balance_value);
    draw_main_value(pixels, stride,
                    (rect_t){value_width, 0, value_width, 72},
                    "EV", ev_value);
    draw_main_value(pixels, stride,
                    (rect_t){value_width * 2, 0, value_width, 72},
                    "EXP", exposure_value);
    draw_main_value(pixels, stride,
                    (rect_t){value_width * 3, 0, value_width, 72},
                    "ISO", iso_value);
    fill_rect(pixels, stride, (rect_t){0, 408, 800, 72}, 0xc012181eu);
    draw_button(pixels, stride, settings, "SETTINGS", 0, 0);
    draw_button(pixels, stride, zoom,
                ui->lens_known && ui->lens_index == 0
                    ? "FULL" : "ZOOM", ui->lens_known && ui->lens_index == 0,
                0);
    draw_button(pixels, stride, histogram, "HIST",
                ui->live_histogram_visible, 0);
    draw_button(pixels, stride, mode_or_record,
                ui->capture_sequence_active
                    ? "CANCEL"
                    : ui->capture_mode != CALF_CAPTURE_VIDEO
                    ? "MODE" : (ui->status.recording ? "STOP" : "REC"),
                ui->capture_sequence_active || ui->status.recording,
                ui->capture_sequence_active || ui->status.recording);
    draw_button(pixels, stride, record,
                k_capture_modes[(int)ui->capture_mode].label,
                1, 0);

    if(ui->status.storage_free_mb >= 0) {
        char storage[48];
        uint64_t tenths_gb =
            ((uint64_t)(unsigned)ui->status.storage_free_mb * 10u + 512u) /
            1024u;
        storage[0] = '\0';
        append_text(storage, sizeof(storage), "FREE ");
        append_uint(storage, sizeof(storage), (unsigned)(tenths_gb / 10u));
        append_text(storage, sizeof(storage), ".");
        append_uint(storage, sizeof(storage), (unsigned)(tenths_gb % 10u));
        append_text(storage, sizeof(storage), " GB");
        draw_text(pixels, stride, 18, 382, storage, 2, 0xe8ffffffu);
    }

    if(!ui->live_histogram_visible && !ui->capture_sequence_active)
        draw_motion_reticle(ui, pixels, stride);

    if(ui->live_histogram_visible)
        draw_histogram_panel(pixels, stride, ui->live_histogram,
                             ui->live_histogram_valid,
                             ui->live_histogram_error
                                 ? "UNAVAILABLE" : "LOADING");

    if(ui->capture_mode == CALF_CAPTURE_NIGHT) {
        char preview[48];
        preview[0] = '\0';
        append_text(preview, sizeof(preview),
                    ui->night_preview_clipped ? "NIGHT PREVIEW LIMITED"
                                              : "NIGHT PREVIEW");
        if(ui->night_preview_iso > 0) {
            append_text(preview, sizeof(preview), "  ISO ");
            append_uint(preview, sizeof(preview),
                        (unsigned)ui->night_preview_iso);
        }
        draw_text_centered(pixels, stride, (rect_t){180, 78, 440, 30},
                           preview, 2, 0xffffd166u);
    }
    else if(ui->capture_mode != CALF_CAPTURE_VIDEO && ui->drive_mode_known &&
       ui->drive_mode_index > 0 &&
       ui->drive_mode_index < (int)ARRAY_SIZE(k_drive_modes))
        draw_text_centered(pixels, stride, (rect_t){250, 78, 300, 30},
                           k_drive_modes[ui->drive_mode_index].label,
                           2, 0xffffd166u);
    if(ui->capture_sequence_active)
        draw_capture_sequence(ui, pixels, stride);
}

static void draw_choice_grid(const calf_ui_t *ui, uint32_t *pixels, int stride,
                             const choice_t *choices, int count, int selected,
                             int columns, int top, int height, const char *title)
{
    int i;
    fill_rect(pixels, stride, (rect_t){0, 72, 800, 408}, 0xe00c1116u);
    draw_top_bar(ui, pixels, stride, title, 1);
    for(i = 0; i < count; ++i) {
        rect_t rectangle = grid_cell(i, columns, top, height);
        draw_button(pixels, stride, rectangle, choices[i].label,
                    i == selected, 0);
        if(ui->focus_visible && ui->focus_index == i)
            draw_focus_frame(pixels, stride, rectangle);
    }
}

static void draw_drive_modes(const calf_ui_t *ui, uint32_t *pixels, int stride)
{
    static const char *const row_labels[] = {
        "SINGLE", "TIMER", "BURST", "INTERVAL",
    };
    static const char *const option_labels[] = {
        "NO DELAY", "2 SEC", "5 SEC", "10 SEC", "3 SHOTS",
        "5 SHOTS", "10 SHOTS", "1 SEC", "2 SEC", "5 SEC",
        "10 SEC", "30 SEC", "60 SEC",
    };
    int row;
    int index;
    fill_rect(pixels, stride, (rect_t){0, 72, 800, 408}, 0xe00c1116u);
    draw_top_bar(ui, pixels, stride, "DRIVE MODE", 1);
    for(row = 0; row < 4; ++row) {
        rect_t label = {12, 84 + row * 92, 164, 80};
        fill_rect(pixels, stride, label, 0xff4b5e6fu);
        fill_rect(pixels, stride,
                  (rect_t){label.x + 3, label.y + 3,
                           label.w - 6, label.h - 6},
                  0xe0263440u);
        draw_text_centered(pixels, stride, label, row_labels[row],
                           2, 0xffffd166u);
    }
    for(index = 0; index < (int)ARRAY_SIZE(k_drive_modes); ++index) {
        rect_t rectangle = drive_mode_cell(index);
        draw_button(pixels, stride, rectangle, option_labels[index],
                    ui->drive_mode_known && ui->drive_mode_index == index, 0);
        if(ui->focus_visible && ui->focus_index == index)
            draw_focus_frame(pixels, stride, rectangle);
    }
}

static void draw_label_grid(const calf_ui_t *ui, uint32_t *pixels, int stride,
                            const char *title, const char *const *labels,
                            int count, int columns, int top, int height)
{
    int i;
    fill_rect(pixels, stride, (rect_t){0, 72, 800, 408}, 0xe00c1116u);
    draw_top_bar(ui, pixels, stride, title, 1);
    for(i = 0; i < count; ++i) {
        rect_t rectangle = grid_cell(i, columns, top, height);
        draw_button(pixels, stride, rectangle, labels[i], 0, 0);
        if(ui->focus_visible && ui->focus_index == i)
            draw_focus_frame(pixels, stride, rectangle);
    }
}

static void draw_settings_hub(const calf_ui_t *ui, uint32_t *pixels, int stride)
{
    int i;
    fill_rect(pixels, stride, (rect_t){0, 72, 800, 408}, 0xe00c1116u);
    draw_top_bar(ui, pixels, stride, "SETTINGS", 1);
    for(i = 0; i < (int)ARRAY_SIZE(k_settings_categories); ++i) {
        rect_t rectangle = grid_cell(i, 2, 80, 70);
        draw_button(pixels, stride, rectangle,
                    k_settings_categories[i].label, 0, 0);
        if(ui->focus_visible && ui->focus_index == i)
            draw_focus_frame(pixels, stride, rectangle);
    }
}

static void draw_datetime_settings(const calf_ui_t *ui, uint32_t *pixels,
                                   int stride)
{
    int index;
    char datetime[24];
    size_t used = 0;
    datetime[0] = '\0';
    if(ui->datetime_known) {
        append_padded_uint(datetime, sizeof(datetime), &used,
                           (unsigned)ui->datetime_year, 4);
        append_text(datetime, sizeof(datetime), "-"); ++used;
        append_padded_uint(datetime, sizeof(datetime), &used,
                           (unsigned)ui->datetime_month, 2);
        append_text(datetime, sizeof(datetime), "-"); ++used;
        append_padded_uint(datetime, sizeof(datetime), &used,
                           (unsigned)ui->datetime_day, 2);
        append_text(datetime, sizeof(datetime), " "); ++used;
        append_padded_uint(datetime, sizeof(datetime), &used,
                           (unsigned)ui->datetime_hour, 2);
        append_text(datetime, sizeof(datetime), ":"); ++used;
        append_padded_uint(datetime, sizeof(datetime), &used,
                           (unsigned)ui->datetime_minute, 2);
    }
    else text_copy(datetime, sizeof(datetime), "UNKNOWN");
    fill_rect(pixels, stride, (rect_t){0, 72, 800, 408}, 0xe00c1116u);
    draw_top_bar(ui, pixels, stride, "DATE TIME", 1);
    for(index = 0; index < (int)ARRAY_SIZE(k_datetime_setting_labels); ++index) {
        rect_t rectangle = grid_cell(index, 2, 112, 142);
        const char *detail = "UNKNOWN";
        draw_button(pixels, stride, rectangle,
                    k_datetime_setting_labels[index], 0, 0);
        if(index == 0 && ui->timezone_known)
            detail = k_timezones[ui->timezone_index].label;
        else if(index == 1 && ui->auto_time_known)
            detail = k_auto_time_values[ui->auto_time_index].label;
        else if(index == 2)
            detail = datetime;
        fill_rect(pixels, stride,
                  (rect_t){rectangle.x + 3,
                           rectangle.y + rectangle.h - 35,
                           rectangle.w - 6, 32}, 0xe0111820u);
        draw_text_centered(pixels, stride,
                           (rect_t){rectangle.x + 3,
                                    rectangle.y + rectangle.h - 35,
                                    rectangle.w - 6, 32},
                           detail, 2, 0xff55e6b5u);
        if(ui->focus_visible && ui->focus_index == index)
            draw_focus_frame(pixels, stride, rectangle);
    }
}

static const char *image_setting_detail(const calf_ui_t *ui, int index,
                                        char *numeric, size_t capacity,
                                        int *known)
{
    *known = 0;
    numeric[0] = '\0';
    if(index == 0) {
        *known = ui->white_balance_known;
        return *known ? k_white_balances[ui->white_balance_index].label : "?";
    }
    if(index == 1) {
        *known = ui->ev_known;
        return *known ? k_ev_values[ui->ev_index].label : "?";
    }
    if(index >= 2 && index <= 6) {
        int level = index - 2;
        *known = ui->image_level_known[level];
        if(!*known) return "?";
        append_uint(numeric, capacity,
                    (unsigned)ui->image_level_index[level]);
        append_text(numeric, capacity, "/20");
        return numeric;
    }
    if(index == 7) {
        *known = ui->antiflicker_known;
        return *known ? k_antiflicker_values[ui->antiflicker_index].label : "?";
    }
    *known = ui->effect_known;
    return *known ? k_image_effects[ui->effect_index].label : "?";
}

static void draw_image_settings(const calf_ui_t *ui, uint32_t *pixels,
                                int stride)
{
    int index;
    fill_rect(pixels, stride, (rect_t){0, 72, 800, 408}, 0xe00c1116u);
    draw_top_bar(ui, pixels, stride, "IMAGE", 1);
    for(index = 0; index < (int)ARRAY_SIZE(k_image_setting_labels); ++index) {
        rect_t rectangle = grid_cell(index, 3, 80, 116);
        char numeric[16];
        int known;
        const char *detail = image_setting_detail(
            ui, index, numeric, sizeof(numeric), &known);
        fill_rect(pixels, stride, rectangle, 0xffd8e1e8u);
        fill_rect(pixels, stride,
                  (rect_t){rectangle.x + 3, rectangle.y + 3,
                           rectangle.w - 6, rectangle.h - 6},
                  0xc0202832u);
        draw_text_centered(pixels, stride,
                           (rect_t){rectangle.x + 6, rectangle.y + 9,
                                    rectangle.w - 12, 38},
                           k_image_setting_labels[index], 3, 0xffffffffu);
        draw_text_centered(pixels, stride,
                           (rect_t){rectangle.x + 6, rectangle.y + 58,
                                    rectangle.w - 12, 42},
                           detail, 3,
                           known ? 0xff55e6b5u : 0xffbfcbd5u);
        if(ui->focus_visible && ui->focus_index == index)
            draw_focus_frame(pixels, stride, rectangle);
    }
}

static void draw_detail_settings(const calf_ui_t *ui, uint32_t *pixels,
                                 int stride, const char *title,
                                 const char *const *labels,
                                 const char *const *details,
                                 const int *known, int count, int columns,
                                 int top, int height)
{
    int index;
    fill_rect(pixels, stride, (rect_t){0, 72, 800, 408}, 0xe00c1116u);
    draw_top_bar(ui, pixels, stride, title, 1);
    for(index = 0; index < count; ++index) {
        rect_t rectangle = grid_cell(index, columns, top, height);
        fill_rect(pixels, stride, rectangle, 0xffd8e1e8u);
        fill_rect(pixels, stride,
                  (rect_t){rectangle.x + 3, rectangle.y + 3,
                           rectangle.w - 6, rectangle.h - 6},
                  0xc0202832u);
        draw_text_centered(pixels, stride,
                           (rect_t){rectangle.x + 6, rectangle.y + 9,
                                    rectangle.w - 12, 38},
                           labels[index], 3, 0xffffffffu);
        draw_text_centered(pixels, stride,
                           (rect_t){rectangle.x + 6,
                                    rectangle.y + rectangle.h - 58,
                                    rectangle.w - 12, 42},
                           known[index] ? details[index] : "?", 3,
                           known[index] ? 0xff55e6b5u : 0xffbfcbd5u);
        if(ui->focus_visible && ui->focus_index == index)
            draw_focus_frame(pixels, stride, rectangle);
    }
}

static void draw_camera_settings(const calf_ui_t *ui, uint32_t *pixels,
                                 int stride)
{
    const choice_t *resolutions = ui->capture_mode == CALF_CAPTURE_VIDEO
                                      ? k_video_resolutions
                                      : k_photo_resolutions;
    int resolution_count = ui->capture_mode == CALF_CAPTURE_VIDEO
                               ? (int)ARRAY_SIZE(k_video_resolutions)
                               : (int)ARRAY_SIZE(k_photo_resolutions);
    int resolution_valid = ui->resolution_known &&
                           ui->resolution_index >= 0 &&
                           ui->resolution_index < resolution_count;
    const char *details[] = {
        k_capture_modes[(int)ui->capture_mode].label,
        resolution_valid ? resolutions[ui->resolution_index].label : "?",
        ui->photo_format_known
            ? k_photo_formats[ui->photo_format_index].label : "?",
        ui->drive_mode_known
            ? k_drive_modes[ui->drive_mode_index].label : "?",
    };
    const int known[] = {
        1, resolution_valid, ui->photo_format_known, ui->drive_mode_known,
    };
    draw_detail_settings(ui, pixels, stride, "CAMERA",
                         k_camera_setting_labels, details, known, 4, 2,
                         112, 142);
}

static void draw_encoding_settings(const calf_ui_t *ui, uint32_t *pixels,
                                   int stride)
{
    const char *details[] = {
        ui->encoding_codec_known
            ? k_encoding_codecs[ui->encoding_codec_index].label : "?",
        ui->image_quality_known
            ? k_image_qualities[ui->image_quality_index].label : "?",
        ui->encoding_color_range_known
            ? k_color_ranges[ui->encoding_color_range_index].label : "?",
    };
    const int known[] = {
        ui->encoding_codec_known, ui->image_quality_known,
        ui->encoding_color_range_known,
    };
    draw_detail_settings(ui, pixels, stride, "VIDEO RECORDING",
                         k_encoding_setting_labels, details, known, 3, 2,
                         112, 142);
}

static void draw_recording_settings(const calf_ui_t *ui, uint32_t *pixels,
                                    int stride)
{
    const char *details[] = {
        ui->recording_codec_known
            ? k_recording_codecs[ui->recording_codec_index].label : "?",
        ui->recording_bitrate_known
            ? k_recording_bitrates[ui->recording_bitrate_index].label : "?",
        ui->recording_gop_known
            ? k_recording_gops[ui->recording_gop_index].label : "?",
        ui->recording_color_range_known
            ? k_color_ranges[ui->recording_color_range_index].label : "?",
    };
    const int known[] = {
        ui->recording_codec_known, ui->recording_bitrate_known,
        ui->recording_gop_known, ui->recording_color_range_known,
    };
    draw_detail_settings(ui, pixels, stride, "LIVE STREAMING",
                         k_recording_setting_labels, details, known, 4, 2,
                         108, 142);
}

static void draw_settings_category(const calf_ui_t *ui, uint32_t *pixels,
                                   int stride)
{
    if(ui->screen == CALF_SCREEN_SETTINGS_CAMERA)
        draw_camera_settings(ui, pixels, stride);
    else if(ui->screen == CALF_SCREEN_SETTINGS_IMAGE)
        draw_image_settings(ui, pixels, stride);
    else if(ui->screen == CALF_SCREEN_SETTINGS_ENCODING)
        draw_encoding_settings(ui, pixels, stride);
    else if(ui->screen == CALF_SCREEN_SETTINGS_RECORDING)
        draw_recording_settings(ui, pixels, stride);
    else if(ui->screen == CALF_SCREEN_SETTINGS_LIVE)
        draw_label_grid(ui, pixels, stride, "UVC", k_live_setting_labels,
                        (int)ARRAY_SIZE(k_live_setting_labels), 2, 108, 142);
    else if(ui->screen == CALF_SCREEN_SETTINGS_NETWORK) {
        const char *labels[] = {
            k_network_setting_labels[0],
            ui->wifi_enabled_known && !ui->wifi_enabled
                ? "TURN WI-FI ON" : "TURN WI-FI OFF",
            k_network_setting_labels[2], k_network_setting_labels[3],
        };
        draw_label_grid(ui, pixels, stride, "NETWORK", labels,
                        (int)ARRAY_SIZE(labels), 2, 112, 142);
    }
    else if(ui->screen == CALF_SCREEN_SETTINGS_AUDIO)
        draw_label_grid(ui, pixels, stride, "AUDIO", k_audio_setting_labels,
                        (int)ARRAY_SIZE(k_audio_setting_labels), 2, 82, 116);
    else if(ui->screen == CALF_SCREEN_SETTINGS_STORAGE)
        draw_label_grid(ui, pixels, stride, "STORAGE", k_storage_setting_labels,
                        (int)ARRAY_SIZE(k_storage_setting_labels), 2, 108, 142);
    else if(ui->screen == CALF_SCREEN_SETTINGS_DATETIME)
        draw_datetime_settings(ui, pixels, stride);
    else
        draw_label_grid(ui, pixels, stride, "GENERAL", k_general_setting_labels,
                        (int)ARRAY_SIZE(k_general_setting_labels), 2, 80, 82);
}

static void format_milli(char *text, size_t capacity, int value,
                         const char *unit)
{
    unsigned magnitude;
    unsigned hundredths;
    text[0] = '\0';
    if(value < 0) {
        append_text(text, capacity, "-");
        magnitude = (unsigned)(-value);
    }
    else magnitude = (unsigned)value;
    hundredths = (magnitude % 1000u + 5u) / 10u;
    magnitude /= 1000u;
    if(hundredths >= 100u) {
        ++magnitude;
        hundredths = 0;
    }
    append_uint(text, capacity, magnitude);
    append_text(text, capacity, ".");
    if(hundredths < 10u) append_text(text, capacity, "0");
    append_uint(text, capacity, hundredths);
    append_text(text, capacity, unit);
}

static void format_watts(char *text, size_t capacity, int milliwatts)
{
    unsigned magnitude;
    unsigned tenths;
    text[0] = '\0';
    if(milliwatts < 0) {
        append_text(text, capacity, "-");
        magnitude = (unsigned)(-milliwatts);
    }
    else magnitude = (unsigned)milliwatts;
    tenths = (magnitude % 1000u + 50u) / 100u;
    magnitude /= 1000u;
    if(tenths >= 10u) {
        ++magnitude;
        tenths = 0;
    }
    append_uint(text, capacity, magnitude);
    append_text(text, capacity, ".");
    append_uint(text, capacity, tenths);
    append_text(text, capacity, "W");
}

static void draw_power_panel(uint32_t *pixels, int stride, rect_t panel,
                             const char *title, uint32_t accent,
                             const char *voltage, const char *current,
                             const char *power, const char *footer)
{
    const char *const headings[] = {"VOLTAGE", "CURRENT", "POWER"};
    const char *const values[] = {voltage, current, power};
    int index;
    fill_rect(pixels, stride, panel, accent);
    fill_rect(pixels, stride,
              (rect_t){panel.x + 2, panel.y + 2,
                       panel.w - 4, panel.h - 4}, 0xff17212au);
    draw_text(pixels, stride, panel.x + 12, panel.y + 9,
              title, 2, accent);
    for(index = 0; index < 3; ++index) {
        rect_t column = {panel.x + 8 + index * (panel.w - 16) / 3,
                         panel.y + 31, (panel.w - 16) / 3, 45};
        draw_text_centered(pixels, stride,
                           (rect_t){column.x, column.y, column.w, 13},
                           headings[index], 1, 0xff91a1aeu);
        draw_text_centered(pixels, stride,
                           (rect_t){column.x, column.y + 16,
                                    column.w, 28},
                           values[index], 2, 0xffffffffu);
    }
    draw_text_centered(pixels, stride,
                       (rect_t){panel.x + 8, panel.y + 80,
                                panel.w - 16, 18},
                       footer, 1, accent);
}

static void format_temperature(char *text, size_t capacity, int temperature)
{
    text[0] = '\0';
    if(temperature < 0) {
        append_text(text, capacity, "--");
        return;
    }
    append_uint(text, capacity, (unsigned)temperature);
    append_text(text, capacity, "\xc2\xb0" "C");
}

static void draw_temperature_panel(uint32_t *pixels, int stride, rect_t panel,
                                   int system_temperature,
                                   int core_temperature, uint32_t accent)
{
    const char *const headings[] = {"SYSTEM", "CORE"};
    const int temperatures[] = {system_temperature, core_temperature};
    int index;
    fill_rect(pixels, stride, panel, accent);
    fill_rect(pixels, stride,
              (rect_t){panel.x + 2, panel.y + 2,
                       panel.w - 4, panel.h - 4}, 0xff17212au);
    draw_text(pixels, stride, panel.x + 12, panel.y + 9,
              "TEMPERATURE", 2, accent);
    for(index = 0; index < 2; ++index) {
        char value[16];
        rect_t column = {panel.x + 8 + index * (panel.w - 16) / 2,
                         panel.y + 31, (panel.w - 16) / 2, 45};
        format_temperature(value, sizeof(value), temperatures[index]);
        draw_text_centered(pixels, stride,
                           (rect_t){column.x, column.y, column.w, 13},
                           headings[index], 1, 0xff91a1aeu);
        draw_text_centered(pixels, stride,
                           (rect_t){column.x, column.y + 16,
                                    column.w, 28},
                           value, 2, 0xffffffffu);
    }
    draw_text_centered(pixels, stride,
                       (rect_t){panel.x + 8, panel.y + 80,
                                panel.w - 16, 18},
                       system_temperature >= 0 || core_temperature >= 0
                           ? "BACKEND TELEMETRY" : "UNAVAILABLE",
                       1, accent);
}

static const calf_power_sample_t *power_history_sample(const calf_ui_t *ui,
                                                        int ordered_index)
{
    int oldest = ui->power_history_count == CALF_POWER_HISTORY_COUNT
                     ? ui->power_history_next : 0;
    int index = (oldest + ordered_index) % CALF_POWER_HISTORY_COUNT;
    return &ui->power_history[index];
}

static int power_chart_y(int value, int minimum, int maximum, rect_t chart)
{
    int range = maximum - minimum;
    if(range <= 0) return chart.y + chart.h / 2;
    return chart.y + chart.h - 1 -
           (value - minimum) * (chart.h - 1) / range;
}

static void draw_power_history(const calf_ui_t *ui, uint32_t *pixels,
                               int stride)
{
    const uint32_t usb_color = 0xff52c7ffu;
    const uint32_t battery_color = 0xffffc14du;
    const uint32_t temperature_color = 0xffff7b72u;
    const rect_t usb_panel = {12, 80, 252, 106};
    const rect_t battery_panel = {274, 80, 252, 106};
    const rect_t temperature_panel = {536, 80, 252, 106};
    const rect_t chart = {52, 232, 736, 202};
    char usb_voltage[16] = "--";
    char usb_current[16] = "--";
    char usb_power[16] = "--";
    char battery_voltage[16] = "--";
    char battery_current[16] = "--";
    char battery_power[16] = "--";
    char usb_footer[24] = "UNAVAILABLE";
    char battery_footer[32] = "UNAVAILABLE";
    char device_load[40] = "CAMERA LOAD --";
    char maximum_label[16];
    char minimum_label[16];
    int minimum = 0;
    int maximum = 0;
    int index;
    int previous_usb = 0;
    int previous_battery = 0;
    int previous_x = 0;
    int have_previous = 0;

    fill_rect(pixels, stride, (rect_t){0, 72, 800, 408}, 0xff0c1116u);
    draw_top_bar(ui, pixels, stride, "POWER STATS", 1);
    if(ui->power.valid) {
        format_milli(usb_voltage, sizeof(usb_voltage), ui->power.usb_mv, "V");
        format_milli(usb_current, sizeof(usb_current), ui->power.usb_ma, "A");
        format_watts(usb_power, sizeof(usb_power), ui->power.usb_mw);
        format_milli(battery_voltage, sizeof(battery_voltage),
                     ui->power.battery_mv, "V");
        format_milli(battery_current, sizeof(battery_current),
                     ui->power.battery_ma, "A");
        format_watts(battery_power, sizeof(battery_power),
                     ui->power.battery_mw);
        text_copy(usb_footer, sizeof(usb_footer),
                  ui->power.usb_mv == 0 ? "DISCONNECTED" : "INPUT");
        text_copy(battery_footer, sizeof(battery_footer),
                  ui->power.battery_ma < 0 ? "CHARGING" :
                  (ui->power.battery_ma > 0 ? "DISCHARGING" : "IDLE"));
        if(ui->status.battery_percent >= 0) {
            append_text(battery_footer, sizeof(battery_footer), "  ");
            append_uint(battery_footer, sizeof(battery_footer),
                        (unsigned)ui->status.battery_percent);
            append_text(battery_footer, sizeof(battery_footer), "%");
        }
        text_copy(device_load, sizeof(device_load), "CAMERA LOAD ");
        format_watts(device_load + text_length(device_load),
                     sizeof(device_load) - text_length(device_load),
                     ui->power.device_mw);
        append_text(device_load, sizeof(device_load), " EST");
    }
    draw_power_panel(pixels, stride, usb_panel, "USB", usb_color,
                     usb_voltage, usb_current, usb_power, usb_footer);
    draw_power_panel(pixels, stride, battery_panel, "BATTERY", battery_color,
                     battery_voltage, battery_current, battery_power,
                     battery_footer);
    draw_temperature_panel(pixels, stride, temperature_panel,
                           ui->status.system_temp, ui->status.core_temp,
                           temperature_color);
    draw_text_centered(pixels, stride, (rect_t){250, 194, 300, 26},
                       device_load, 2, 0xffffffffu);
    fill_rect(pixels, stride, (rect_t){62, 204, 12, 6}, usb_color);
    draw_text(pixels, stride, 80, 200, "USB", 1, 0xffc7d3dcu);
    fill_rect(pixels, stride, (rect_t){122, 204, 12, 6}, battery_color);
    draw_text(pixels, stride, 140, 200, "BATTERY", 1, 0xffc7d3dcu);
    fill_rect(pixels, stride, (rect_t){656, 202, 12, 10}, 0xff4a2028u);
    draw_text(pixels, stride, 674, 200, "RECORDING", 1, 0xffc7d3dcu);

    for(index = 0; index < ui->power_history_count; ++index) {
        const calf_power_sample_t *sample = power_history_sample(ui, index);
        if(!sample->valid) continue;
        if(sample->usb_mw > maximum) maximum = sample->usb_mw;
        if(sample->battery_mw > maximum) maximum = sample->battery_mw;
        if(sample->battery_mw < minimum) minimum = sample->battery_mw;
    }
    if(maximum < 1000) maximum = 1000;
    maximum += 500;
    minimum -= 500;
    fill_rect(pixels, stride, chart, 0xff111920u);
    for(index = 0; index < ui->power_history_count; ++index) {
        const calf_power_sample_t *sample = power_history_sample(ui, index);
        int slot = CALF_POWER_HISTORY_COUNT - ui->power_history_count + index;
        int x0 = chart.x + slot * (chart.w - 1) /
                           (CALF_POWER_HISTORY_COUNT - 1);
        int x1 = chart.x + (slot + 1) * (chart.w - 1) /
                           (CALF_POWER_HISTORY_COUNT - 1);
        if(sample->recording)
            fill_rect(pixels, stride,
                      (rect_t){x0, chart.y, x1 - x0 + 1, chart.h},
                      0xff24181du);
    }
    for(index = 1; index < 4; ++index) {
        int y = chart.y + index * chart.h / 4;
        fill_rect(pixels, stride,
                  (rect_t){chart.x, y, chart.w, 1}, 0xff2a3741u);
    }
    {
        int zero_y = power_chart_y(0, minimum, maximum, chart);
        fill_rect(pixels, stride,
                  (rect_t){chart.x, zero_y, chart.w, 2}, 0xff71808bu);
    }
    for(index = 0; index < ui->power_history_count; ++index) {
        const calf_power_sample_t *sample = power_history_sample(ui, index);
        int slot = CALF_POWER_HISTORY_COUNT - ui->power_history_count + index;
        int x = chart.x + slot * (chart.w - 1) /
                          (CALF_POWER_HISTORY_COUNT - 1);
        int usb_y;
        int battery_y;
        if(!sample->valid) {
            have_previous = 0;
            continue;
        }
        usb_y = power_chart_y(sample->usb_mw, minimum, maximum, chart);
        battery_y = power_chart_y(sample->battery_mw,
                                  minimum, maximum, chart);
        if(have_previous) {
            draw_line(pixels, stride, previous_x, previous_usb,
                      x, usb_y, usb_color);
            draw_line(pixels, stride, previous_x, previous_usb + 1,
                      x, usb_y + 1, usb_color);
            draw_line(pixels, stride, previous_x, previous_battery,
                      x, battery_y, battery_color);
            draw_line(pixels, stride, previous_x, previous_battery + 1,
                      x, battery_y + 1, battery_color);
        }
        previous_x = x;
        previous_usb = usb_y;
        previous_battery = battery_y;
        have_previous = 1;
    }
    format_watts(maximum_label, sizeof(maximum_label), maximum);
    format_watts(minimum_label, sizeof(minimum_label), minimum);
    draw_text(pixels, stride, 4, chart.y, maximum_label, 1, 0xff91a1aeu);
    draw_text(pixels, stride, 4, chart.y + chart.h - 7,
              minimum_label, 1, 0xff91a1aeu);
    draw_text(pixels, stride, chart.x, 451, "-30 MIN", 1, 0xff91a1aeu);
    draw_text(pixels, stride, chart.x + chart.w - 18, 451,
              "NOW", 1, 0xff91a1aeu);
}

static void format_datetime_component(char *text, size_t capacity,
                                      int value, int digits)
{
    size_t used = 0;
    text[0] = '\0';
    append_padded_uint(text, capacity, &used, (unsigned)value,
                       (unsigned)digits);
}

static void draw_datetime_adjust(const calf_ui_t *ui, uint32_t *pixels,
                                 int stride)
{
    static const char *const labels[] = {
        "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SECOND",
    };
    const int values[] = {
        ui->datetime_year, ui->datetime_month, ui->datetime_day,
        ui->datetime_hour, ui->datetime_minute, ui->datetime_second,
    };
    int index;
    fill_rect(pixels, stride, (rect_t){0, 72, 800, 408}, 0xe00c1116u);
    draw_top_bar(ui, pixels, stride, "SET DATE TIME", 1);
    for(index = 0; index < 6; ++index) {
        rect_t row = {12, 78 + index * 52, 776, 46};
        char value[8];
        fill_rect(pixels, stride, row, 0xc0202832u);
        draw_text_centered(pixels, stride,
                           (rect_t){24, row.y, 132, row.h},
                           labels[index], 2, 0xffbfcbd5u);
        draw_button(pixels, stride,
                    (rect_t){170, row.y, 116, row.h}, "-", 0, 0);
        format_datetime_component(value, sizeof(value), values[index],
                                  index == 0 ? 4 : 2);
        draw_text_centered(pixels, stride,
                           (rect_t){300, row.y, 200, row.h},
                           value, 3, 0xffffffffu);
        draw_button(pixels, stride,
                    (rect_t){514, row.y, 116, row.h}, "+", 0, 0);
        if(ui->focus_visible && ui->focus_index == index)
            draw_focus_frame(pixels, stride, row);
    }
    draw_button(pixels, stride, (rect_t){280, 398, 240, 66}, "APPLY", 1, 0);
    if(ui->focus_visible && ui->focus_index == 6)
        draw_focus_frame(pixels, stride, (rect_t){280, 398, 240, 66});
}

static void draw_lenses(const calf_ui_t *ui, uint32_t *pixels, int stride)
{
    int i;
    fill_rect(pixels, stride, (rect_t){0, 72, 800, 408}, 0xe00c1116u);
    draw_top_bar(ui, pixels, stride, "INSPECTION", 1);
    for(i = 0; i < (int)ARRAY_SIZE(k_lenses); ++i) {
        rect_t rectangle = {16 + i * 262, 126, 244, 218};
        draw_button(pixels, stride, rectangle, k_lenses[i].label,
                    ui->lens_known && i == ui->lens_index, 0);
        if(ui->focus_visible && ui->focus_index == i)
            draw_focus_frame(pixels, stride, rectangle);
    }
    draw_text_centered(pixels, stride, (rect_t){40, 365, 720, 70},
                       "MODE CHANGE STOPS CAMERA GRAPH", 2, 0xffffd166u);
}

static void format_media_time(char *text, size_t capacity, int seconds)
{
    size_t used = 0;
    unsigned value = seconds > 0 ? (unsigned)seconds : 0;
    unsigned hours = value / 3600u;
    unsigned minutes = (value / 60u) % 60u;
    text[0] = '\0';
    if(hours != 0) {
        append_uint(text, capacity, hours);
        append_text(text, capacity, ":");
        append_padded_uint(text, capacity, &used, minutes, 2);
    }
    else append_uint(text, capacity, minutes);
    append_text(text, capacity, ":");
    append_padded_uint(text, capacity, &used, value % 60u, 2);
}

static void draw_histogram_panel(uint32_t *pixels, int stride,
                                 const uint32_t *bins, int valid,
                                 const char *unavailable)
{
    rect_t panel = {432, 154, 348, 202};
    uint32_t maximum = 0;
    int index;
    fill_rect(pixels, stride, panel, 0xc00c1116u);
    draw_text(pixels, stride, panel.x + 12, panel.y + 10,
              "LUMINANCE", 2, 0xffffffffu);
    if(!valid) {
        draw_text_centered(pixels, stride,
                           (rect_t){panel.x, panel.y + 58, panel.w, 90},
                           unavailable, 2, 0xffffd166u);
        return;
    }
    fill_rect(pixels, stride,
              (rect_t){panel.x + 12, panel.y + 86, 320, 1},
              0x805d6b78u);
    fill_rect(pixels, stride,
              (rect_t){panel.x + 12, panel.y + 136, 320, 1},
              0x805d6b78u);
    fill_rect(pixels, stride,
              (rect_t){panel.x + 12, panel.y + 186, 320, 1},
              0xc05d6b78u);
    for(index = 0; index < CALF_HISTOGRAM_BIN_COUNT; ++index)
        if(bins[index] > maximum) maximum = bins[index];
    if(maximum == 0) return;
    for(index = 0; index < CALF_HISTOGRAM_BIN_COUNT; ++index) {
        int height = (int)(((uint64_t)bins[index] * 148u) / maximum);
        int left = panel.x + 12 +
                   index * 320 / CALF_HISTOGRAM_BIN_COUNT;
        int right = panel.x + 12 +
                    (index + 1) * 320 / CALF_HISTOGRAM_BIN_COUNT;
        int width = right - left;
        if(width > 1) --width;
        fill_rect(pixels, stride,
                  (rect_t){left, panel.y + 186 - height, width, height},
                  0xff55e6b5u);
    }
    draw_text(pixels, stride, panel.x + 12, panel.y + 190,
              "0", 1, 0xffbfcbd5u);
    draw_text(pixels, stride, panel.x + 314, panel.y + 190,
              "255", 1, 0xffbfcbd5u);
}

static void draw_gallery_right_preview(const calf_ui_t *ui, uint32_t *pixels,
                                       int stride)
{
    rect_t area = {0, 116, 800, 292};
    int source_x = ui->gallery_preview_width / 2;
    int source_y = 0;
    int source_width = ui->gallery_preview_width / 2;
    int source_height = ui->gallery_preview_height;
    int x;
    int y;
    if(!ui->gallery_zoom_right ||
       ui->gallery_preview_pixels == (const uint32_t *)0 ||
       source_width <= 0 || source_height <= 0)
        return;
    if((long)source_width * area.h > (long)source_height * area.w) {
        int cropped = source_height * area.w / area.h;
        source_x += (source_width - cropped) / 2;
        source_width = cropped;
    }
    else {
        int cropped = source_width * area.h / area.w;
        source_y += (source_height - cropped) / 2;
        source_height = cropped;
    }
    for(y = 0; y < area.h; ++y) {
        int source_row = source_y + y * source_height / area.h;
        for(x = 0; x < area.w; ++x) {
            int source_column = source_x + x * source_width / area.w;
            pixels[(area.y + y) * stride + area.x + x] =
                ui->gallery_preview_pixels[
                    source_row * ui->gallery_preview_width + source_column];
        }
    }
}

static void draw_gallery(const calf_ui_t *ui, uint32_t *pixels, int stride)
{
    char position[32];
    char timing[40];
    char volume[24];
    int filename_scale = 3;
    draw_top_bar(ui, pixels, stride, "GALLERY", 1);
    draw_gallery_right_preview(ui, pixels, stride);
    fill_rect(pixels, stride, (rect_t){0, 72, 800, 44}, 0xc0182028u);
    if(ui->gallery_has_item) {
        position[0] = '\0';
        append_uint(position, sizeof(position),
                    (unsigned)(ui->gallery_index + 1));
        append_text(position, sizeof(position), " / ");
        append_uint(position, sizeof(position), (unsigned)ui->gallery_count);
        while(filename_scale > 2 &&
              text_width(ui->gallery_filename, filename_scale) > 390)
            --filename_scale;
        draw_text(pixels, stride, 18, 82, ui->gallery_filename,
                  filename_scale, 0xffffffffu);
        if(ui->gallery_is_video && ui->gallery_timing_known) {
            format_media_time(timing, sizeof(timing),
                              ui->gallery_position_seconds);
            append_text(timing, sizeof(timing), " / ");
            {
                char total[20];
                format_media_time(total, sizeof(total),
                                  ui->gallery_duration_seconds);
                append_text(timing, sizeof(timing), total);
            }
            draw_text_centered(pixels, stride, (rect_t){402, 72, 218, 44},
                               timing, 2, 0xffffffffu);
        }
        draw_text_centered(pixels, stride, (rect_t){628, 72, 160, 44},
                           position, 2, 0xffbfcbd5u);
        if(ui->gallery_histogram_visible)
            draw_histogram_panel(
                pixels, stride, ui->gallery_histogram,
                ui->gallery_histogram_valid,
                ui->gallery_is_video ? "PHOTO ONLY" : "LOADING");
        if(ui->gallery_is_video && ui->gallery_playing &&
           ui->gallery_volume_visible) {
            volume[0] = '\0';
            append_text(volume, sizeof(volume), "VOL ");
            if(ui->speaker_volume_known)
                append_uint(volume, sizeof(volume),
                            (unsigned)ui->speaker_volume_index);
            else append_text(volume, sizeof(volume), "--");
            fill_rect(pixels, stride, (rect_t){276, 360, 248, 38},
                      0xc00c1116u);
            draw_text_centered(pixels, stride, (rect_t){276, 360, 248, 38},
                               volume, 2, 0xffffffffu);
        }
    }
    else
        draw_text_centered(pixels, stride, (rect_t){0, 150, 800, 160},
                           "NO MEDIA", 4, 0xffffffffu);

    fill_rect(pixels, stride, (rect_t){0, 408, 800, 72}, 0xd012181eu);
    draw_button(pixels, stride, (rect_t){8, 414, 190, 58}, "PREV", 0, 0);
    draw_button(pixels, stride, (rect_t){206, 414, 190, 58},
                ui->gallery_is_video
                    ? (ui->gallery_playing ? "PAUSE" : "PLAY") : "PHOTO",
                ui->gallery_is_video, ui->gallery_playing);
    draw_button(pixels, stride, (rect_t){404, 414, 190, 58}, "NEXT", 0, 0);
    draw_button(pixels, stride, (rect_t){602, 414, 190, 58}, "DELETE", 0, 1);
}

static void draw_delete_confirm(const calf_ui_t *ui, uint32_t *pixels,
                                int stride)
{
    fill_rect(pixels, stride, (rect_t){0, 0, 800, 480}, 0xf00c1116u);
    draw_top_bar(ui, pixels, stride, "DELETE MEDIA", 1);
    draw_text_centered(pixels, stride, (rect_t){40, 112, 720, 54},
                       "DELETE THIS FILE?", 4, 0xffffd166u);
    draw_text_centered(pixels, stride, (rect_t){60, 180, 680, 54},
                       ui->gallery_filename, 3, 0xffffffffu);
    draw_text_centered(pixels, stride, (rect_t){70, 236, 660, 38},
                       "THIS CANNOT BE UNDONE", 2, 0xffff6269u);
    draw_button(pixels, stride, (rect_t){148, 300, 236, 92}, "CANCEL", 1, 0);
    draw_button(pixels, stride, (rect_t){416, 300, 236, 92}, "DELETE", 0, 1);
    if(ui->focus_visible)
        draw_focus_frame(pixels, stride,
            ui->focus_index == 1 ? (rect_t){416, 300, 236, 92}
                                 : (rect_t){148, 300, 236, 92});
}

static void draw_wifi_list(const calf_ui_t *ui, uint32_t *pixels, int stride)
{
    char status[96];
    int row;
    fill_rect(pixels, stride, (rect_t){0, 0, 800, 480}, 0xf00c1116u);
    draw_top_bar(ui, pixels, stride, "WI-FI NETWORKS", 1);
    status[0] = '\0';
    if(ui->wifi_current_ssid[0] != '\0') {
        append_text(status, sizeof(status), "CONNECTED: ");
        append_text(status, sizeof(status), ui->wifi_current_ssid);
        if(ui->wifi_ip_address[0] != '\0') {
            append_text(status, sizeof(status), "  ");
            append_text(status, sizeof(status), ui->wifi_ip_address);
        }
    }
    else append_text(status, sizeof(status), "NOT CONNECTED");
    draw_text_centered(pixels, stride, (rect_t){12, 74, 776, 44}, status, 2,
                       ui->wifi_current_ssid[0] != '\0'
                           ? 0xff55e6b5u : 0xffffd166u);
    if(ui->wifi_network_count == 0)
        draw_text_centered(pixels, stride, (rect_t){30, 180, 740, 120},
                           "NO NETWORKS FOUND", 3, 0xffffffffu);
    for(row = 0; row < 5; ++row) {
        int index = ui->wifi_list_offset + row;
        rect_t rectangle = {12, 126 + row * 56, 776, 50};
        char signal[24];
        int selected;
        if(index < 0) continue;
        if(index >= ui->wifi_network_count) break;
        selected = text_equal(ui->wifi_networks[index].ssid,
                              ui->wifi_current_ssid);
        draw_button(pixels, stride, rectangle,
                    ui->wifi_networks[index].ssid, selected, 0);
        signal[0] = '\0';
        append_int(signal, sizeof(signal), ui->wifi_networks[index].level);
        append_text(signal, sizeof(signal), " DBM");
        fill_rect(pixels, stride,
                  (rect_t){rectangle.x + rectangle.w - 142,
                           rectangle.y + 5, 132, rectangle.h - 10},
                  0xe0111820u);
        draw_text_centered(pixels, stride,
                           (rect_t){rectangle.x + rectangle.w - 142,
                                    rectangle.y + 5, 132,
                                    rectangle.h - 10},
                           signal, 2, 0xffbfcbd5u);
        if(ui->focus_visible && ui->focus_index == index)
            draw_focus_frame(pixels, stride, rectangle);
    }
    draw_button(pixels, stride, (rect_t){12, 414, 238, 54}, "PREV", 0, 0);
    draw_button(pixels, stride, (rect_t){270, 414, 260, 54}, "REFRESH", 0, 0);
    draw_button(pixels, stride, (rect_t){550, 414, 238, 54}, "NEXT", 0, 0);
    if(ui->focus_visible && ui->focus_index == ui->wifi_network_count)
        draw_focus_frame(pixels, stride, (rect_t){270, 414, 260, 54});
}

static void draw_wifi_password(const calf_ui_t *ui, uint32_t *pixels,
                               int stride)
{
    static const char *const mode_labels[] = {"LOWER", "UPPER", "SYMBOLS"};
    char masked_password[CALF_WIFI_PASSWORD_CAPACITY];
    size_t length = text_length(ui->wifi_password);
    size_t index;
    if(length >= sizeof(masked_password)) length = sizeof(masked_password) - 1;
    for(index = 0; index < length; ++index) masked_password[index] = '*';
    masked_password[length] = '\0';
    fill_rect(pixels, stride, (rect_t){0, 0, 800, 480}, 0xf00c1116u);
    draw_top_bar(ui, pixels, stride, "WI-FI PASSWORD", 1);
    draw_text_centered(
        pixels, stride, (rect_t){130, 72, 658, 38},
        ui->wifi_selected_index >= 0 &&
                ui->wifi_selected_index < ui->wifi_network_count
            ? ui->wifi_networks[ui->wifi_selected_index].ssid : "NETWORK",
        2, 0xff55e6b5u);
    fill_rect(pixels, stride, (rect_t){16, 112, 768, 44}, 0xe0202832u);
    draw_text_centered(pixels, stride, (rect_t){22, 112, 756, 44},
                       length == 0 ? "PASSWORD (LEAVE BLANK FOR OPEN)"
                                   : masked_password,
                       2, 0xffffffffu);
    for(index = 0; index < 40; ++index) {
        char label[2] = {
            k_wifi_keyboard_rows[ui->wifi_keyboard_mode][index], '\0'
        };
        rect_t rectangle = wifi_key_cell((int)index);
        draw_button(pixels, stride, rectangle, label, 0, 0);
        if(ui->focus_visible && ui->focus_index == (int)index)
            draw_focus_frame(pixels, stride, rectangle);
    }
    draw_button(pixels, stride, wifi_special_cell(0),
                mode_labels[ui->wifi_keyboard_mode], 0, 0);
    draw_button(pixels, stride, wifi_special_cell(1), "SPACE", 0, 0);
    draw_button(pixels, stride, wifi_special_cell(2), "ERASE", 0, 0);
    draw_button(pixels, stride, wifi_special_cell(3),
                length == 0 ? "OPEN" : "CONNECT", 1, 0);
    if(ui->focus_visible && ui->focus_index >= 40)
        draw_focus_frame(pixels, stride,
                         wifi_special_cell(ui->focus_index - 40));
}

static void draw_wifi_off_confirm(const calf_ui_t *ui, uint32_t *pixels,
                                  int stride)
{
    fill_rect(pixels, stride, (rect_t){0, 0, 800, 480}, 0xf00c1116u);
    draw_top_bar(ui, pixels, stride, "WI-FI POWER", 1);
    draw_text_centered(pixels, stride, (rect_t){50, 108, 700, 52},
                       "TURN WI-FI OFF?", 4, 0xffffd166u);
    draw_text_centered(pixels, stride, (rect_t){50, 184, 700, 40},
                       "TELNET AND REMOTE CONTROL WILL DISCONNECT", 2,
                       0xffff6269u);
    draw_text_centered(pixels, stride, (rect_t){50, 232, 700, 40},
                       "USE THIS SCREEN TO TURN WI-FI ON AGAIN", 2,
                       0xff55e6b5u);
    draw_button(pixels, stride, (rect_t){148, 316, 236, 92}, "CANCEL", 1, 0);
    draw_button(pixels, stride, (rect_t){416, 316, 236, 92}, "TURN OFF", 0, 1);
    if(ui->focus_visible)
        draw_focus_frame(pixels, stride,
            ui->focus_index == 1 ? (rect_t){416, 316, 236, 92}
                                 : (rect_t){148, 316, 236, 92});
}

static void draw_update_confirm(const calf_ui_t *ui, uint32_t *pixels,
                                int stride)
{
    char size[48];
    fill_rect(pixels, stride, (rect_t){0, 0, 800, 480}, 0xf00c1116u);
    draw_top_bar(ui, pixels, stride, "FIRMWARE UPDATE", 1);
    draw_text_centered(pixels, stride, (rect_t){50, 92, 700, 48},
                       "INSTALL vpupdate.bin?", 3, 0xffffd166u);
    size[0] = '\0';
    append_text(size, sizeof(size), "VALIDATED TAR - ");
    append_uint(size, sizeof(size),
                (unsigned)(ui->update_size_mb < 0 ? 0 : ui->update_size_mb));
    append_text(size, sizeof(size), " MB");
    draw_text_centered(pixels, stride, (rect_t){80, 148, 640, 40},
                       size, 2, 0xffffffffu);
    draw_text_centered(pixels, stride, (rect_t){50, 202, 700, 38},
                       "USB POWER AND 50% BATTERY REQUIRED", 2,
                       0xff55e6b5u);
    draw_text_centered(pixels, stride, (rect_t){50, 242, 700, 38},
                       "CAMERA REBOOTS INTO RECOVERY", 2, 0xffffd166u);
    draw_text_centered(pixels, stride, (rect_t){50, 276, 700, 32},
                       "DO NOT REMOVE POWER - NO AUTO ROLLBACK", 2,
                       0xffff6269u);
    draw_button(pixels, stride, (rect_t){148, 316, 236, 92}, "CANCEL", 1, 0);
    draw_button(pixels, stride, (rect_t){416, 316, 236, 92}, "INSTALL", 0, 1);
    if(ui->focus_visible)
        draw_focus_frame(pixels, stride,
            ui->focus_index == 1 ? (rect_t){416, 316, 236, 92}
                                 : (rect_t){148, 316, 236, 92});
}

static void draw_stock_ui_confirm(const calf_ui_t *ui, uint32_t *pixels,
                                  int stride)
{
    fill_rect(pixels, stride, (rect_t){0, 0, 800, 480}, 0xf00c1116u);
    draw_top_bar(ui, pixels, stride, "STOCK UI", 1);
    draw_text_centered(pixels, stride, (rect_t){50, 108, 700, 52},
                       "LOAD STOCK UI?", 4, 0xffffd166u);
    draw_text_centered(pixels, stride, (rect_t){50, 184, 700, 40},
                       "CAMERA BACKEND AND UI WILL RESTART", 2,
                       0xffff6269u);
    draw_text_centered(pixels, stride, (rect_t){50, 232, 700, 40},
                       "REBOOT RETURNS TO THE CALF UI", 2,
                       0xff55e6b5u);
    draw_button(pixels, stride, (rect_t){148, 316, 236, 92}, "CANCEL", 1, 0);
    draw_button(pixels, stride, (rect_t){416, 316, 236, 92}, "LOAD", 0, 1);
    if(ui->focus_visible)
        draw_focus_frame(pixels, stride,
            ui->focus_index == 1 ? (rect_t){416, 316, 236, 92}
                                 : (rect_t){148, 316, 236, 92});
}

void calf_ui_render(const calf_ui_t *ui, uint32_t *argb, int stride_pixels)
{
    int x;
    int y;
    g_render_language = ui->language_index >= 0 &&
                        ui->language_index < (int)CALF_LANGUAGE_COUNT
                            ? (calf_language_t)ui->language_index
                            : CALF_LANGUAGE_ENGLISH;
    for(y = 0; y < CALF_UI_HEIGHT; ++y)
        for(x = 0; x < CALF_UI_WIDTH; ++x)
            argb[y * stride_pixels + x] = 0x00000000u;

    if(ui->screen == CALF_SCREEN_MAIN) draw_main(ui, argb, stride_pixels);
    else if(ui->screen == CALF_SCREEN_EXPOSURE) {
        const choice_t *choices;
        size_t count;
        choices = calf_ui_exposure_choices(ui, &count);
        draw_choice_grid(ui, argb, stride_pixels, choices, (int)count,
                         calf_ui_exposure_visible_selection(ui),
                         ui->capture_mode == CALF_CAPTURE_NIGHT ? 3 : 4,
                         88, 108, "EXPOSURE");
    }
    else if(ui->screen == CALF_SCREEN_ISO) {
        const choice_t *choices;
        size_t count;
        choices = calf_ui_iso_choices(ui, &count);
        draw_choice_grid(ui, argb, stride_pixels, choices, (int)count,
                         calf_ui_iso_visible_selection(ui),
                         ui->capture_mode == CALF_CAPTURE_NIGHT ? 4 : 3,
                         88, 108, "ISO");
    }
    else if(ui->screen == CALF_SCREEN_LENS)
        draw_lenses(ui, argb, stride_pixels);
    else if(ui->screen == CALF_SCREEN_SETTINGS)
        draw_settings_hub(ui, argb, stride_pixels);
    else if(ui->screen >= CALF_SCREEN_SETTINGS_CAMERA &&
            ui->screen <= CALF_SCREEN_SETTINGS_GENERAL)
        draw_settings_category(ui, argb, stride_pixels);
    else if(ui->screen == CALF_SCREEN_WHITE_BALANCE)
        draw_choice_grid(ui, argb, stride_pixels, k_white_balances,
                         (int)ARRAY_SIZE(k_white_balances),
                         ui->white_balance_known ? ui->white_balance_index : -1,
                         3, 88, 156, "WHITE BAL");
    else if(ui->screen == CALF_SCREEN_EV)
        draw_choice_grid(ui, argb, stride_pixels, k_ev_values,
                         (int)ARRAY_SIZE(k_ev_values),
                         ui->ev_known ? ui->ev_index : -1,
                         4, 88, 156, "EV");
    else if(ui->screen == CALF_SCREEN_ANTIFLICKER)
        draw_choice_grid(ui, argb, stride_pixels, k_antiflicker_values,
                         (int)ARRAY_SIZE(k_antiflicker_values),
                         ui->antiflicker_known ? ui->antiflicker_index : -1,
                         2, 108, 142, "FLICKER");
    else if(ui->screen >= CALF_SCREEN_IMAGE_BRIGHTNESS &&
            ui->screen <= CALF_SCREEN_IMAGE_DNR) {
        static const char *const titles[] = {
            "BRIGHTNESS", "CONTRAST", "SATURATION", "SHARPNESS", "NOISE RED",
        };
        int level = (int)ui->screen - (int)CALF_SCREEN_IMAGE_BRIGHTNESS;
        draw_choice_grid(ui, argb, stride_pixels, k_image_levels,
                         (int)ARRAY_SIZE(k_image_levels),
                         ui->image_level_known[level]
                             ? ui->image_level_index[level] : -1,
                         7, 80, 108, titles[level]);
    }
    else if(ui->screen == CALF_SCREEN_IMAGE_EFFECT)
        draw_choice_grid(ui, argb, stride_pixels, k_image_effects,
                         (int)ARRAY_SIZE(k_image_effects),
                         ui->effect_known ? ui->effect_index : -1,
                         2, 126, 218, "EFFECT");
    else if(ui->screen == CALF_SCREEN_DISPLAY)
        draw_choice_grid(ui, argb, stride_pixels, k_backlight_values,
                         (int)ARRAY_SIZE(k_backlight_values),
                         ui->backlight_known ? ui->backlight_index : -1,
                         7, 80, 82, "DISPLAY");
    else if(ui->screen == CALF_SCREEN_DISPLAY_OFF)
        draw_choice_grid(ui, argb, stride_pixels, k_display_off_values,
                         (int)ARRAY_SIZE(k_display_off_values),
                         ui->display_off_known ? ui->display_off_index : -1,
                         2, 80, 82, "DISPLAY OFF");
    else if(ui->screen == CALF_SCREEN_LANGUAGE)
        draw_choice_grid(ui, argb, stride_pixels, k_languages,
                         (int)ARRAY_SIZE(k_languages),
                         ui->language_known ? ui->language_index : -1,
                         1, 126, 218, "LANGUAGE");
    else if(ui->screen == CALF_SCREEN_INDICATOR_LED)
        draw_choice_grid(ui, argb, stride_pixels, k_indicator_led_values,
                         (int)ARRAY_SIZE(k_indicator_led_values),
                         ui->indicator_led_known
                             ? ui->indicator_led_index : -1,
                         2, 126, 218, "INDICATOR LED");
    else if(ui->screen == CALF_SCREEN_AUDIO_INPUT)
        draw_choice_grid(ui, argb, stride_pixels, k_audio_inputs,
                         (int)ARRAY_SIZE(k_audio_inputs),
                         ui->audio_input_known ? ui->audio_input_index : -1,
                         2, 108, 142, "AUDIO INPUT");
    else if(ui->screen >= CALF_SCREEN_AUDIO_BUILTIN_VOLUME &&
            ui->screen <= CALF_SCREEN_AUDIO_USB_VOLUME) {
        static const char *const titles[] = {
            "BUILTIN MIC", "LINE IN", "USB MIC",
        };
        int input = (int)ui->screen -
                    (int)CALF_SCREEN_AUDIO_BUILTIN_VOLUME;
        draw_choice_grid(ui, argb, stride_pixels, k_audio_input_volumes,
                         (int)ARRAY_SIZE(k_audio_input_volumes),
                         ui->audio_input_volume_known[input]
                             ? ui->audio_input_volume_index[input] : -1,
                         4, 88, 108, titles[input]);
    }
    else if(ui->screen == CALF_SCREEN_AUDIO_SPEAKER_VOLUME)
        draw_choice_grid(ui, argb, stride_pixels, k_speaker_volumes,
                         (int)ARRAY_SIZE(k_speaker_volumes),
                         ui->speaker_volume_known
                             ? ui->speaker_volume_index : -1,
                         4, 80, 82, "SPEAKER");
    else if(ui->screen == CALF_SCREEN_TIMEZONE)
        draw_choice_grid(ui, argb, stride_pixels, k_timezones,
                         (int)ARRAY_SIZE(k_timezones),
                         ui->timezone_known ? ui->timezone_index : -1,
                         5, 80, 68, "TIME ZONE");
    else if(ui->screen == CALF_SCREEN_AUTO_TIME)
        draw_choice_grid(ui, argb, stride_pixels, k_auto_time_values,
                         (int)ARRAY_SIZE(k_auto_time_values),
                         ui->auto_time_known ? ui->auto_time_index : -1,
                         2, 126, 218, "AUTO SET");
    else if(ui->screen == CALF_SCREEN_CAPTURE_MODE)
        draw_choice_grid(ui, argb, stride_pixels, k_capture_modes,
                         (int)ARRAY_SIZE(k_capture_modes),
                         (int)ui->capture_mode,
                         3, 126, 218, "CAPTURE MODE");
    else if(ui->screen == CALF_SCREEN_CAMERA_RESOLUTION) {
        const choice_t *choices = ui->capture_mode == CALF_CAPTURE_VIDEO
                                      ? k_video_resolutions
                                      : k_photo_resolutions;
        int count = ui->capture_mode == CALF_CAPTURE_VIDEO
                        ? (int)ARRAY_SIZE(k_video_resolutions)
                        : (int)ARRAY_SIZE(k_photo_resolutions);
        draw_choice_grid(ui, argb, stride_pixels, choices, count,
                         ui->resolution_known ? ui->resolution_index : -1,
                         2, ui->capture_mode == CALF_CAPTURE_VIDEO ? 80 : 126,
                         ui->capture_mode == CALF_CAPTURE_VIDEO ? 70 : 218,
                         "RESOLUTION");
    }
    else if(ui->screen == CALF_SCREEN_PHOTO_FORMAT)
        draw_choice_grid(ui, argb, stride_pixels, k_photo_formats,
                         (int)ARRAY_SIZE(k_photo_formats),
                         ui->photo_format_known
                             ? ui->photo_format_index : -1,
                         2, 126, 218, "PHOTO FORMAT");
    else if(ui->screen == CALF_SCREEN_DRIVE_MODE)
        draw_drive_modes(ui, argb, stride_pixels);
    else if(ui->screen == CALF_SCREEN_ENCODING_CODEC)
        draw_choice_grid(ui, argb, stride_pixels, k_encoding_codecs,
                         (int)ARRAY_SIZE(k_encoding_codecs),
                         ui->encoding_codec_known
                             ? ui->encoding_codec_index : -1,
                         2, 126, 218, "VIDEO CODEC");
    else if(ui->screen == CALF_SCREEN_ENCODING_IMAGE_QUALITY)
        draw_choice_grid(ui, argb, stride_pixels, k_image_qualities,
                         (int)ARRAY_SIZE(k_image_qualities),
                         ui->image_quality_known
                             ? ui->image_quality_index : -1,
                         2, 108, 142, "IMAGE QUALITY");
    else if(ui->screen == CALF_SCREEN_ENCODING_COLOR_RANGE)
        draw_choice_grid(ui, argb, stride_pixels, k_color_ranges,
                         (int)ARRAY_SIZE(k_color_ranges),
                         ui->encoding_color_range_known
                             ? ui->encoding_color_range_index : -1,
                         2, 126, 218, "COLOR RANGE");
    else if(ui->screen == CALF_SCREEN_RECORDING_CODEC)
        draw_choice_grid(ui, argb, stride_pixels, k_recording_codecs,
                         (int)ARRAY_SIZE(k_recording_codecs),
                         ui->recording_codec_known
                             ? ui->recording_codec_index : -1,
                         2, 108, 142, "LIVE CODEC");
    else if(ui->screen == CALF_SCREEN_RECORDING_BITRATE)
        draw_choice_grid(ui, argb, stride_pixels, k_recording_bitrates,
                         (int)ARRAY_SIZE(k_recording_bitrates),
                         ui->recording_bitrate_known
                             ? ui->recording_bitrate_index : -1,
                         2, 80, 70, "LIVE BITRATE");
    else if(ui->screen == CALF_SCREEN_RECORDING_GOP)
        draw_choice_grid(ui, argb, stride_pixels, k_recording_gops,
                         (int)ARRAY_SIZE(k_recording_gops),
                         ui->recording_gop_known
                             ? ui->recording_gop_index : -1,
                         3, 108, 142, "LIVE GOP");
    else if(ui->screen == CALF_SCREEN_RECORDING_COLOR_RANGE)
        draw_choice_grid(ui, argb, stride_pixels, k_color_ranges,
                         (int)ARRAY_SIZE(k_color_ranges),
                         ui->recording_color_range_known
                             ? ui->recording_color_range_index : -1,
                         2, 126, 218, "LIVE RANGE");
    else if(ui->screen == CALF_SCREEN_ADJUST_DATETIME)
        draw_datetime_adjust(ui, argb, stride_pixels);
    else if(ui->screen == CALF_SCREEN_GALLERY)
        draw_gallery(ui, argb, stride_pixels);
    else if(ui->screen == CALF_SCREEN_DELETE_CONFIRM)
        draw_delete_confirm(ui, argb, stride_pixels);
    else if(ui->screen == CALF_SCREEN_WIFI_LIST)
        draw_wifi_list(ui, argb, stride_pixels);
    else if(ui->screen == CALF_SCREEN_WIFI_PASSWORD)
        draw_wifi_password(ui, argb, stride_pixels);
    else if(ui->screen == CALF_SCREEN_WIFI_OFF_CONFIRM)
        draw_wifi_off_confirm(ui, argb, stride_pixels);
    else if(ui->screen == CALF_SCREEN_UPDATE_CONFIRM)
        draw_update_confirm(ui, argb, stride_pixels);
    else if(ui->screen == CALF_SCREEN_STOCK_UI_CONFIRM)
        draw_stock_ui_confirm(ui, argb, stride_pixels);
    else if(ui->screen == CALF_SCREEN_POWER_HISTORY)
        draw_power_history(ui, argb, stride_pixels);

    if(ui->message[0] != '\0') {
        rect_t message = {190, 286, 420, 62};
        uint32_t color = ui->message_is_error ? 0xf0a11f2bu : 0xe0202832u;
        if(ui->pending_action != CALF_ACTION_NONE) message.y = 262;
        fill_rect(argb, stride_pixels, message, color);
        draw_text_centered(argb, stride_pixels, message, ui->message, 2,
                           0xffffffffu);
    }
}
