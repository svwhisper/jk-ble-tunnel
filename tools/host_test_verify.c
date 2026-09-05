/* Actual supervisor boot-verification logic with the production state cache.
 * No device, network, serial, command or settings I/O; submissions are captured. */
#include <assert.h>
#include "../node_a/main/supervisor.c"

EventGroupHandle_t g_evt;
static bool enabled = true;
static EventBits_t event_bits = EVT_MQTT_UP;
static unsigned polls, releases, publications;
static char published[192];
bool ble_owner_ble_enabled(void) { return enabled; }
EventBits_t xEventGroupGetBits(EventGroupHandle_t group)
{ (void)group; return event_bits; }
void arbiter_poll(uint8_t id, uint8_t opcode)
{ assert(id < CFG_NUM_UNITS && opcode == JK_CMD_DEVICE_INFO); polls++; }
void arbiter_submit(const bms_request_t *r)
{ assert(r->kind == TXN_DISCONNECT && r->source == SRC_INTERNAL); releases++; }
void mqtt_publish_verify(const char *json)
{ publications++; strlcpy(published, json, sizeof(published)); }
void tunnel_send_link(uint8_t id, tunnel_link_state_t link)
{ assert(id < CFG_NUM_UNITS && link == LINK_REACHABLE_IDLE); }

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
    verify_tick(65000001);
    assert(s_vfy_bank == 1 && !strcmp(s_vfy_result[0], "no_frames"));
    assert(releases == 1);
    /* A frame seen for the NEXT bank before its turn is not fresh evidence. */
    state_note_frame(1, 64000000);
    state_set_link_state(1, LINK_UP, true, 66000000);
    verify_tick(66000000);
    assert(s_vfy_bank == 1);
    state_note_frame(1, 67000000);
    verify_tick(67000000);
    assert(s_vfy_bank == 2 && !strcmp(s_vfy_result[1], "ok"));
    assert(releases == 2);
    /* Other-bank activity does not verify a never-connected bank. */
    state_note_frame(1, 80000000);
    verify_tick(112000001);
    assert(s_vfy_bank == 3 && !strcmp(s_vfy_result[2], "no_connect"));
    assert(releases == 2);
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
    puts("PASS: actual boot verification gates, stale/equal/cross-bank evidence, silent/disconnected deadlines, once-per-boot reporting");
    return 0;
}
