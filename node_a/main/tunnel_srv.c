/*
 * tunnel_srv.c — LAN TCP server, tunnel wire format (spec §6).
 *
 * One task owns the socket. Inbound frames are handled inline; outbound frames
 * come from two sources drained each loop: g_q_notify (NOTIFY to app) and the
 * local out-queue (LINK/WRITE_RESULT/READ_CACHE/PING/TABLE...). Nothing else
 * writes the socket, so no socket mutex is needed.
 */
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "tunnel_srv.h"
#include "queues.h"
#include "config.h"
#include "arbiter.h"
#include "state_cache.h"
#include "nvs_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "tunnel_srv";

typedef struct { uint8_t buf[TUNNEL_HDR_LEN + TUNNEL_MAX_PAYLOAD]; uint16_t len; } tun_out_t;
static QueueHandle_t s_out;
static volatile int  s_client = -1;
static volatile bool s_up = false;

bool tunnel_is_up(void) { return s_up; }

/* ---- framing ------------------------------------------------------------ */
static uint16_t frame(uint8_t *out, uint8_t type, uint8_t bms_id,
                      const uint8_t *pl, uint16_t len)
{
    out[0] = type; out[1] = bms_id;
    out[2] = len & 0xFF; out[3] = len >> 8;
    if (len) memcpy(out + TUNNEL_HDR_LEN, pl, len);
    return TUNNEL_HDR_LEN + len;
}
static void enqueue(uint8_t type, uint8_t bms_id, const uint8_t *pl, uint16_t len)
{
    if (len > TUNNEL_MAX_PAYLOAD) return;
    tun_out_t m; m.len = frame(m.buf, type, bms_id, pl, len);
    xQueueSend(s_out, &m, 0);   /* drop if backed up: state is refreshed anyway */
}

void tunnel_send_link(uint8_t id, tunnel_link_state_t st)
{ uint8_t b = (uint8_t)st; enqueue(TUN_LINK, id, &b, 1); }
void tunnel_send_write_result(uint8_t id, uint8_t idx, tunnel_write_status_t st)
{ uint8_t p[2] = { idx, (uint8_t)st }; enqueue(TUN_WRITE_RESULT, id, p, 2); }
void tunnel_send_read_cache(uint8_t id, uint8_t idx, const uint8_t *d, uint16_t len)
{
    uint8_t p[1 + TUNNEL_MAX_PAYLOAD]; p[0] = idx;
    if (len > TUNNEL_MAX_PAYLOAD - 1) len = TUNNEL_MAX_PAYLOAD - 1;
    memcpy(p + 1, d, len); enqueue(TUN_READ_CACHE, id, p, len + 1);
}

/* ---- resync: send blueprint + state (spec §6 TABLE_REQ handler) --------- */
static void send_blueprint(void)
{
    /* TABLE: shared blueprint (bms_id 0xFF). Take the first valid harvest. */
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        harvest_entry_t h;
        if (nvs_get_harvest(id, &h) && h.valid) {
            uint8_t pl[1 + HARVEST_MAX_CHARS * sizeof(tunnel_char_desc_t)];
            pl[0] = h.char_count;
            memcpy(pl + 1, h.chars, h.char_count * sizeof(tunnel_char_desc_t));
            enqueue(TUN_TABLE, TUNNEL_BMS_ID_LINK, pl,
                    1 + h.char_count * sizeof(tunnel_char_desc_t));
            break;
        }
    }
    /* IDENT + LINK per unit. */
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        harvest_entry_t h;
        if (nvs_get_harvest(id, &h) && h.valid)
            enqueue(TUN_IDENT, id, (const uint8_t *)h.name, strlen(h.name));
        bms_runtime_t rt; state_get_runtime(id, &rt);
        uint8_t st = (uint8_t)rt.link; enqueue(TUN_LINK, id, &st, 1);
        /* Prime read caches from the current cell/settings snapshot. */
        /* (READ_CACHE priming filled in once decode offsets are bench-verified.) */
    }
}

