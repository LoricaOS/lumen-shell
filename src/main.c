/* lumen-shell — the LoricaOS desktop shell: top bar (Aegis menu, the focused
 * app's menu, volume popup, Wi-Fi/battery/CPU-temp status, notifications).
 *
 * Split out of lumen's compositor process (which used to draw all of this
 * in-process via on_draw_desktop/on_draw_overlay callbacks and intercept its
 * clicks before comp_handle_mouse ran). This is now an ordinary external
 * client, exactly like citadel-dock — talks to lumen over a top-anchored
 * panel (LUMEN_OP_CREATE_PANEL, LUMEN_PANEL_TOP) instead of the dock's
 * bottom-anchored one. The compositor still owns: window management, the
 * wallpaper, process spawning (SYS_SPAWN), and anything needing the POWER
 * capability (reboot/poweroff) or in-process state (the About window,
 * lock-screen input freeze) — those go back to lumen via LUMEN_OP_INVOKE,
 * same mechanism the dock already uses to launch apps.
 *
 * The bar is normally a thin 34px-tall strip (TOPBAR_HEIGHT), full screen
 * width. Its dropdowns (Aegis menu, app-menu, volume popup) and the
 * notification toast extend below that, so the window grows via
 * LUMEN_OP_RESIZE_SELF while one is open/showing and shrinks back to
 * TOPBAR_HEIGHT once it isn't — the same self-resize primitive citadel-dock
 * uses to fit its task area, just growing downward instead of widening.
 *
 * Frosted glass: lumen's compositor automatically blurs+tints the REAL
 * desktop behind any chromeless+frosted window (the panel style) and blits
 * our own content on top wherever it isn't the exact C_TERM_BG key color —
 * we don't (and, as an external client with no access to the real
 * framebuffer, can't) blur anything ourselves. So the base fill here is
 * C_TERM_BG, and popups skip the old in-process draw_box_blur() call (it
 * would just blur our own buffer, not the desktop) but keep their translucent
 * tint fill — a flat dark tint over the key, same tradeoff citadel-dock
 * already accepted for its own panel. Popup corners are square (not rounded)
 * here — a cosmetic simplification versus the original in-process version.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/syscall.h>
#include <stdint.h>

#include <glyph.h>
#include <font.h>
#include <taskbar.h>
#include <theme.h>
#include <lumen_client.h>

/* ── Bridge surface_t expected by glyph draw_* over the lumen window backbuf
 * (same helper citadel-dock uses). ─────────────────────────────────────── */
static surface_t
backbuf_surface(lumen_window_t *win)
{
    surface_t s;
    s.buf   = (uint32_t *)win->backbuf;
    s.w     = win->w;
    s.h     = win->h;
    s.pitch = win->stride;
    return s;
}

static void log_console(const char *msg) { write(2, msg, strlen(msg)); }

/* ── Aegis menu (top-left brand icon click) ───────────────────────────────
 * Ported verbatim from lumen's old in-process menu, minus compositor-only
 * actions (About/Lock/Reboot/Poweroff now go back to lumen via
 * LUMEN_OP_INVOKE — see handle_mouse). */
static const char *menu_labels[] = {
    "About LoricaOS", "Applications", "Files", "Text Editor", "Calculator",
    "Settings...", "Lock Screen", "---", "Restart", "Power Off"
};
#define MENU_ITEM_ABOUT      0
#define MENU_ITEM_APPS       1
#define MENU_ITEM_FILES      2
#define MENU_ITEM_EDITOR     3
#define MENU_ITEM_CALCULATOR 4
#define MENU_ITEM_SETTINGS   5
#define MENU_ITEM_LOCK       6
#define MENU_ITEM_SEPARATOR  7
#define MENU_ITEM_REBOOT     8
#define MENU_ITEM_POWEROFF   9
#define MENU_ITEMS           10

static int menu_open;
static int menu_hover = -1;

#define MENU_X        4
#define MENU_Y        TOPBAR_HEIGHT
#define MENU_W        160
#define MENU_ITEM_H   28
#define MENU_SEP_H    10
#define MENU_BG       THEME_SURFACE_2
#define MENU_HOVER_BG THEME_SELECTION
#define MENU_TEXT      THEME_TEXT
#define MENU_SEP_COL  THEME_BORDER

