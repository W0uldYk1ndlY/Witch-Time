/* Self-test of the PC settings menu without the game: graphics.ini rewritten in place (gfx_option_set, gfx_ini_get) and the
 * panel drawn at two window sizes (pcmenu.c), the pictures written as BMP into the folder given (for a look).
 *   clang -D_GNU_SOURCE -D'__declspec(x)=' -Iruntime/posix -Iruntime -Iruntime/gen tests/pcmenu_selftest.c runtime/gfx_options.c \
 *         runtime/plat_posix.c -lm -o /tmp/pcmenu_selftest && /tmp/pcmenu_selftest /tmp/pcmenu_test */
#include "../runtime/pcmenu.c"
#include <assert.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
static int setenv(const char *name, const char *value, int overwrite) {
    return !overwrite && getenv(name) ? 0 : _putenv_s(name, value);
}
static int unsetenv(const char *name) { return _putenv_s(name, ""); }
#endif

volatile LONG g_guest_frozen, g_display_close;
volatile int game_cutscene_fps, game_model_lod;
volatile int game_hud_centre;
static int g_vsync_applied, g_win_applied;
void nnhid_menu_read(uint64_t *b, int *lx, int *ly, int swap_ab) { (void)swap_ab; *b = 0; *lx = *ly = 0; }
static int g_menu_back;
int nnhid_menu_key(int vk) { return vk == VK_ESCAPE && g_menu_back; }
int nvn_front_post_pcmenu(void) { return 1; }
int nvn_vk_hdr_on(void) { return 0; }
void display_vsync_apply(void) { g_vsync_applied++; }
int display_window_apply(void) { g_win_applied++; return 1; }
void display_mouse_want(int on) { (void)on; }
int nnpad_mouse_cfg(float *s, int *i) { (void)s; (void)i; return 0; }
static const char *g_prompts = "playstation";
const char *nnpad_prompts_setting(void) { return g_prompts; }
int nnpad_prompts_set(const char *val) { g_prompts = val; return 1; }
static const char *g_menu_swap = "off";
const char *nnpad_menu_swap_setting(void) { return g_menu_swap; }
int nnpad_menu_swap_set(const char *val) { g_menu_swap = val; return 1; }
int nnpad_menu_swapped(void) { return !strcmp(g_menu_swap, "on"); }
static const char *g_layout = "auto";
const char *nnpad_layout_setting(void) { return g_layout; }
int nnpad_layout_set(const char *val) { g_layout = val; return 1; }
int nnpad_button_prompts(void) { return 3; }
int nnpad_prompts_by_letter(void) { return 1; }
static uint8_t g_saved[2][16];
static int g_saved_on[2], g_type[2];
int nnpad_bind_get(int ch, unsigned char *o) { if (g_saved_on[ch]) memcpy(o, g_saved[ch], 16); return g_saved_on[ch]; }
int nnpad_bind_set(int ch, const unsigned char *b) { g_saved_on[ch] = b != NULL; if (b) memcpy(g_saved[ch], b, 16); return 1; }
int options_control_type(int ch) { return g_type[ch]; }
static uint32_t g_pad;
uint32_t nnhid_menu_pad(void) { return g_pad; }
int display_mouse_pixels(int *x, int *y) { *x = *y = -1; return 0; }
SHORT GetAsyncKeyState(int vk) { (void)vk; return 0; }