/* ---- inbound handling --------------------------------------------------- */
static void on_frame(uint8_t type, uint8_t bms_id, const uint8_t *pl, uint16_t len)
{
    switch (type) {
    case TUN_TABLE_REQ:
        send_blueprint();
        break;
    case TUN_WRITE:                       /* [idx][withResponse][data] */
        if (len >= 2)
            arbiter_app_write(bms_id, pl[0], pl[1], pl + 2, len - 2);
        break;
    case TUN_CLIENT:                      /* [connected] */
        if (len >= 1) arbiter_set_app_connected(bms_id, pl[0] != 0);
        break;
    case TUN_PING:
        break;                            /* liveness only */
    default:
        ESP_LOGW(TAG, "unexpected inbound type 0x%02x", type);
        break;
    }
}

/* ---- socket loop -------------------------------------------------------- */
static int listen_socket(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
                             .sin_port = htons(CFG_TUNNEL_PORT) };
    bind(s, (struct sockaddr *)&a, sizeof(a));
    listen(s, 1);
    return s;
}

static bool read_n(int s, uint8_t *buf, int n)
{
    int got = 0;
    while (got < n) {
        int r = recv(s, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

static void serve_client(int c)
{
    int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    s_client = c; s_up = true;
    xEventGroupSetBits(g_evt, EVT_TUNNEL_UP);
    ESP_LOGI(TAG, "tunnel client connected");

    int64_t last_ping = esp_timer_get_time();
    int64_t last_rx   = esp_timer_get_time();

    for (;;) {
        /* Drain outbound: NOTIFY + control. */
        notify_item_t it;
        while (xQueueReceive(g_q_notify, &it, 0) == pdTRUE) {
            uint8_t pl[1 + JK_FRAME_MAX]; pl[0] = it.idx;
            memcpy(pl + 1, it.data, it.len);
            uint8_t out[TUNNEL_HDR_LEN + 1 + JK_FRAME_MAX];
            uint16_t n = frame(out, TUN_NOTIFY, it.bms_id, pl, it.len + 1);
            if (send(c, out, n, 0) < 0) goto drop;
        }
        tun_out_t m;
        while (xQueueReceive(s_out, &m, 0) == pdTRUE)
            if (send(c, m.buf, m.len, 0) < 0) goto drop;

        /* Inbound with a short timeout so we loop for outbound + pings. */
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        fd_set r; FD_ZERO(&r); FD_SET(c, &r);
        int sel = select(c + 1, &r, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(c, &r)) {
            uint8_t hdr[TUNNEL_HDR_LEN];
            if (!read_n(c, hdr, TUNNEL_HDR_LEN)) goto drop;
            uint16_t plen = hdr[2] | (hdr[3] << 8);
            uint8_t pl[TUNNEL_MAX_PAYLOAD];
            if (plen > TUNNEL_MAX_PAYLOAD || (plen && !read_n(c, pl, plen))) goto drop;
            last_rx = esp_timer_get_time();
            on_frame(hdr[0], hdr[1], pl, plen);
        }

        int64_t now = esp_timer_get_time();
        if (now - last_ping > CFG_TUNNEL_PING_MS * 1000LL) {
            uint8_t out[TUNNEL_HDR_LEN];
            uint16_t n = frame(out, TUN_PING, TUNNEL_BMS_ID_LINK, NULL, 0);
            if (send(c, out, n, 0) < 0) goto drop;
            last_ping = now;
        }
        if (now - last_rx > CFG_TUNNEL_DEAD_MS * 1000LL) {
            ESP_LOGW(TAG, "tunnel peer silent > dead timeout"); goto drop;
        }
    }
drop:
    ESP_LOGW(TAG, "tunnel client dropped");
    s_up = false; s_client = -1;
    xEventGroupClearBits(g_evt, EVT_TUNNEL_UP);
    close(c);
}

static void tunnel_task(void *arg)
{
    int ls = listen_socket();
    for (;;) {
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        int c = accept(ls, (struct sockaddr *)&ca, &cl);
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        /* Single-client: replace any prior (should already be closed). */
        if (s_client >= 0) { close(s_client); s_client = -1; }
        serve_client(c);
    }
}

void tunnel_srv_start(void)
{
    s_out = xQueueCreate(16, sizeof(tun_out_t));
    xTaskCreatePinnedToCore(tunnel_task, "tunnel_srv", 6144, NULL, 5, NULL, tskNO_AFFINITY);
}