static int
menu_total_height(void)
{
    int h = 8;
    for (int i = 0; i < MENU_ITEMS; i++)
        h += (menu_labels[i][0] == '-') ? MENU_SEP_H : MENU_ITEM_H;
    h += 8;
    return h;
}

static void
menu_draw(surface_t *s)
{
    if (!menu_open) return;
    int mh = menu_total_height();

    /* Translucent dark tint over the key-color base (no local blur — see the
     * file header comment on frosted glass). */
    draw_blend_rect(s, MENU_X, MENU_Y, MENU_W, mh, MENU_BG, 180);

    draw_blend_rect(s, MENU_X, MENU_Y, MENU_W, 1, 0x00FFFFFF, 20);
    draw_blend_rect(s, MENU_X, MENU_Y + mh - 1, MENU_W, 1, 0x00000000, 30);
    draw_blend_rect(s, MENU_X, MENU_Y, 1, mh, 0x00FFFFFF, 15);
    draw_blend_rect(s, MENU_X + MENU_W - 1, MENU_Y, 1, mh, 0x00000000, 20);

    int y = MENU_Y + 8;
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (menu_labels[i][0] == '-') {
            draw_blend_rect(s, MENU_X + 12, y + MENU_SEP_H / 2 - 1,
                            MENU_W - 24, 1, MENU_SEP_COL, 120);
            y += MENU_SEP_H;
        } else {
            if (i == menu_hover)
                draw_blend_rounded_rect(s, MENU_X + 4, y, MENU_W - 8,
                                        MENU_ITEM_H, 6, MENU_HOVER_BG, 150);
            uint32_t item_fg = (i == menu_hover) ? THEME_TEXT_ON_ACCENT
                                                 : MENU_TEXT;
            if (g_font_ui)
                font_draw_text(s, g_font_ui, 14, MENU_X + 16, y + 6,
                               menu_labels[i], item_fg);
            y += MENU_ITEM_H;
        }
    }
}

static glyph_rect_t
menu_rect(void)
{
    return (glyph_rect_t){MENU_X, MENU_Y, MENU_W, menu_total_height()};
}

static int
menu_hit_test(int mx, int my)
{
    if (!menu_open) return -1;
    int mh = menu_total_height();
    if (mx < MENU_X || mx >= MENU_X + MENU_W ||
        my < MENU_Y || my >= MENU_Y + mh)
        return -1;
    int y = MENU_Y + 8;
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (menu_labels[i][0] == '-') {
            y += MENU_SEP_H;
        } else {
            if (my >= y && my < y + MENU_ITEM_H) return i;
            y += MENU_ITEM_H;
        }
    }
    return -1;
}

/* ── Volume popup (vertical slider dropdown) ──────────────────────────────
 * Ported verbatim; s_volume is set directly via the ungated sys_audio_volume
 * syscall (querying/setting volume confers no authority — no cap needed). */
#define SYS_AUDIO_VOLUME 503
static int s_fb_w;
static char s_clock_str[12] = "00:00";
static int s_volume = 80;
static int s_vol_drag = 0;

static void set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_volume = v;
    syscall(SYS_AUDIO_VOLUME, (long)v);
}

static int vol_open;
#define VOLP_W       46
#define VOLP_H       150
#define VOLP_PAD     16
#define VOLP_TRACK_W 6

static glyph_rect_t
vol_popup_rect(void)
{
    int ix = topbar_volume_icon_x(s_fb_w, s_clock_str);
    int x = ix + 5 - VOLP_W / 2;
    if (x < 4) x = 4;
    if (x + VOLP_W > s_fb_w - 4) x = s_fb_w - 4 - VOLP_W;
    int y = BAR_MARGIN_TOP + BAR_H + 6;
    return (glyph_rect_t){x, y, VOLP_W, VOLP_H};
}

static void
vol_track_geom(int *tx, int *ty, int *th)
{
    glyph_rect_t r = vol_popup_rect();
    *tx = r.x + VOLP_W / 2;
    *ty = r.y + VOLP_PAD;
    *th = VOLP_H - 2 * VOLP_PAD;
}

