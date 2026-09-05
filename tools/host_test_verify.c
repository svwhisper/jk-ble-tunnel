/* Actual supervisor boot-verification logic with the production state cache.
 * No device, network, serial, command or settings I/O; submissions are captured. */
#include <assert.h>
#include "../node_a/main/supervisor.c"

EventGroupHandle_t g_evt;
static bool enabled = true;
static EventBits_t event_bits = EVT_MQTT_UP;
static unsigned polls, releases, publications;
static char published[192];
static int64_t test_now;
static uint8_t last_release_id;
int64_t esp_timer_get_time(void) { return test_now; }
int64_t net_wifi_down_ms(void) { return 0; }
bool net_wifi_up(void) { return true; }
int esp_wifi_disconnect(void) { assert(0); return 0; }
void decoder_session_reset(uint8_t id) { assert(id < CFG_NUM_UNITS); }
void decoder_send_opener(uint8_t id) { assert(id < CFG_NUM_UNITS); }
void arbiter_clear_pending(uint8_t id) { assert(id < CFG_NUM_UNITS); }
void mqtt_publish_link(uint8_t id) { assert(id < CFG_NUM_UNITS); }
bool measure_active(uint8_t *id) { (void)id; return false; }
void measure_force_restore(uint8_t id) { (void)id; assert(0); }
bool ble_owner_ble_enabled(void) { return enabled; }
EventBits_t xEventGroupGetBits(EventGroupHandle_t group)
{ (void)group; return event_bits; }
void arbiter_poll(uint8_t id, uint8_t opcode)
{ assert(id < CFG_NUM_UNITS && (opcode == JK_CMD_DEVICE_INFO || opcode == JK_CMD_CELL_INFO)); polls++; }
void arbiter_submit(const bms_request_t *r)
{ assert(r->kind == TXN_DISCONNECT && r->source == SRC_INTERNAL);
  releases++; last_release_id = r->bms_id; }
void mqtt_publish_verify(const char *json)
{ publications++; strlcpy(published, json, sizeof(published)); }
void tunnel_send_link(uint8_t id, tunnel_link_state_t link)
{ assert(id < CFG_NUM_UNITS && link <= LINK_UP); }

static void maintain_at(int64_t now)
{ test_now = now; maintenance_tick(); }

static void test_release_policy(void)
{
    /* Actual maintenance idle gate is still the only release path. */
    state_cache_init();
    memset(s_last_link, 0xff, sizeof(s_last_link));
    memset(s_linkup_us, 0, sizeof(s_linkup_us));
    memset(s_hold_until_us, 0, sizeof(s_hold_until_us));
    memset(s_hold_s, 0, sizeof(s_hold_s));
    s_vfy_done = false; s_vfy_bank = 0; s_vfy_start_us = 20000000;
    memset(s_vfy_result, 0, sizeof(s_vfy_result));
    releases = 0;
    state_set_link_state(0, LINK_UP, true, 21000000);
    maintain_at(21000000); /* records link-up and preserves bootstrap */
    state_set_app_connected(0, true, 22000000);
    state_note_frame(0, 23000000);
    verify_tick(23000000); /* success with an app already attached */
    assert(s_vfy_bank == 1 && !verify_wants(0));
    assert(!strcmp(s_vfy_result[0], "ok") && releases == 0);
    s_vfy_done = true; /* isolate normal idle behavior from other banks */
    maintain_at(121000000); /* past link-up grace: app still owns the link */
    assert(releases == 0);
    state_set_app_connected(0, false, 122000000);
    maintain_at(182000000); /* exactly 60 s: preserve existing strict boundary */
    assert(releases == 0);
    maintain_at(182000001);
    assert(releases == 1 && last_release_id == 0);
    state_set_link_state(0, LINK_REACHABLE_IDLE, false, 0);
    maintain_at(183000000); /* simulated GAP completion: no held-link leak */
    assert(releases == 1);

    /* No app during verification: relinquish demand, allow a later attach
     * to keep the link, then honor its own departure/grace. */
    s_vfy_done = false; s_vfy_bank = 1; s_vfy_start_us = 200000000;
    state_set_link_state(1, LINK_UP, true, 201000000);
    maintain_at(201000000);
    state_note_frame(1, 202000000);
    verify_tick(202000000);
    assert(s_vfy_bank == 2 && releases == 1 && !verify_wants(1));
    s_vfy_done = true;
    maintain_at(250000000);
    assert(releases == 1); /* no premature teardown */
    state_set_app_connected(1, true, 251000000);
    maintain_at(270000000);
    assert(releases == 1);
    state_set_app_connected(1, false, 271000000);
    maintain_at(331000001);
    assert(releases == 2 && last_release_id == 1);
    state_set_link_state(1, LINK_REACHABLE_IDLE, false, 0);

    /* A verified link with no phone at all still becomes idle after grace. */
    state_set_link_state(2, LINK_UP, true, 400000000);
    maintain_at(400000000);
    maintain_at(460000000);
    assert(releases == 2);
    maintain_at(460000001);
    assert(releases == 3 && last_release_id == 2);
}

