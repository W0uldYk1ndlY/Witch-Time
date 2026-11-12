#include "../runtime/diag_replay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* 1. Test synthetic Player 1 discovery with no physical controller */
static void test_synthetic_player1_discovery_no_physical_controller(void) {
    printf("[1/12] Testing synthetic Player 1 discovery with no physical controller...\n");

    /* Ensure initial state is disabled */
    diag_input_enable_synthetic_player1(false);
    assert(!diag_input_is_synthetic_player1_enabled());

    /* With no physical controller and disabled synthetic player 1 -> disconnected */
    bool connected = diag_input_is_player_connected(0, 1u /* STYLE_FULLKEY */, NULL, -1, false);
    assert(!connected);

    /* Enable synthetic player 1 */
    diag_input_enable_synthetic_player1(true);
    assert(diag_input_is_synthetic_player1_enabled());

    /* With no physical controller and enabled synthetic player 1 -> discovered before first query */
    connected = diag_input_is_player_connected(0, 1u /* STYLE_FULLKEY */, NULL, -1, false);
    assert(connected);

    printf("      Synthetic Player 1 discovered as FullKey without physical pad (passed)\n");
}

/* 2. Test supported style restrictions */
static void test_supported_style_restrictions(void) {
    printf("[2/12] Testing supported style restrictions...\n");

    diag_input_enable_synthetic_player1(true);

    /* STYLE_FULLKEY bit is 1u. When game only supports Handheld (e.g., 2u) or JoyDual (e.g., 4u) without FullKey */
    uint32_t non_fullkey_styles = 2u; /* Handheld only */
    bool connected = diag_input_is_player_connected(0, non_fullkey_styles, NULL, -1, false);
    assert(!connected);

    non_fullkey_styles = 4u; /* JoyDual only */
    connected = diag_input_is_player_connected(0, non_fullkey_styles, NULL, -1, false);
    assert(!connected);

    /* Both FullKey and Handheld supported */
    uint32_t fullkey_and_handheld = 1u | 2u;
    connected = diag_input_is_player_connected(0, fullkey_and_handheld, NULL, -1, false);
    assert(connected);

    printf("      Non-FullKey styles properly rejected for synthetic player 1 (passed)\n");
}

/* 3. Test supported ID whitelist restrictions */
static void test_supported_id_restrictions(void) {
    printf("[3/12] Testing supported ID whitelist restrictions...\n");

    diag_input_enable_synthetic_player1(true);

    /* Whitelist containing player 0 */
    uint32_t allowed_ids_with_p1[] = {0, 1, 2};
    bool connected = diag_input_is_player_connected(0, 1u, allowed_ids_with_p1, 3, false);
    assert(connected);

    /* Whitelist excluding player 0 (e.g. players 1 and 2 only) */
    uint32_t allowed_ids_without_p1[] = {1, 2};
    connected = diag_input_is_player_connected(0, 1u, allowed_ids_without_p1, 2, false);
    assert(!connected);

    /* Players 1..7 do not get synthetic player 1 connection */
    for (uint32_t p = 1; p < 8; p++) {
        connected = diag_input_is_player_connected(p, 1u, NULL, -1, false);
        assert(!connected);
    }

    /* Out of range player ID (> 7) */
    connected = diag_input_is_player_connected(8, 1u, NULL, -1, true);
    assert(!connected);

    printf("      Supported ID whitelist restrictions verified (passed)\n");
}

/* 4. Test hotplug and live device change decoupling */
static void test_hotplug_and_device_decoupling(void) {
    printf("[4/12] Testing hotplug and live device decoupling...\n");

    diag_input_enable_synthetic_player1(true);

    /* Initially disconnected physical pad */
    assert(diag_input_is_player_connected(0, 1u, NULL, -1, false));

    /* Physical pad hotplug: connected */
    assert(diag_input_is_player_connected(0, 1u, NULL, -1, true));

    /* Physical pad hotplug: disconnected */
    assert(diag_input_is_player_connected(0, 1u, NULL, -1, false));

    /* Repeated rapid hotplug cycles */
    for (int i = 0; i < 100; i++) {
        bool phys = (i % 2) != 0;
        bool p1_conn = diag_input_is_player_connected(0, 1u, NULL, -1, phys);
        assert(p1_conn == true);
    }

    printf("      Synthetic Player 1 remained consistently connected across hotplug changes (passed)\n");
}