static int
vol_from_y(int my)
{
    int tx, ty, th;
    vol_track_geom(&tx, &ty, &th);
    int v = (ty + th - my) * 100 / th;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

static int
vol_popup_hit(int mx, int my)
{
    if (!vol_open) return 0;
    glyph_rect_t r = vol_popup_rect();
    return mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h;
}

static void
vol_popup_draw(surface_t *s)
{
    if (!vol_open) return;
    glyph_rect_t r = vol_popup_rect();
    int x = r.x, y = r.y, w = r.w, h = r.h;

    draw_blend_rect(s, x, y, w, h, THEME_SURFACE_2, 190);
    draw_blend_rect(s, x, y, w, 1, 0x00FFFFFF, 20);
    draw_blend_rect(s, x, y + h - 1, w, 1, 0x00000000, 30);

    int tx, ty, th;
    vol_track_geom(&tx, &ty, &th);
    int tw = VOLP_TRACK_W;
    draw_blend_rounded_rect(s, tx - tw / 2, ty, tw, th, tw / 2, 0x00FFFFFF, 55);
    int fill = th * s_volume / 100;
    if (fill > 0)
        draw_blend_rounded_rect(s, tx - tw / 2, ty + th - fill, tw, fill,
                                tw / 2, THEME_ACCENT, 255);
    draw_circle_filled(s, tx, ty + th - fill, 6, 0x00FFFFFF);
}

/* ── Wi-Fi status (top-bar icon) ─────────────────────────────────────────── */
#define SYS_NETCFG 500
typedef struct { char ssid[33]; unsigned char channel, sec, connected, pad; } wifi_net_t;
static int s_wifi_n = -1;

static void wifi_refresh(void)
{
    wifi_net_t buf[24];
    long n = syscall(SYS_NETCFG, 2, (long)buf, 24, 0);
    s_wifi_n = (n >= 0) ? (int)n : -1;
}

static int isqrt_i(int n) { int x = 0; while ((x + 1) * (x + 1) <= n) x++; return x; }

static void draw_wifi_glyph(surface_t *s, int cx, int cy, uint32_t col)
{
    const int radii[3] = { 10, 7, 4 };
    for (int i = 0; i < 3; i++) {
        int r = radii[i];
        for (int dx = -r; dx <= r; dx++) {
            int adx = dx < 0 ? -dx : dx;
            int dy = isqrt_i(r * r - dx * dx);
            if (dy >= adx) draw_fill_rect(s, cx + dx, cy - dy, 2, 2, col);
        }
    }
    draw_circle_filled(s, cx, cy, 2, col);
}

static int wifi_icon_cx(int screen_w)
{
    return topbar_volume_icon_x(screen_w, s_clock_str) - 26;
}

static void draw_wifi_status(surface_t *s, int screen_w)
{
    int cx = wifi_icon_cx(screen_w);
    int cy = BAR_MARGIN_TOP + BAR_H - 7;
    uint32_t col = (s_wifi_n > 0) ? THEME_TEXT : THEME_TEXT_DIM;
    draw_wifi_glyph(s, cx, cy, col);
}

/* ── Hardware monitor (CPU temp + battery) ────────────────────────────────
 * Ungated syscall (read-only telemetry confers no authority). */
#define SYS_HWMON 506
typedef struct {
    int           cpu_temp_c, cpu_temp_max_c;
    unsigned char battery_present, battery_percent, battery_charging, ac_online;
    unsigned char reserved[4];
} hwmon_t;
static hwmon_t s_hw = { .cpu_temp_c = -1 };

static void hwmon_refresh(void)
{
    hwmon_t h;
    memset(&h, 0, sizeof h);
    if (syscall(SYS_HWMON, (long)&h, 0, 0, 0) == 0)
        s_hw = h;
    else { s_hw.cpu_temp_c = -1; s_hw.battery_present = 0; }
}

static uint32_t batt_color(int pct)
{
    if (pct > 50) return THEME_OK;
    if (pct > 20) return THEME_WARN;
    return THEME_ERROR;
}

static void draw_hwmon_status(surface_t *s, int screen_w)
{
    int wifi_cx = wifi_icon_cx(screen_w);
    int right   = wifi_cx - 20;
    int ty      = (TOPBAR_HEIGHT - 13) / 2;

    if (s_hw.battery_present) {
        const int BW = 22, BH = 11, NUB = 2;
        int by = (TOPBAR_HEIGHT - BH) / 2;
        int bx = right - NUB - BW;
        uint32_t col = s_hw.battery_charging ? THEME_ACCENT
                                             : batt_color(s_hw.battery_percent);
        draw_rounded_outline(s, bx, by, BW, BH, 2, 1, THEME_TEXT);
        draw_fill_rect(s, bx + BW, by + (BH - 5) / 2, NUB, 5, THEME_TEXT);
        int fillw = (BW - 4) * s_hw.battery_percent / 100;
        if (fillw > 0) draw_fill_rect(s, bx + 2, by + 2, fillw, BH - 4, col);
        char pc[8];
        snprintf(pc, sizeof pc, "%d%%", s_hw.battery_percent);
        int pw = g_font_ui ? font_text_width(g_font_ui, 13, pc) : (int)strlen(pc) * 7;
        if (g_font_ui) font_draw_text(s, g_font_ui, 13, bx - 6 - pw, ty, pc, THEME_TEXT);
        right = bx - 6 - pw - 12;
    }

    if (s_hw.cpu_temp_c >= 0) {
        int mx = s_hw.cpu_temp_max_c > 0 ? s_hw.cpu_temp_max_c : 100;
        uint32_t col = (s_hw.cpu_temp_c >= mx - 5)      ? THEME_ERROR
                     : (s_hw.cpu_temp_c >= mx * 8 / 10) ? THEME_WARN
                                                        : THEME_TEXT;
        char tc[12];
        snprintf(tc, sizeof tc, "%d\xC2\xB0""C", s_hw.cpu_temp_c);
        int tw = g_font_ui ? font_text_width(g_font_ui, 13, tc) : (int)strlen(tc) * 7;
        if (g_font_ui) font_draw_text(s, g_font_ui, 13, right - tw, ty, tc, col);
    }
}

/* ── Top-bar app menu (File/Edit/… for the focused window) ────────────────
 * The focused window's menu now arrives as a pushed LUMEN_EV_MENU_STATE
 * event (col_count==0 = none) instead of an in-process lumen_window_menu()
 * call — see the event loop's LUMEN_EV_MENU_STATE case. Item clicks go back
 * via LUMEN_OP_INVOKE_FOCUSED_MENU (lumen_invoke_focused_menu), which the
 * compositor targets at whatever window is focused at delivery time. */
static lumen_set_menu_t s_menu_state;   /* col_count==0 until MENU_STATE arrives */

static int appmenu_open  = -1;
static int appmenu_hover = -1;
static int appmenu_col_x[LUMEN_MENU_MAX_COLS];
static int appmenu_col_w[LUMEN_MENU_MAX_COLS];
static int appmenu_ncols;

#define APPMENU_TITLE_PAD 10
#define APPMENU_ITEM_H    26
#define APPMENU_SEP_H     10

static const lumen_set_menu_t *appmenu_current(void)
{
    return s_menu_state.col_count > 0 ? &s_menu_state : NULL;
}

static void appmenu_draw_titles(surface_t *s)
{
    appmenu_ncols = 0;
    const lumen_set_menu_t *m = appmenu_current();
    if (!m) return;

    int fh = g_font_ui ? font_height(g_font_ui, 14) : 12;
    int ty = BAR_MARGIN_TOP + (BAR_H - fh) / 2;
    int x  = topbar_appmenu_x();
    for (int c = 0; c < (int)m->col_count && c < LUMEN_MENU_MAX_COLS; c++) {
        const char *title = m->cols[c].title;
        int tw = g_font_ui ? font_text_width(g_font_ui, 14, title)
                           : (int)strlen(title) * 8;
        int w = tw + 2 * APPMENU_TITLE_PAD;
        if (c == appmenu_open)
            draw_blend_rounded_rect(s, x, BAR_MARGIN_TOP + 2, w, BAR_H - 4, 6,
                                    0x00FFFFFF, 34);
        if (g_font_ui)
            font_draw_text(s, g_font_ui, 14, x + APPMENU_TITLE_PAD, ty, title,
                           0x00FFFFFF);
        appmenu_col_x[c] = x;
        appmenu_col_w[c] = w;
        appmenu_ncols = c + 1;
        x += w;
    }
}

static glyph_rect_t appmenu_dropdown_rect(void)
{
    const lumen_set_menu_t *m = appmenu_current();
    if (!m || appmenu_open < 0 || appmenu_open >= (int)m->col_count)
        return (glyph_rect_t){0, 0, 0, 0};
    const lumen_menu_col_t *col = &m->cols[appmenu_open];
    int wmax = 90;
    for (int i = 0; i < (int)col->item_count; i++) {
        const char *lb = col->items[i].label;
        if (!lb[0]) continue;
        int w = g_font_ui ? font_text_width(g_font_ui, 14, lb) : (int)strlen(lb) * 8;
        if (w > wmax) wmax = w;
    }
    int w = wmax + 32;
    int h = 8;
    for (int i = 0; i < (int)col->item_count; i++)
        h += col->items[i].label[0] ? APPMENU_ITEM_H : APPMENU_SEP_H;
    h += 8;
    int x = (appmenu_open < appmenu_ncols) ? appmenu_col_x[appmenu_open]
                                           : topbar_appmenu_x();
    int y = BAR_MARGIN_TOP + BAR_H + 4;
    if (x + w > s_fb_w - 4) x = s_fb_w - 4 - w;
    if (x < 4) x = 4;
    return (glyph_rect_t){x, y, w, h};
}

static void appmenu_dropdown_draw(surface_t *s)
{
    const lumen_set_menu_t *m = appmenu_current();
    if (!m || appmenu_open < 0 || appmenu_open >= (int)m->col_count) return;
    const lumen_menu_col_t *col = &m->cols[appmenu_open];
    glyph_rect_t r = appmenu_dropdown_rect();
    int x = r.x, y = r.y, w = r.w, h = r.h;

    draw_blend_rect(s, x, y, w, h, THEME_SURFACE_2, 190);
    draw_blend_rect(s, x, y, w, 1, 0x00FFFFFF, 20);
    draw_blend_rect(s, x, y + h - 1, w, 1, 0x00000000, 30);

    int iy = y + 8;
    for (int i = 0; i < (int)col->item_count; i++) {
        const char *lb = col->items[i].label;
        if (!lb[0]) {
            draw_blend_rect(s, x + 10, iy + APPMENU_SEP_H / 2 - 1, w - 20, 1,
                            THEME_BORDER, 120);
            iy += APPMENU_SEP_H;
            continue;
        }
        if (i == appmenu_hover)
            draw_blend_rounded_rect(s, x + 4, iy, w - 8, APPMENU_ITEM_H, 6,
                                    THEME_SELECTION, 150);
        uint32_t fg = (i == appmenu_hover) ? THEME_TEXT_ON_ACCENT : THEME_TEXT;
        if (g_font_ui)
            font_draw_text(s, g_font_ui, 14, x + 14, iy + (APPMENU_ITEM_H - 14) / 2,
                           lb, fg);
        iy += APPMENU_ITEM_H;
    }
}

static int appmenu_col_at(int mx, int my)
{
    if (my < BAR_MARGIN_TOP || my >= BAR_MARGIN_TOP + BAR_H) return -1;
    for (int c = 0; c < appmenu_ncols; c++)
        if (mx >= appmenu_col_x[c] && mx < appmenu_col_x[c] + appmenu_col_w[c])
            return c;
    return -1;
}

static int appmenu_item_at(int mx, int my)
{
    const lumen_set_menu_t *m = appmenu_current();
    if (!m || appmenu_open < 0 || appmenu_open >= (int)m->col_count) return -1;
    const lumen_menu_col_t *col = &m->cols[appmenu_open];
    glyph_rect_t r = appmenu_dropdown_rect();
    if (mx < r.x || mx >= r.x + r.w || my < r.y || my >= r.y + r.h) return -1;
    int iy = r.y + 8;
    for (int i = 0; i < (int)col->item_count; i++) {
        if (!col->items[i].label[0]) { iy += APPMENU_SEP_H; continue; }
        if (my >= iy && my < iy + APPMENU_ITEM_H) return i;
        iy += APPMENU_ITEM_H;
    }
    return -1;
}

/* ── Notification toast ──────────────────────────────────────────────────── */
#define NOTIF_W   320
#define NOTIF_H   52
static char s_notif_title[96];
static int  s_notif_wifi;
static long s_notif_expiry;

static void notify_post(const char *title, int wifi_icon, int secs)
{
    strncpy(s_notif_title, title, sizeof(s_notif_title) - 1);
    s_notif_title[sizeof(s_notif_title) - 1] = 0;
    s_notif_wifi = wifi_icon;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    s_notif_expiry = t.tv_sec + secs;
}

static int notify_active(void)
{
    if (!s_notif_expiry) return 0;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    if (t.tv_sec >= s_notif_expiry) { s_notif_expiry = 0; return 0; }
    return 1;
}

static void notify_draw(surface_t *s, int screen_w)
{
    if (!notify_active()) return;
    int x = screen_w - NOTIF_W - 16;
    int y = TOPBAR_HEIGHT + 14;
    draw_rounded_rect(s, x, y, NOTIF_W, NOTIF_H, 12, THEME_SURFACE_2);
    draw_rounded_outline(s, x, y, NOTIF_W, NOTIF_H, 12, 1, THEME_BORDER);
    int tx = x + 20;
    if (s_notif_wifi) {
        draw_wifi_glyph(s, x + 22, y + NOTIF_H - 16, THEME_ACCENT);
        tx = x + 44;
    }
    if (g_font_ui)
        font_draw_text(s, g_font_ui, 14, tx, y + (NOTIF_H - 14) / 2,
                       s_notif_title, THEME_TEXT);
}

/* ── Layout / render ──────────────────────────────────────────────────────
 * The window is normally just TOPBAR_HEIGHT tall; it grows via
 * LUMEN_OP_RESIZE_SELF to cover whichever dropdown/toast is showing (the
 * server keeps it full-width + top-anchored across resizes — see lumen's
 * handle_resize_self), and shrinks back once nothing needs the extra
 * height. */
static int needed_height(void)
{
    int h = TOPBAR_HEIGHT;
    if (menu_open) {
        glyph_rect_t r = menu_rect();
        if (r.y + r.h > h) h = r.y + r.h;
    }
    if (vol_open) {
        glyph_rect_t r = vol_popup_rect();
        if (r.y + r.h > h) h = r.y + r.h;
    }
    if (appmenu_open >= 0) {
        glyph_rect_t r = appmenu_dropdown_rect();
        if (r.y + r.h > h) h = r.y + r.h;
    }
    if (notify_active()) {
        int b = TOPBAR_HEIGHT + 14 + NOTIF_H;
        if (b > h) h = b;
    }
    return h;
}

static void render(lumen_window_t *win)
{
    surface_t s = backbuf_surface(win);
    /* Key-color base: lumen's compositor replaces every C_TERM_BG pixel with
     * a real blur+tint of the desktop behind us (see the file header). */
    for (int i = 0; i < s.w * s.h; i++)
        s.buf[i] = C_TERM_BG;

    topbar_draw(&s, s_fb_w, s_clock_str, s_volume, 1 /* always recompute —
                 we redraw from a fresh key-color buffer every frame */);
    draw_wifi_status(&s, s_fb_w);
    draw_hwmon_status(&s, s_fb_w);
    appmenu_draw_titles(&s);
    menu_draw(&s);
    vol_popup_draw(&s);
    appmenu_dropdown_draw(&s);
    notify_draw(&s, s_fb_w);

    lumen_window_present(win);
}

/* Resize (if the required height changed) then repaint. win's fields are
 * updated in place by lumen_window_resize_self — no need to rebind callers. */
static void relayout_and_render(lumen_window_t *win)
{
    int h = needed_height();
    if (h != win->h)
        lumen_window_resize_self(win, s_fb_w, h);
    render(win);
}

/* ── Mouse handling ───────────────────────────────────────────────────────
 * x,y arrive window-local per the protocol; since this panel is always
 * anchored at screen (0,0), window-local == screen-absolute, so every
 * hit-test below (written for screen coordinates originally) is unchanged. */
static void handle_mouse(lumen_window_t *win, int fd, const lumen_event_t *ev)
{
    int x = ev->mouse.x, y = ev->mouse.y;
    int redraw = 0;

    if (ev->mouse.evtype == LUMEN_MOUSE_UP) {
        if (s_vol_drag) { s_vol_drag = 0; redraw = 1; }
    } else if (ev->mouse.evtype == LUMEN_MOUSE_MOVE) {
        if ((ev->mouse.buttons & 1) && s_vol_drag) {
            set_volume(vol_from_y(y));
            redraw = 1;
        }
        if (menu_open) {
            int h = menu_hit_test(x, y);
            if (h != menu_hover) { menu_hover = h; redraw = 1; }
        }
        if (appmenu_open >= 0) {
            int h = appmenu_item_at(x, y);
            if (h != appmenu_hover) { appmenu_hover = h; redraw = 1; }
        }
    } else if (ev->mouse.evtype == LUMEN_MOUSE_DOWN && (ev->mouse.buttons & 1)) {
        if (menu_open) {
            int item = menu_hit_test(x, y);
            if (item >= 0 && menu_labels[item][0] != '-') {
                switch (item) {
                case MENU_ITEM_ABOUT:      lumen_invoke(fd, "about"); break;
                case MENU_ITEM_LOCK:       lumen_invoke(fd, "lock"); break;
                case MENU_ITEM_REBOOT:     lumen_invoke(fd, "reboot"); break;
                case MENU_ITEM_POWEROFF:   lumen_invoke(fd, "poweroff"); break;
                case MENU_ITEM_SETTINGS:   lumen_invoke(fd, "settings"); break;
                case MENU_ITEM_APPS:       lumen_invoke(fd, "applications"); break;
                case MENU_ITEM_FILES:      lumen_invoke(fd, "filemanager"); break;
                case MENU_ITEM_EDITOR:     lumen_invoke(fd, "editor"); break;
                case MENU_ITEM_CALCULATOR: lumen_invoke(fd, "calculator"); break;
                default: break;
                }
            }
            menu_open = 0; menu_hover = -1;
            redraw = 1;
        } else if (appmenu_open >= 0) {
            int col  = appmenu_col_at(x, y);
            int item = appmenu_item_at(x, y);
            if (col >= 0) {
                appmenu_open = (col == appmenu_open) ? -1 : col;
                appmenu_hover = -1;
            } else if (item >= 0) {
                const lumen_set_menu_t *m = appmenu_current();
                if (m) {
                    uint32_t cmd = m->cols[appmenu_open].items[item].command;
                    lumen_invoke_focused_menu(fd, cmd);
                }
                appmenu_open = -1; appmenu_hover = -1;
            } else {
                appmenu_open = -1; appmenu_hover = -1;
            }
            redraw = 1;
        } else if (vol_open && !topbar_volume_icon_hit(x, y, s_fb_w, s_clock_str)) {
            if (vol_popup_hit(x, y)) {
                set_volume(vol_from_y(y));
                s_vol_drag = 1;
            } else {
                vol_open = 0;
            }
            redraw = 1;
        } else {
            int col = appmenu_col_at(x, y);
            if (col >= 0) {
                menu_open = 0; menu_hover = -1;
                vol_open = 0;
                appmenu_open = col; appmenu_hover = -1;
                redraw = 1;
            } else if (topbar_volume_icon_hit(x, y, s_fb_w, s_clock_str)) {
                vol_open = !vol_open;
                if (vol_open) {
                    menu_open = 0; menu_hover = -1;
                    appmenu_open = -1; appmenu_hover = -1;
                }
                redraw = 1;
            } else if (topbar_hit_aegis(x, y, s_fb_w)) {
                menu_open = !menu_open; menu_hover = -1;
                if (menu_open) {
                    vol_open = 0;
                    appmenu_open = -1; appmenu_hover = -1;
                }
                redraw = 1;
            }
        }
    }

    if (redraw) relayout_and_render(win);
}

/* ── Periodic tick: clock, Wi-Fi/hwmon polling, theme-pref reload, the
 * first-boot Wi-Fi notification, notification expiry. Runs once per
 * wall-clock second, same gate the old in-process loop used — called every
 * iteration of the event loop (event-paced, not sleep-paced) so it stays on
 * time regardless of how busy the loop is. ───────────────────────────────── */
static void tick(lumen_window_t *win)
{
    static time_t last_sec = (time_t)-1;
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    if (mono.tv_sec == last_sec) return;
    last_sec = mono.tv_sec;

    int redraw = 0;

    glyph_theme_reload_prefs();
    {
        static int p_light = -1, p_wp = -1, p_nl = -1;
        int lt = glyph_theme_light();
        int wp = glyph_theme_wallpaper();
        int nl = glyph_theme_night_light();
        if (p_light != -1 && (lt != p_light || wp != p_wp || nl != p_nl))
            redraw = 1;
        p_light = lt; p_wp = wp; p_nl = nl;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        time_t local = ts.tv_sec + glyph_theme_tz_offset() * 60;
        struct tm *tm = gmtime(&local);
        if (tm) {
            char nc[12];
            if (glyph_theme_clock24())
                snprintf(nc, sizeof(nc), "%02d:%02d", tm->tm_hour, tm->tm_min);
            else {
                int h12 = tm->tm_hour % 12; if (h12 == 0) h12 = 12;
                snprintf(nc, sizeof(nc), "%d:%02d %s", h12, tm->tm_min,
                         tm->tm_hour < 12 ? "AM" : "PM");
            }
            if (strcmp(nc, s_clock_str) != 0) {
                memcpy(s_clock_str, nc, sizeof(s_clock_str));
                redraw = 1;
            }
        }
    }

    int prev_wifi = s_wifi_n;
    wifi_refresh();
    int prev_t = s_hw.cpu_temp_c;
    int prev_b = s_hw.battery_present ? s_hw.battery_percent + 1 : 0;
    hwmon_refresh();
    int now_b = s_hw.battery_present ? s_hw.battery_percent + 1 : 0;
    if (prev_wifi != s_wifi_n || prev_t != s_hw.cpu_temp_c || prev_b != now_b)
        redraw = 1;

    static int notif_shown = 0, upkeep_ticks = 0;
    upkeep_ticks++;
    if (!notif_shown && s_wifi_n > 0 && upkeep_ticks >= 2) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d Wi-Fi network%s available.",
                 s_wifi_n, s_wifi_n == 1 ? " is" : "s are");
        notify_post(msg, 1, 12);
        notif_shown = 1;
        redraw = 1;
    }

    static int was_notif_active = 0;
    int now_active = notify_active();
    if (now_active != was_notif_active) redraw = 1;
    was_notif_active = now_active;

    if (redraw) relayout_and_render(win);
}

