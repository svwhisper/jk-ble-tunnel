/*
 * adv_mgr.c — ext-adv set management. NIMBLE-PASS: verify the ext-adv param
 * struct fields and start/stop signatures against the pinned IDF NimBLE.
 */
#include <string.h>
#include "adv_mgr.h"
#include "config.h"
#include "tunnel_cli.h"
#include "esp_log.h"
#include "esp_random.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "adv_mgr";

typedef struct {
    uint8_t addr[6];
    char    name[32];
    bool    configured;
    bool    advertising;
    bool    connected;
    tunnel_link_state_t link;
} adv_set_t;

static adv_set_t s_set[CFG_NUM_UNITS];

const uint8_t *adv_mgr_addr(uint8_t id)
{ return (id < CFG_NUM_UNITS) ? s_set[id].addr : NULL; }

int adv_mgr_identity_for_addr(const uint8_t *addr)
{
    for (int i = 0; i < CFG_NUM_UNITS; i++)
        if (memcmp(s_set[i].addr, addr, 6) == 0) return i;
    return -1;
}

/* Build legacy connectable adv data: flags + complete name + 16-bit svc UUID. */
static void set_adv_data(uint8_t id)
{
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)s_set[id].name;
    f.name_len = strlen(s_set[id].name);
    f.name_is_complete = 1;
    ble_uuid16_t u = BLE_UUID16_INIT(JK_SVC_UUID);
    f.uuids16 = &u; f.num_uuids16 = 1; f.uuids16_is_complete = 1;
    /* NIMBLE-PASS: for ext adv use ble_gap_ext_adv_set_data with an mbuf built
     * from these fields (ble_hs_adv_set_fields into a buffer). */
    ESP_LOGD(TAG, "adv data set[%u] '%s'", id, s_set[id].name);
}

static void configure(uint8_t id)
{
    struct ble_gap_ext_adv_params p = {0};
    p.connectable = 1;
    p.legacy_pdu = 1;                 /* spec §5: legacy connectable per set   */
    p.scannable = 1;
    p.own_addr_type = BLE_OWN_ADDR_RANDOM;
    p.primary_phy = BLE_HCI_LE_PHY_1M;
    p.secondary_phy = BLE_HCI_LE_PHY_1M;
    p.sid = id;
    p.itvl_min = 0x00A0; p.itvl_max = 0x00F0;   /* ~100-150 ms */
    /* NIMBLE-PASS: ble_gap_ext_adv_configure(id, &p, NULL, gap_event_cb, NULL); */
    ble_gap_ext_adv_set_addr(id, s_set[id].addr);   /* NIMBLE-PASS */
    set_adv_data(id);
    s_set[id].configured = true;
}

static bool should_advertise(uint8_t id)
{
    if (s_set[id].connected) return false;              /* connected set is silent */
    if (!tunnel_cli_up()) return false;                 /* no tunnel => lie (spec §11) */
    if (s_set[id].link == LINK_UNREACHABLE && !CFG_ADVERTISE_WHEN_DOWN) return false;
    return s_set[id].name[0] != '\0';
}

static void apply(uint8_t id)
{
    bool want = should_advertise(id);
    if (want && !s_set[id].advertising) {
        if (!s_set[id].configured) configure(id);
        /* NIMBLE-PASS: ble_gap_ext_adv_start(id, 0, 0); */
        s_set[id].advertising = true;
        ESP_LOGI(TAG, "adv start set[%u] '%s'", id, s_set[id].name);
    } else if (!want && s_set[id].advertising) {
        /* NIMBLE-PASS: ble_gap_ext_adv_stop(id); */
        s_set[id].advertising = false;
        ESP_LOGI(TAG, "adv stop set[%u]", id);
    }
}

void adv_mgr_set_name(uint8_t id, const char *name)
{
    if (id >= CFG_NUM_UNITS) return;
    strlcpy(s_set[id].name, name, sizeof(s_set[id].name));
    if (s_set[id].configured) set_adv_data(id);
    apply(id);
}
void adv_mgr_on_link(uint8_t id, tunnel_link_state_t st)
{ if (id < CFG_NUM_UNITS) { s_set[id].link = st; apply(id); } }
void adv_mgr_on_connect(uint8_t id)
{ if (id < CFG_NUM_UNITS) { s_set[id].connected = true; apply(id); } }
void adv_mgr_on_disconnect(uint8_t id)
{ if (id < CFG_NUM_UNITS) { s_set[id].connected = false; apply(id); } }

void adv_mgr_pause_all(void)
{
    for (int i = 0; i < CFG_NUM_UNITS; i++)
        if (s_set[i].advertising) { /* NIMBLE-PASS ble_gap_ext_adv_stop(i); */
            s_set[i].advertising = false; }
    ESP_LOGW(TAG, "all advertising paused");
}

void adv_mgr_init(void)
{
    memset(s_set, 0, sizeof(s_set));
    for (int i = 0; i < CFG_NUM_UNITS; i++) {
        /* Deterministic static-random address per set (top two bits = 11). */
        esp_fill_random(s_set[i].addr, 6);
        s_set[i].addr[5] |= 0xC0;
        s_set[i].link = LINK_UNREACHABLE;
    }
}