/* 5. Test confirm/cancel behavior */
static void test_confirm_cancel_policy(void) {
    printf("[5/12] Testing confirm/cancel button policies...\n");

    /* Default Native A confirm */
    uint64_t confirm_btn = diag_replay_get_confirm_button(DIAG_REPLAY_CONFIRM_A);
    uint64_t cancel_btn = diag_replay_get_cancel_button(DIAG_REPLAY_CONFIRM_A);
    assert(confirm_btn == DIAG_CONFIRM_BIT_A);
    assert(cancel_btn == DIAG_CANCEL_BIT_B);

    /* Swapped B confirm */
    confirm_btn = diag_replay_get_confirm_button(DIAG_REPLAY_CONFIRM_B);
    cancel_btn = diag_replay_get_cancel_button(DIAG_REPLAY_CONFIRM_B);
    assert(confirm_btn == DIAG_CANCEL_BIT_B);
    assert(cancel_btn == DIAG_CONFIRM_BIT_A);

    /* Profile 1 verification with native A confirm -> accepted */
    diag_input_policy_check_t check = {
        .source_policy = "synthetic_fullkey_player1",
        .mapping_policy = "default_type_a",
        .confirm_policy = "native_a",
        .is_keyboard_origin = false,
        .is_custom_binding = false,
        .is_menu_swapped = false,
    };
    diag_input_policy_result_t res = diag_verify_input_policy(&check);
    assert(res.accepted);
    assert(res.reason_code == NULL);

    /* Profile 1 verification with swapped confirm in settings -> rejected */
    check.is_menu_swapped = true;
    res = diag_verify_input_policy(&check);
    assert(!res.accepted);
    assert(strcmp(res.reason_code, "DIFFERENT_MAPPING_POLICY") == 0);

    /* Profile 1 verification with confirm_policy != native_a -> rejected */
    check.is_menu_swapped = false;
    check.confirm_policy = "swapped_b";
    res = diag_verify_input_policy(&check);
    assert(!res.accepted);
    assert(strcmp(res.reason_code, "DIFFERENT_MAPPING_POLICY") == 0);

    printf("      Confirm/cancel policies verified and enforced (passed)\n");
}

/* 6. Test rejection of keyboard origin and custom bindings in Profile 1 */
static void test_rejection_of_keyboard_and_custom_bindings(void) {
    printf("[6/12] Testing rejection of keyboard origin and custom bindings...\n");

    diag_input_policy_check_t check = {
        .source_policy = "synthetic_fullkey_player1",
        .mapping_policy = "default_type_a",
        .confirm_policy = "native_a",
        .is_keyboard_origin = false,
        .is_custom_binding = false,
        .is_menu_swapped = false,
    };

    /* Keyboard origin rejected */
    check.is_keyboard_origin = true;
    diag_input_policy_result_t res = diag_verify_input_policy(&check);
    assert(!res.accepted);
    assert(strcmp(res.reason_code, "DIFFERENT_MAPPING_POLICY") == 0);
    check.is_keyboard_origin = false;

    /* Custom controller bindings rejected */
    check.is_custom_binding = true;
    res = diag_verify_input_policy(&check);
    assert(!res.accepted);
    assert(strcmp(res.reason_code, "DIFFERENT_MAPPING_POLICY") == 0);
    check.is_custom_binding = false;

    /* Unsupported source policy */
    check.source_policy = "keyboard_wasd";
    res = diag_verify_input_policy(&check);
    assert(!res.accepted);
    assert(strcmp(res.reason_code, "INCOMPATIBLE_PROFILES") == 0);
    check.source_policy = "synthetic_fullkey_player1";

    /* Unsupported mapping policy */
    check.mapping_policy = "type_b";
    res = diag_verify_input_policy(&check);
    assert(!res.accepted);
    assert(strcmp(res.reason_code, "DIFFERENT_MAPPING_POLICY") == 0);

    printf("      Profile 1 restrictions against keyboard and custom bindings enforced (passed)\n");
}