int main(void)
{
    int fd = -1;
    for (;;) {
        fd = lumen_connect();
        if (fd >= 0) break;
        if (fd != -111) {
            char buf[96];
            int n = snprintf(buf, sizeof(buf),
                "[SHELL] lumen_connect=%d (giving up)\n", fd);
            if (n > 0) log_console(buf);
            return 1;
        }
        struct timespec ts = { 0, 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    font_init();

    /* Requested width is ignored for a top-anchored panel — the server
     * always stretches it to the full screen width (see lumen's
     * handle_create_common). Height is honored. */
    lumen_window_t *win = lumen_panel_create_anchored(fd, 1, TOPBAR_HEIGHT,
                                                       LUMEN_PANEL_TOP);
    if (!win) {
        log_console("[SHELL] FAIL: panel_create_anchored returned NULL\n");
        close(fd);
        return 1;
    }
    s_fb_w = win->w;

    wifi_refresh();
    hwmon_refresh();
    render(win);

    log_console("[SHELL] ready\n");

    for (;;) {
        lumen_event_t ev;
        int r = lumen_wait_event(fd, &ev, 250);
        if (r < 0) {
            log_console("[SHELL] connection lost\n");
            break;
        }
        if (r > 0) {
            switch (ev.type) {
            case LUMEN_EV_MOUSE:
                handle_mouse(win, fd, &ev);
                break;
            case LUMEN_EV_MENU_STATE: {
                uint32_t old_id = s_menu_state.window_id;
                int old_had = s_menu_state.col_count > 0;
                s_menu_state = *ev.menu_state.menu;
                int new_had = s_menu_state.col_count > 0;
                if (s_menu_state.window_id != old_id || old_had != new_had) {
                    appmenu_open = -1;
                    appmenu_hover = -1;
                }
                relayout_and_render(win);
                break;
            }
            case LUMEN_EV_CLOSE_REQUEST:
                /* Ignore — the shell is unkillable from the UI, same as
                 * the dock. */
                break;
            default:
                break;
            }
        }
        tick(win);
    }

    lumen_window_destroy(win);
    close(fd);
    return 0;
}
