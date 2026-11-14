/* Execute the production HID handlers with mocked physical pad and guest memory. */
#define diag_record_delivered_pad_state test_record_delivered_pad_state
#include "../runtime/nnhid.c"
#include <assert.h>
#include "../runtime/diag.h"

bool diag_is_enabled(void) { return false; }

static int physical, last_origin = -1;
static NnPadState physical_state;
static diag_delivered_pad_state_t delivered;
int nnpad_init(void) { return 1; }
int nnpad_keyboard_player(void) { return -1; }
int nnpad_present(int player) { return player == 0 && physical; }
int nnpad_get(int player, NnPadState *out) { if (!nnpad_present(player)) return 0; *out = physical_state; return 1; }
int nnpad_kb_vk(int idx) { (void)idx; return 0; }
int nnpad_mouse_cfg(float *s, int *i) { (void)s; (void)i; return 0; }
void nnpad_prompts_used(int origin) { last_origin = origin; }
void prompts_tick(void) { }
void menu_swap_tick(void) { }
void display_mouse_want(int on) { (void)on; }
void display_mouse_take(int *x, int *y) { *x = *y = 0; }
int display_mouse_hold(void) { return 0; }
int pcmenu_gate(uint32_t id, uint64_t buttons) { (void)id; (void)buttons; return 0; }
void nvn_front_shot_fight(int frame) { (void)frame; }
void nvn_front_shot_step(int step) { (void)step; }
void display_l1_poll(uint64_t b, long long ticks, long long poll) { (void)b; (void)ticks; (void)poll; }
void guest_trap(const char *what, uint64_t address) { (void)what; (void)address; abort(); }
void register_imports(const ImportDef *defs, int count) { (void)defs; (void)count; }
void nnpad_rumble(int player, float low, float high) { (void)player; (void)low; (void)high; }
void nnpad_disconnect(int player) { (void)player; }
int nnpad_selftest(const NnHidHooks *h) { (void)h; return 0; }
void report_and_exit(int code) { exit(code); }
int diag_writer_write_outcome(const char *o, const char *r, const char *c) { (void)o; (void)r; (void)c; return 1; }
void diag_record_delivered_pad_state(uint32_t id, const diag_delivered_pad_state_t *s) { if (id == 0) delivered = *s; }

static void state(uint32_t id, uint8_t out[0x28]) {
    Ctx c = {0}; c.x[0] = (uint64_t)(uintptr_t)out; c.x[1] = (uint64_t)(uintptr_t)&id;
    hid_GetNpadState_FullKey(&c);
}
int main(int argc, char **argv) {
    g_hid_ready = 1; g_sdl_pad = 1; g_keyboard_player = -1; g_pad_test = 1;
    touch_runtime_init(NULL); touch_runtime_focus(1, 1);
    touch_runtime_geometry((TouchRect){0, 0, 1920, 1080}, 1, 1920, 1080); touch_runtime_set_ready(1);
    TouchSnapshot snapshot = touch_runtime_snapshot();
    TouchRect a = snapshot.geometry.control[TOUCH_A], left = snapshot.geometry.control[TOUCH_LEFT_STICK];
    uint8_t output[0x28];
    if (argc > 1) {
        (void)argv;
        touch_runtime_set_replay(getenv("WT_PLAY") != NULL); /* Android window bootstrap */
        touch_runtime_event(0, 1, 1, a.x + a.w / 2, a.y + a.h / 2);
        touch_runtime_event(0, 1, 2, snapshot.settings.x + 1, snapshot.settings.y + 1);
        g_rec_mark = 0;
        state(0, output);
        assert(delivered.raw_buttons == 4 && delivered.lx == 123 && delivered.ly == 456);
        assert(!touch_runtime_take_settings() && !touch_runtime_snapshot().pad.buttons);
        puts("WT_PLAY excludes touch and Settings");
        return 0;
    }
    touch_runtime_event(0, 1, 1, a.x + a.w / 2, a.y + a.h / 2);
    state(0, output);
    assert((rd64((uint64_t)(uintptr_t)output + 8) & 1) && (rd32((uint64_t)(uintptr_t)output + 0x20) & 1));
    assert((delivered.raw_buttons & 1) && last_origin == 2 && player_connected(0) && !player_connected(1));
    physical = 1; physical_state.buttons = 2; physical_state.lx = 20000; physical_state.rx = 16000;
    touch_runtime_event(0, 1, 2, left.x + left.w / 2, left.y + left.h / 2);
    state(0, output);
    assert((delivered.raw_buttons & 3) == 3 && delivered.lx == 0 && delivered.rx == 16000);
    state(0, output); assert(last_origin == 0);
    touch_runtime_event(1, 1, 1, a.x + a.w / 2, a.y + a.h / 2);
    state(0, output); assert(last_origin == 2);
    touch_runtime_event(2, 1, 2, 0, 0); state(0, output);
    assert(delivered.lx == 20000 && delivered.rx == 16000);
    touch_runtime_event(0, 1, 7, snapshot.settings.x + 1, snapshot.settings.y + 1);
    diag_replay_record_t replay_record = {.poll = 0, .anchor = -1, .buttons = DIAG_REPLAY_BTN_X,
                                           .lx = 1234, .rx = -4321, .attributes = DIAG_REPLAY_ATTR_CONNECTED};
    diag_replay_session_t replay = {0};
    replay.is_loaded = true; replay.records = &replay_record; replay.record_count = 1; replay.header.total_polls = 2;
    diag_input_set_active_replay(&replay);
    state(0, output);
    assert(delivered.raw_buttons == DIAG_REPLAY_BTN_X && delivered.lx == 1234 && delivered.rx == -4321);
    assert(!touch_runtime_take_settings());
    diag_input_set_active_replay(NULL); diag_input_enable_synthetic_player1(false);
    touch_runtime_event(1, 1, 1, a.x + a.w / 2, a.y + a.h / 2);
    assert(!touch_runtime_snapshot().pad.buttons && !touch_runtime_take_settings());
    g_supported_styles = 0; assert(!player_connected(0));
    g_supported_styles = 1; g_supported_id_count = 1; g_supported_ids[0] = 1; assert(!player_connected(0));
    puts("touch HID production-handler self-test passed");
    return 0;
}