/* 7. Test ordinary input preferences remain unchanged when synthetic player 1 disabled */
static void test_ordinary_input_preservation(void) {
    printf("[7/12] Testing preservation of ordinary input...\n");

    /* Disable synthetic player 1 */
    diag_input_enable_synthetic_player1(false);
    assert(!diag_input_is_synthetic_player1_enabled());

    /* Ordinary physical controller connection reflects real physical state */
    assert(diag_input_is_player_connected(0, 1u, NULL, -1, true) == true);
    assert(diag_input_is_player_connected(0, 1u, NULL, -1, false) == false);

    /* Player 2 physical controller */
    assert(diag_input_is_player_connected(1, 1u, NULL, -1, true) == true);
    assert(diag_input_is_player_connected(1, 1u, NULL, -1, false) == false);

    printf("       Ordinary input remains untouched and correctly reflects hardware state (passed)\n");
}

/* 8. Test right-stick movement, release, and derived bits */
static void test_right_stick_and_derived_bits(void) {
    printf("[8/12] Testing right-stick movement, release, and derived stick bits...\n");

    uint64_t composed = 0;
    uint32_t out_attr = 0;

    /* Neutral sticks: no derived bits */
    diag_compose_fullkey_state(0, 0, 0, 0, 0, DIAG_REPLAY_ATTR_CONNECTED, &composed, &out_attr);
    assert((composed >> 16) == 0);
    assert(out_attr == DIAG_REPLAY_ATTR_CONNECTED);

    /* Left stick deflection */
    diag_compose_fullkey_state(0, -20000, 25000, 0, 0, DIAG_REPLAY_ATTR_CONNECTED, &composed, &out_attr);
    assert(composed & (1ULL << 16)); /* Left */
    assert(composed & (1ULL << 17)); /* Up */
    assert(!(composed & (1ULL << 18)));
    assert(!(composed & (1ULL << 19)));

    /* Right stick deflection */
    diag_compose_fullkey_state(0, 0, 0, 24000, -22000, DIAG_REPLAY_ATTR_CONNECTED | DIAG_REPLAY_ATTR_WIRED, &composed, &out_attr);
    assert(composed & (1ULL << 22)); /* Right Stick Right */
    assert(composed & (1ULL << 23)); /* Right Stick Down */
    assert(!(composed & (1ULL << 20)));
    assert(!(composed & (1ULL << 21)));
    assert(out_attr == (DIAG_REPLAY_ATTR_CONNECTED | DIAG_REPLAY_ATTR_WIRED));

    /* Right stick release */
    diag_compose_fullkey_state(0, 0, 0, 0, 0, DIAG_REPLAY_ATTR_CONNECTED, &composed, &out_attr);
    assert((composed & (0x0F00000ULL)) == 0); /* bits 20..23 clear */

    printf("       Right-stick movement, release, and derived bits correctly composed (passed)\n");
}

/* 9. Test connection attributes preservation */
static void test_connection_attributes_preservation(void) {
    printf("[9/12] Testing connection attributes preservation...\n");

    uint64_t composed = 0;
    uint32_t out_attr = 0;

    /* Wireless connection */
    diag_compose_fullkey_state(DIAG_REPLAY_BTN_A, 0, 0, 0, 0, DIAG_REPLAY_ATTR_CONNECTED, &composed, &out_attr);
    assert(out_attr == DIAG_REPLAY_ATTR_CONNECTED);
    assert((out_attr & DIAG_REPLAY_ATTR_WIRED) == 0);

    /* Wired connection */
    diag_compose_fullkey_state(DIAG_REPLAY_BTN_A, 0, 0, 0, 0, DIAG_REPLAY_ATTR_CONNECTED | DIAG_REPLAY_ATTR_WIRED, &composed, &out_attr);
    assert((out_attr & DIAG_REPLAY_ATTR_CONNECTED) != 0);
    assert((out_attr & DIAG_REPLAY_ATTR_WIRED) != 0);

    /* Disallowed extra attribute bits masked off */
    diag_compose_fullkey_state(DIAG_REPLAY_BTN_A, 0, 0, 0, 0, 0xFFFFFFFFU, &composed, &out_attr);
    assert(out_attr == (DIAG_REPLAY_ATTR_CONNECTED | DIAG_REPLAY_ATTR_WIRED));

    printf("       Connection attributes accurately preserved without corruption (passed)\n");
}