static char *slurp(const char *path) { FILE *f = fopen(path, "rb"); static char b[16384]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0; return strdup(b); }
static void spit(const char *path, const char *s) { FILE *f = fopen(path, "wb"); fputs(s, f); fclose(f); }
static void save_bmp(const char *path, const uint8_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    uint8_t head[54] = {'B', 'M'};
    uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h, v;
    memcpy(head + 2, &size, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4); head[26] = 1; head[28] = 24;
    fwrite(head, 1, 54, f);
    uint8_t *r = calloc(1, row);
    for (int y = h - 1; y >= 0; y--) { for (int x = 0; x < w; x++) memcpy(r + x * 3, px + ((size_t)y * w + x) * 4, 3); fwrite(r, 1, row, f); }
    free(r); fclose(f);
}
static void panel(const char *dir, const char *name, int W, int H) {
    int r[4]; unsigned gen;
    assert(pcmenu_panel_size(W, H, r, &gen));
    assert(r[0] >= 0 && r[1] >= 0 && r[0] + r[2] <= W && r[1] + r[3] <= H && r[2] > 100 && r[3] > 100);
    uint8_t *px = malloc((size_t)r[2] * r[3] * 4);
    pcmenu_panel_read(px, r[2], r[3]);
    char path[300]; snprintf(path, sizeof path, "%s/%s.bmp", dir, name);
    save_bmp(path, px, r[2], r[3]);
    printf("panel %dx%d in a %dx%d window: %s\n", r[2], r[3], W, H, path);
    free(px);
}
#ifdef __ANDROID__
#include "diag.h"
bool diag_is_enabled(void) { return false; }
static void finger(int type, float x, float y) { touch_runtime_event(type, 1, 77, x, y); }
static void finger_tap(float x, float y) { finger(0, x, y); finger(2, x, y); Sleep(100); }
static void redraw_menu(void) { int r[4]; unsigned generation; assert(pcmenu_panel_size(1920, 1080, r, &generation)); }
#endif

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    char ini[300], buf[64];
#ifdef __ANDROID__
    TouchPointer queued;
#endif
    snprintf(ini, sizeof ini, "%s/graphics_test.ini", dir);
    mkdir(dir, 0755);
#ifdef __ANDROID__
    char touch_path[300];
    snprintf(touch_path, sizeof touch_path, "%s/controllers.ini", dir);
    touch_runtime_init(touch_path);
    touch_runtime_focus(1, 1);
    touch_runtime_geometry((TouchRect){0, 0, 1920, 1080}, 1, 1920, 1080);
    touch_runtime_set_ready(1);
    assert(!touch_runtime_snapshot().presentation.valid);
    touch_runtime_presentation((TouchPresentation){1, 1920, 1080, 1920, 1080, 1, 0});
    assert(touch_runtime_snapshot().presentation.valid);