int main(void)
{
    state_cache_init();
    /* Existing boot/MQTT/BLE gates remain unchanged. */
    verify_tick(19999999);
    assert(s_vfy_bank == -1 && polls == 0);
    event_bits = 0;
    verify_tick(20000000);
    assert(s_vfy_bank == -1);
    event_bits = EVT_MQTT_UP; enabled = false;
    verify_tick(20000000);
    assert(s_vfy_bank == -1);
    enabled = true;
    /* Regression: successful connection with NO frame must not pass verify. */
    verify_tick(20000000);
    assert(s_vfy_bank == 0 && !s_vfy_result[0][0]);
    state_set_link_state(0, LINK_UP, true, 21000000);
    verify_tick(22000000);
    assert(s_vfy_bank == 0 && !s_vfy_result[0][0] && releases == 0);
    /* Decoder cache writes and stale/equal frame times cannot pass. */
    jk_cell_info_t ci = {0}; jk_settings_t settings = {0};
    state_set_cells(0, &ci); state_set_settings(0, &settings);
    state_note_frame(0, 19999999);
    verify_tick(23000000);
    assert(s_vfy_bank == 0);
    state_note_frame(0, 20000000);
    verify_tick(24000000);
    assert(s_vfy_bank == 0);
    /* Timeout boundary unchanged: held-but-silent is no_frames, not ok. */
    verify_tick(65000000);
    assert(s_vfy_bank == 0);
    state_set_app_connected(0, true, 65000000);
    verify_tick(65000001);
    assert(s_vfy_bank == 1 && !strcmp(s_vfy_result[0], "no_frames"));
    /* Regression: completing verification must not disconnect this phone. */
    assert(releases == 0);
    /* A frame seen for the NEXT bank before its turn is not fresh evidence. */
    state_note_frame(1, 64000000);
    state_set_link_state(1, LINK_UP, true, 66000000);
    verify_tick(66000000);
    assert(s_vfy_bank == 1);
    state_note_frame(1, 67000000);
    verify_tick(67000000);
    assert(s_vfy_bank == 2 && !strcmp(s_vfy_result[1], "ok"));
    assert(releases == 0);
    /* Other-bank activity does not verify a never-connected bank. */
    state_note_frame(1, 80000000);
    verify_tick(112000001);
    assert(s_vfy_bank == 3 && !strcmp(s_vfy_result[2], "no_connect"));
    assert(releases == 0);
    state_note_frame(3, 113000000);
    verify_tick(114000000);
    assert(s_vfy_bank == CFG_NUM_UNITS && !strcmp(s_vfy_result[3], "ok"));
    verify_tick(115000000);
    assert(s_vfy_done && publications == 1);
    assert(strstr(published, "\"0\":\"no_frames\""));
    assert(strstr(published, "\"1\":\"ok\""));
    assert(strstr(published, "\"2\":\"no_connect\""));
    assert(strstr(published, "\"3\":\"ok\""));
    unsigned previous_polls = polls;
    verify_tick(999000000);
    assert(publications == 1 && polls == previous_polls);
    test_release_policy();
    puts("PASS: verification success/timeout never queue teardown; active/late phone attach and ordinary 60 s idle release");
    puts("PASS: actual boot verification gates, stale/equal/cross-bank evidence, silent/disconnected deadlines, once-per-boot reporting");
    return 0;
}