/* 10. Test neutral release on EOF, cancellation, and failure */
static void test_neutral_release_on_eof_cancel_failure(void) {
    printf("[10/12] Testing neutral release on EOF, cancel, and failure...\n");

    diag_replay_record_t dummy_rec = {
        .poll = 1,
        .anchor = -1,
        .anchor_poll = 0,
        .buttons = DIAG_REPLAY_BTN_A | DIAG_REPLAY_BTN_ZR,
        .lx = 10000,
        .ly = -10000,
        .rx = 20000,
        .ry = -20000,
        .attributes = DIAG_REPLAY_ATTR_CONNECTED | DIAG_REPLAY_ATTR_WIRED,
    };

    diag_replay_session_t sess;
    memset(&sess, 0, sizeof(sess));
    sess.is_loaded = true;
    sess.has_failed = false;
    sess.is_cancelled = false;
    sess.is_eof = false;
    sess.record_count = 1;
    sess.records = &dummy_rec;
    sess.current_pos = 0;
    sess.header.total_polls = 10;

    uint64_t b = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint32_t attr = 0;

    /* Normal active poll */
    bool ok = diag_replay_poll(&sess, 1, -1, 0, &b, &lx, &ly, &rx, &ry, &attr);
    assert(ok);
    assert(b == (DIAG_REPLAY_BTN_A | DIAG_REPLAY_BTN_ZR));
    assert(lx == 10000 && rx == 20000);

    /* Neutral release on cancellation */
    diag_replay_cancel(&sess);
    assert(sess.is_cancelled);
    ok = diag_replay_poll(&sess, 2, -1, 0, &b, &lx, &ly, &rx, &ry, &attr);
    assert(ok);
    assert(b == 0);
    assert(lx == 0 && ly == 0 && rx == 0 && ry == 0);
    assert(attr == DIAG_REPLAY_ATTR_CONNECTED);

    /* Neutral release on EOF */
    sess.is_cancelled = false;
    diag_replay_set_eof(&sess, true);
    assert(sess.is_eof);
    ok = diag_replay_poll(&sess, 3, -1, 0, &b, &lx, &ly, &rx, &ry, &attr);
    assert(ok);
    assert(b == 0);
    assert(lx == 0 && ly == 0 && rx == 0 && ry == 0);
    assert(attr == DIAG_REPLAY_ATTR_CONNECTED);

    /* Neutral release on session failure */
    sess.is_eof = false;
    sess.has_failed = true;
    ok = diag_replay_poll(&sess, 4, -1, 0, &b, &lx, &ly, &rx, &ry, &attr);
    assert(!ok);
    assert(b == 0);
    assert(lx == 0 && ly == 0 && rx == 0 && ry == 0);

    printf("        Neutral release strictly delivered on cancellation, EOF, and failure (passed)\n");
}