#endif
    /* a file of CRLF lines with comments, an unknown key, a key twice, no newline at the end */
    spit(ini, "; comment = not a key\r\nwindow = windowed\r\nvsync   =   auto   \r\nmystery = 7\r\naspect_ratio = 21:9\r\nhud_area = 16:9\r\n[section]\r\nvsync = clock\r\nresolution_scale = 1.0");
    setenv("WT_GRAPHICS_CONFIG", ini, 1);
    /* a value not in a row's list (v0.5.2's 1.4, a hand-edited 16:10): the nearest one that way; none: left / right stay */
    int rs = row_of("resolution_scale"), ar = row_of("aspect_ratio"), ws = row_of("size");
    snprintf(g_raw[rs], sizeof g_raw[rs], "1.4");
    assert(!strcmp(k_rows[rs].val[nearest(rs, 1, 0)], "1.6") && !strcmp(k_rows[rs].val[nearest(rs, -1, 0)], "1.2"));
    snprintf(g_raw[rs], sizeof g_raw[rs], "9");
    assert(nearest(rs, 1, 0) == -1 && nearest(rs, 1, 1) == 0 && !strcmp(k_rows[rs].val[nearest(rs, -1, 0)], "3.0"));
    snprintf(g_raw[ar], sizeof g_raw[ar], "16:10");
    assert(!strcmp(k_rows[ar].val[nearest(ar, -1, 0)], "16:9") && nearest(ar, 1, 0) == -1);
    snprintf(g_raw[ws], sizeof g_raw[ws], "1280x800");
    assert(!strcmp(k_rows[ws].val[nearest(ws, -1, 0)], "1280x720") && !strcmp(k_rows[ws].val[nearest(ws, 1, 0)], "1600x900"));
    int vs = row_of("vsync");                              /* a number in a row of words: the ends, as before */
    snprintf(g_raw[vs], sizeof g_raw[vs], "1");
    assert(nearest(vs, 1, 0) == 0 && nearest(vs, -1, 0) == row_count(vs) - 1);
    g_raw[rs][0] = g_raw[ar][0] = g_raw[ws][0] = g_raw[vs][0] = 0;
    assert(gfx_options()->present == 0);                   /* the last vsync line wins: clock */
    assert(gfx_ini_get("vsync", buf, sizeof buf) && !strcmp(buf, "clock"));
    assert(fabsf(gfx_options()->aspect_ratio - 21.0f / 9.0f) < 1e-5f && fabs(atof(getenv("RECOMPS_ASPECT")) - 21.0 / 9.0) < 1e-5);      /* <w>:<h>, handed on */
    assert(gfx_options()->hud_centre == 1 && !strcmp(getenv("RECOMPS_HUD_CENTRE"), "1"));
    assert(!gfx_ini_get("aspect", buf, sizeof buf));
    assert(gfx_option_set("vsync", "off") && gfx_options()->present == 2);
    char *s = slurp(ini);
    assert(!strcmp(s, "; comment = not a key\r\nwindow = windowed\r\nvsync   =   off   \r\nmystery = 7\r\naspect_ratio = 21:9\r\nhud_area = 16:9\r\n[section]\r\nvsync = off\r\nresolution_scale = 1.0"));
    free(s);
    assert(gfx_option_set("aspect", "stretch") && gfx_options()->aspect_stretch == 1);
    s = slurp(ini);
    assert(strstr(s, "resolution_scale = 1.0\r\naspect = stretch\r\n") && strstr(s, "mystery = 7\r\n"));
    free(s);
    assert(gfx_option_set("resolution_scale", "2.4") && gfx_options()->scale == 2.4f);
    spit(ini, "window = windowed\nwidth = 1280\nheight = 720\nvsync = auto\nresolution_scale = 1.6\nanisotropic_filtering = 8\n");      /* LF file */
    assert(gfx_option_set("width", "1920") && gfx_option_set("height", "1080") && gfx_options()->width == 1920 && gfx_options()->height == 1080);
    s = slurp(ini);
    assert(!strcmp(s, "window = windowed\nwidth = 1920\nheight = 1080\nvsync = auto\nresolution_scale = 1.6\nanisotropic_filtering = 8\n"));
    free(s);
    /* opening from the pad: Start + Select together, not one of them and not the sticks */
    assert(pcmenu_gate(0, 0x30) == 0 && pcmenu_gate(0, 0) == 0 && pcmenu_gate(0, 1u << 10) == 0 && pcmenu_gate(0, 1u << 11) == 0 && !g_open);
    assert(pcmenu_gate(0, 0) == 0 && pcmenu_gate(0, 0xc00) == 1 && g_open);
    InterlockedExchange(&g_open, 0); Sleep(200);
    /* the menu's rows from that file; changes through the menu */
    g_type[1] = 2;                                         /* Viola on the game's Type C */
    menu_open("test", 0, 0);                              /* (its thread runs on with the stubs' idle input) */
    assert(g_cur[5] == 7 && !strcmp(val_name(5), "1440p"));
    assert(gfx_options()->aniso == 16 && gfx_options()->texture_lod_bias == -1.0f);      /* fixed: the file's anisotropic_filtering = 8 is not used */
    assert(!gfx_options()->model_lod_high && g_cur[6] == 0);
    change(6, 1, 0); assert(gfx_options()->model_lod_high && game_model_lod == 2 && !row_restart(6));
    assert(gfx_ini_get("model_lod", buf, sizeof buf) && !strcmp(buf, "highest"));
    change(6, -1, 0); assert(!gfx_options()->model_lod_high && game_model_lod == 1);
    assert(!gfx_options()->no_film_grain && g_cur[7] == 0);      /* not in the file: the game's own */
    change(7, 1, 0); assert(gfx_options()->no_film_grain && !row_restart(7));
    assert(gfx_ini_get("film_grain", buf, sizeof buf) && !strcmp(buf, "off"));
    change(7, -1, 0); assert(!gfx_options()->no_film_grain);
    assert(!gfx_options()->upscaler && g_cur[8] == 0);      /* issue 118: not in the file: off */
    setenv("WT_UPSCALER", "fsr3_quality", 1);
    assert(gfx_option_set("upscaler", "sgsr2_performance") && gfx_options()->upscaler == 2);
    assert(gfx_ini_get("upscaler", buf, sizeof buf) && !strcmp(buf, "sgsr2_performance"));
    unsetenv("WT_UPSCALER");
    assert(gfx_option_set("upscaler", "off") && !gfx_options()->upscaler);
    setenv("WT_SGSR_VARIANT", "bad", 1); assert(gfx_sgsr_variant() == -1);
    setenv("WT_SGSR_VARIANT", "2-pass-cs", 1); assert(gfx_sgsr_variant() == 1);
    unsetenv("WT_SGSR_VARIANT"); assert(gfx_sgsr_variant() == 0);
    pcmenu_take_work(); change(8, 1, 0); assert(gfx_options()->upscaler == 1 && row_restart(8) && !(g_work & 4));      /* applies when the panel closes */
    assert(gfx_ini_get("upscaler", buf, sizeof buf) && !strcmp(buf, "fsr3"));
    change(8, 1, 0); change(8, 1, 0); change(8, 1, 0); change(8, 1, 0); assert(gfx_options()->upscaler == 5 && !strcmp(val_name(8), "Ultra Performance"));
    assert(gfx_ini_get("upscaler", buf, sizeof buf) && !strcmp(buf, "fsr3_ultra_performance"));
    change(8, 1, 0); assert(gfx_options()->upscaler == 6 && !strcmp(val_name(8), "SGSR Native AA"));
    change(8, 1, 0); assert(gfx_options()->upscaler == 7 && gfx_ini_get("upscaler", buf, sizeof buf) && !strcmp(buf, "sgsr2_quality"));
    change(8, 1, 0); assert(gfx_options()->upscaler == 8 && gfx_ini_get("upscaler", buf, sizeof buf) && !strcmp(buf, "sgsr2_performance"));
    for (int k = 0; k < 8; k++) change(8, -1, 0);
    assert(!gfx_options()->upscaler);
    change(2, 1, 0); assert(gfx_options()->present == 4 && g_vsync_applied == 1 && (g_work & 2));      /* auto -> display */
    change(3, 1, 0); assert(game_cutscene_fps == 60);
    pcmenu_take_work(); change(5, 1, 0); assert(row_restart(5) && !(g_work & 4));      /* 1.6 -> 2.0: applies when the panel closes */
    change(5, 1, 0); change(5, 1, 0); change(5, 1, 0); assert(g_cur[5] == 10);      /* 3.0, the last: right stops there */
    change(5, 1, 1); assert(g_cur[5] == 0); change(5, 1, 0); change(5, 1, 0); change(5, 1, 0); change(5, 1, 0); change(5, 1, 0);      /* A goes round */
    g_sel = 5; g_ver++;
    panel(dir, "pcmenu_1280x720", 1280, 720);
    panel(dir, "pcmenu_3840x2160", 3840, 2160);
    g_sel = 2; g_ver++;
    panel(dir, "pcmenu_1920x1080", 1920, 1080);
    /* the Controls page. No layout yet: the game's type shows; a change makes a layout from it, and the button that had it moves */
    int row[16], preset = -1, chr = -1;
    for (int i = 0; i < NROW; i++) { if (k_rows[i].apply == A_BIND) row[k_rows[i].slot] = i; if (k_rows[i].apply == A_PRESET) preset = i; if (k_rows[i].apply == A_CHAR) chr = i; }
    assert(!row_on(row[RB_ZR]) && row_on(0));
    AcquireSRWLockExclusive(&g_lock); page_go(1); g_ver++; ReleaseSRWLockExclusive(&g_lock);
    assert(row_on(row[RB_ZR]) && !row_on(0) && g_sel == chr && g_cur[preset] == 0);
    panel(dir, "controls_1920x1080", 1920, 1080);
    panel(dir, "controls_1280x720", 1280, 720);
    assert(!strcmp(val_name(row[RB_ZR]), "RT") && !strcmp(val_name(row[0]), "A"));        /* an Xbox pad, by letter */
    set_value(row[RB_ZR], RB_R);                           /* issue 82: dodge on the bumper */
    assert(g_saved_on[0] && g_saved[0][RB_ZR] == RB_R && g_saved[0][RB_R] == RB_ZR && rb_valid(g_saved[0]));
    assert(g_cur[row[RB_R]] == RB_ZR && !strcmp(val_name(row[RB_R]), "RT") && !strcmp(g_raw[preset], "Custom") && g_cur[preset] < 0);
    change(chr, 1, 0); assert(g_ch == 1 && !g_saved_on[1] && g_cur[row[RB_L]] == RB_R);    /* Viola: her Type C, nothing saved */
    change(row[9], 1, 1); assert(g_saved_on[1] && rb_valid(g_saved[1]));                   /* left / right on a row go round and swap too */
    set_value(preset, 4); assert(g_saved_on[1] && g_saved[1][RB_ZR] == RB_R && g_cur[preset] == 4);      /* her Type D */
    set_value(preset, 0); assert(!g_saved_on[1] && g_cur[preset] == 0 && g_cur[row[RB_L]] == RB_R);      /* the game's setting again */
    change(chr, -1, 0); assert(g_ch == 0 && g_cur[row[RB_ZR]] == RB_R);
    /* a press on the row: what is down has to go up first, then the next button is taken and does nothing else */
    AcquireSRWLockExclusive(&g_lock); g_sel = row[RB_ZR]; activate(g_sel, 1); ReleaseSRWLockExclusive(&g_lock);
    assert(g_cap == row[RB_ZR] && !strcmp(val_name(g_cap), "Press a button\xE2\x80\xA6"));
    g_pad = 1u << 2; Sleep(100); assert(g_cap >= 0);       /* held from before */
    g_pad = 0; Sleep(100); g_pad = 1u << 3; Sleep(100); assert(g_cap < 0 && g_saved[0][RB_ZR] == 3 && rb_valid(g_saved[0]));
    g_pad = 0; Sleep(100);
    assert(g_sel == row[RB_ZR] && g_page == 1);
    /* back: to the first page, not out of the panel */
    AcquireSRWLockExclusive(&g_lock); page_go(0); g_ver++; ReleaseSRWLockExclusive(&g_lock);
    /* closing: every player reads an idle pad until that player has let go (player 2 = a keyboard holding Esc = Plus) */
    InterlockedExchange(&g_open, 0); Sleep(200);           /* the menu's thread ends: what it does at a close */
    assert(g_release == 0xff && !g_guest_frozen && (g_work & 4) && !row_restart(5));
    pcmenu_take_work();
    assert(pcmenu_gate(1, 1u << 10) == 1);                 /* player 2 still holds Plus: not given */
    assert(pcmenu_gate(0, 0) == 0);                        /* player 1 let go: its input is back at once */
    assert(pcmenu_gate(1, 1u << 10) == 1);                 /* player 1's release does not free player 2 */
    assert(pcmenu_gate(1, 0) == 0 && pcmenu_gate(1, 1u << 10) == 0);      /* let go, then a new press reaches the game */
    assert(pcmenu_gate(0, 1u << 0) == 0);
    /* the game's Options rows (options_row.c): by key, through the same change path */
    int pending;
    pcmenu_key_set("aspect", "keep"); assert(gfx_options()->aspect_stretch == 0 && !strcmp(pcmenu_key_get("aspect", NULL), "keep"));
    pcmenu_key_set("resolution_scale", "1.6"); assert(!strcmp(pcmenu_key_get("resolution_scale", &pending), "1.6") && !pending && (g_work & 4));      /* at once */
    pcmenu_key_set("resolution_scale", "2.4"); assert(!strcmp(pcmenu_key_get("resolution_scale", &pending), "2.4") && !pending && gfx_options()->scale == 2.4f);
    pcmenu_key_set("resolution_scale", "7"); assert(!strcmp(pcmenu_key_get("resolution_scale", NULL), "2.4"));      /* none of the row's */
    s = slurp(ini); assert(strstr(s, "resolution_scale = 2.4\n") && strstr(s, "anisotropic_filtering = 8\n")); free(s);      /* an old key's line stays as it was */
    assert(!strcmp(pcmenu_key_get("no_such_key", NULL), ""));
    pcmenu_key_set("swap_confirm", "on"); assert(!strcmp(g_menu_swap, "on") && !strcmp(pcmenu_key_get("swap_confirm", NULL), "on"));      /* a key of controllers.ini */
    pcmenu_key_set("swap_confirm", "off"); assert(!strcmp(g_menu_swap, "off"));
    pcmenu_key_set("layout", "labels"); assert(!strcmp(g_layout, "labels") && !strcmp(pcmenu_key_get("layout", NULL), "labels"));
    /* a new version over a player's file: its comments and keys, the player's values and keys, the old file kept */
    char old[320], bak[330];
    snprintf(old, sizeof old, "%s/graphics_old.ini", dir); snprintf(bak, sizeof bak, "%s.bak", old);
    const char *player = "; my note\r\n;   The small \"PC SETTINGS  F1 L3 + R3\" note\r\nvsync = off\r\nVSYNC = on\r\nmystery = 7\r\nwindow=borderless\r\nanisotropic_filtering = 8";
    remove(old);
    assert(gfx_ini_refresh(old) == 0);                     /* no file: nothing to do */
    spit(old, player);
    assert(gfx_ini_refresh(old) == 1);
    s = slurp(old);
    assert(strstr(s, "Start + Select") && !strstr(s, "L3 + R3") && !strstr(s, "my note"));
    assert(strstr(s, "\r\nvsync = on\r\n") && strstr(s, "\r\nwindow = borderless\r\n") && strstr(s, "\r\nanisotropic_filtering = 8\r\n"));      /* the last line with a key won */
    assert(strstr(s, "\r\nhdr = auto\r\n") && strstr(s, "\r\n\r\nmystery = 7\r\n") && !strcmp(slurp(bak), player));
    assert(gfx_ini_refresh(old) == 0 && !strcmp(slurp(old), s));      /* once */
    spit(old, "; my note\r\n"); { FILE *f = fopen(old, "ab"); fputs(s, f); fclose(f); }
    assert(gfx_ini_refresh(old) == 0 && strstr(slurp(old), "my note"));      /* the player's comment stays while the defaults stand */
    s = slurp("config/graphics.ini");                      /* exercise LF even when Git checks the shipped file out as CRLF */
    { char *dst = s; for (const char *src = s; *src; src++) if (*src != '\r') *dst++ = *src; *dst = 0; }
    assert(!strstr(s, "\r\n"));
    spit(old, s);
    assert(gfx_ini_refresh(old) == 0);
    { char *d = strstr(s, "\nvsync = auto\n") + 1; memmove(d, d + 13, strlen(d + 13) + 1); }      /* a key gone: LF stays */
    spit(old, s);
    assert(gfx_ini_refresh(old) == 1 && !strstr(slurp(old), "\r\n") && strstr(slurp(old), "\nvsync = auto\n"));