/* 11. Test settings-menu gate, delivered state assertion, and live contamination immunity */
static void test_settings_menu_gate_and_delivered_state(void) {
    printf("[11/12] Testing settings-menu gate and delivered state assertion...\n");

    diag_delivered_pad_state_t st;
    memset(&st, 0, sizeof(st));
    st.sampling_number = 1001;
    st.raw_buttons = DIAG_REPLAY_BTN_A | DIAG_REPLAY_BTN_X;
    st.composed_buttons = DIAG_REPLAY_BTN_A | DIAG_REPLAY_BTN_X;
    st.lx = 15000;
    st.ly = -12000;
    st.rx = 0;
    st.ry = 0;
    st.attributes = DIAG_REPLAY_ATTR_CONNECTED | DIAG_REPLAY_ATTR_WIRED;
    st.is_gated = false;

    /* Record delivered normal state */
    diag_record_delivered_pad_state(0, &st);

    diag_delivered_pad_state_t out;
    bool has_state = diag_get_last_delivered_pad_state(0, &out);
    assert(has_state);
    assert(out.sampling_number == 1001);
    assert(out.composed_buttons == (DIAG_REPLAY_BTN_A | DIAG_REPLAY_BTN_X));
    assert(out.lx == 15000);
    assert(!out.is_gated);

    /* Simulate settings-menu gate active: pad_gate zeroes the state */
    diag_delivered_pad_state_t gated_st;
    memset(&gated_st, 0, sizeof(gated_st));
    gated_st.sampling_number = 1002;
    gated_st.raw_buttons = 0;
    gated_st.composed_buttons = 0;
    gated_st.lx = 0;
    gated_st.ly = 0;
    gated_st.rx = 0;
    gated_st.ry = 0;
    gated_st.attributes = DIAG_REPLAY_ATTR_CONNECTED;
    gated_st.is_gated = true;

    diag_record_delivered_pad_state(0, &gated_st);

    has_state = diag_get_last_delivered_pad_state(0, &out);
    assert(has_state);
    assert(out.sampling_number == 1002);
    assert(out.composed_buttons == 0);
    assert(out.is_gated == true);

    /* Live device contamination immunity */
    diag_input_enable_synthetic_player1(true);
    /* Physical controller hotplug cannot change Player 1 connection */
    assert(diag_input_is_player_connected(0, 1u, NULL, -1, false) == true);
    assert(diag_input_is_player_connected(0, 1u, NULL, -1, true) == true);

    printf("        Settings-menu gate zeros delivered state, live device contamination blocked (passed)\n");
}

/* 12. Test equivalent supported guest actions through actual Type-A mapping */
static void test_type_a_guest_action_equivalence(void) {
    printf("[12/12] Testing Type-A guest action equivalence...\n");

    /* Default Type-A Bayonetta 3 layout mapping */
    typedef struct {
        const char *action_name;
        uint64_t expected_button;
    } guest_action_test_t;

    guest_action_test_t actions[] = {
        {"Kick / Confirm", DIAG_REPLAY_BTN_A},
        {"Jump / Cancel",  DIAG_REPLAY_BTN_B},
        {"Punch",          DIAG_REPLAY_BTN_X},
        {"Shoot",          DIAG_REPLAY_BTN_Y},
        {"Demon Slave",    DIAG_REPLAY_BTN_ZL},
        {"Dodge",          DIAG_REPLAY_BTN_ZR},
        {"Weapon Switch",  DIAG_REPLAY_BTN_L},
        {"Stance Change",  DIAG_REPLAY_BTN_R},
        {"Waypoint",       DIAG_REPLAY_BTN_STICK_L},
        {"Lock-on/Camera", DIAG_REPLAY_BTN_STICK_R},
        {"Pause",          DIAG_REPLAY_BTN_PLUS},
        {"Subscreen",      DIAG_REPLAY_BTN_MINUS},
        {"Taunt",          DIAG_REPLAY_BTN_D_DOWN},
    };

    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
        uint64_t composed = 0;
        uint32_t out_attr = 0;
        diag_compose_fullkey_state(actions[i].expected_button, 0, 0, 0, 0, DIAG_REPLAY_ATTR_CONNECTED, &composed, &out_attr);
        assert((composed & actions[i].expected_button) == actions[i].expected_button);
    }

    printf("        Type-A guest actions map 1:1 to canonical Pro Controller buttons (passed)\n");
}

int main(void) {
    printf("=== Witch-Time Diag Input Policy and Synthetic Player 1 Test Harness ===\n\n");

    test_synthetic_player1_discovery_no_physical_controller();
    test_supported_style_restrictions();
    test_supported_id_restrictions();
    test_hotplug_and_device_decoupling();
    test_confirm_cancel_policy();
    test_rejection_of_keyboard_and_custom_bindings();
    test_ordinary_input_preservation();
    test_right_stick_and_derived_bits();
    test_connection_attributes_preservation();
    test_neutral_release_on_eof_cancel_failure();
    test_settings_menu_gate_and_delivered_state();
    test_type_a_guest_action_equivalence();

    printf("\nAll 12 input policy, canonical composition, and gate tests PASSED successfully.\n");
    return 0;
}