#ifdef __ANDROID__
    TouchConfig cfg = touch_runtime_snapshot().config;
    cfg.enabled = 0;
    assert(touch_runtime_save(&cfg));
    TouchSnapshot touch = touch_runtime_snapshot();
    finger(0, touch.settings.x + 1, touch.settings.y + 1);
    assert(!touch_runtime_take_settings());
    pcmenu_gate(0, 0);
    assert(pcmenu_gate(0, 0xc00) == 1 && g_open); /* OFF recovery uses the physical controller chord. */
    finger(2, touch.settings.x + 1, touch.settings.y + 1);
    Sleep(100);
    assert(g_open && g_guest_frozen && !touch_runtime_snapshot().pad.buttons);
    finger(0, touch.geometry.control[TOUCH_A].x + 1, touch.geometry.control[TOUCH_A].y + 1);
    assert(!touch_runtime_snapshot().pad.buttons);
    finger(3, touch.geometry.control[TOUCH_A].x + 1, touch.geometry.control[TOUCH_A].y + 1);
    redraw_menu();
    for (int i = 0; i < 4; i++) { finger(0, 1100, 800); finger(1, 1100, 300); finger(2, 1100, 300); Sleep(100); redraw_menu(); }
    int touch_page = row_of("touch_page");
    assert(touch_page >= 0 && g_row_y[touch_page] >= 0);
    int target_x = g_rect[0] + (g_row_x0[touch_page] + g_row_x1[touch_page]) / 2;
    int target_y = g_rect[1] + g_row_y[touch_page] + g_row_h / 2;
    touch_runtime_presentation((TouchPresentation){4, 1920, 1080, 1920, 1080, 1, 0});
    finger_tap(target_x, target_y); /* Ordinary menu layout and hit testing both stay in SDL pixels. */
    assert(g_page == PAGE_TOUCH);
    touch_runtime_presentation((TouchPresentation){1, 1920, 1080, 1920, 1080, 1, 0});
    redraw_menu();
    int enabled = row_of("touch_enabled");
#ifdef _WIN32
    char saved_touch[300]; snprintf(saved_touch, sizeof saved_touch, "%s/touch_controls.ini", dir);
    HANDLE held_touch = CreateFileA(saved_touch, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(held_touch != INVALID_HANDLE_VALUE);
    finger_tap(g_rect[0] + (g_row_x0[enabled] + g_row_x1[enabled]) / 2, g_rect[1] + g_row_y[enabled] + g_row_h / 2);
    assert(g_touch_error && !touch_runtime_snapshot().config.enabled);
    assert(CloseHandle(held_touch));
#endif
    finger_tap(g_rect[0] + (g_row_x0[enabled] + g_row_x1[enabled]) / 2, g_rect[1] + g_row_y[enabled] + g_row_h / 2);
    assert(touch_runtime_snapshot().config.enabled);
    redraw_menu();
    int edit = row_of("touch_edit");
    finger_tap(g_rect[0] + (g_row_x0[edit] + g_row_x1[edit]) / 2, g_rect[1] + g_row_y[edit] + g_row_h / 2);
    assert(touch_runtime_snapshot().editing && g_guest_frozen);
    /* Editor coordinates also stay in SDL pixels, bypassing ordinary-menu gesture handling. */
    TouchSnapshot editor_snapshot = touch_runtime_snapshot();
    TouchRect editor_target = editor_snapshot.geometry.control[TOUCH_B];
    touch_runtime_presentation((TouchPresentation){4, 1920, 1080, 1920, 1080, 1, 0});
    touch_runtime_menu(1);
    finger(0, editor_target.x + editor_target.w / 2, editor_target.y + editor_target.h / 2);
    assert(touch_runtime_pointer(&queued));
    assert(touch_runtime_editor_event(queued) == 0);
    assert(touch_runtime_snapshot().selected == TOUCH_B);
    finger(3, editor_target.x, editor_target.y);
    while (touch_runtime_pointer(&queued)) { }
    assert(touch_runtime_editor_event((TouchPointer){0, editor_target.x + editor_target.w / 2, editor_target.y + editor_target.h / 2}) == 0);
    assert(touch_runtime_snapshot().selected == TOUCH_B);
    assert(touch_runtime_editor_event((TouchPointer){3, 0, 0}) == 0);
    touch_runtime_presentation((TouchPresentation){1, 1920, 1080, 1920, 1080, 1, 0});
    g_menu_back = 1; Sleep(100); g_menu_back = 0; Sleep(100);
    assert(!touch_runtime_snapshot().editing && g_page == PAGE_TOUCH && g_open);
    redraw_menu();
    finger_tap(g_rect[0] + g_close_r[0] + g_close_r[2] / 2, g_rect[1] + g_close_r[1] + g_close_r[3] / 2);
    assert(g_page == PAGE_SETTINGS && g_open);
    redraw_menu();
    finger_tap(g_rect[0] + g_close_r[0] + g_close_r[2] / 2, g_rect[1] + g_close_r[1] + g_close_r[3] / 2);
    Sleep(250);
    assert(!g_open && !g_guest_frozen && !touch_runtime_snapshot().pad.buttons);
    /* A changed presentation invalidates queued gesture fragments rather than joining taps across epochs. */
    touch_runtime_menu(1);
    finger(0, 200, 200);
    assert(touch_runtime_pointer(&queued) && queued.type == 0);
    touch_runtime_presentation((TouchPresentation){1, 1920, 1080, 1920, 1080, 1, 0});
    finger(2, 200, 200);
    assert(!touch_runtime_pointer(&queued));
    touch_runtime_menu(0);
    TouchRect hit_a = touch.geometry.control[TOUCH_A];
    finger(1, hit_a.x + hit_a.w / 2, hit_a.y + hit_a.h / 2);
    assert(!touch_runtime_snapshot().pad.buttons);
    finger(0, hit_a.x + hit_a.w / 2, hit_a.y + hit_a.h / 2);
    assert(touch_runtime_snapshot().pad.buttons & 1);
    finger(2, hit_a.x + hit_a.w / 2, hit_a.y + hit_a.h / 2);
#endif
    pcmenu_open_controls(0);
    Sleep(100);
    assert(g_open && g_page == PAGE_CONTROLS && g_direct);
    g_menu_back = 1; Sleep(150); g_menu_back = 0; Sleep(250);
    assert(!g_open && !g_guest_frozen);
    printf("pcmenu self-test: all checks passed\n");
    return 0;
}
