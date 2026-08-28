/*
 * tunnel_cli.c — Node B side of the tunnel. One task owns the socket; ble_periph
 * enqueues WRITE/CLIENT via the out-queue. Inbound frames update nb_state and
 * drive adv_mgr / ble_periph.
 */
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include "tunnel_cli.h"
#include "config.h"
#include "nb_state.h"
#include "ble_periph.h"
#include "adv_mgr.h"
#include "tunnel_proto.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "tunnel_cli";

typedef struct { uint8_t buf[TUNNEL_HDR_LEN + TUNNEL_MAX_PAYLOAD]; uint16_t len; } tun_out_t;
static QueueHandle_t s_out;
static volatile bool s_up;

bool tunnel_cli_up(void) { return s_up; }

static uint16_t frame(uint8_t *o, uint8_t type, uint8_t id, const uint8_t *pl, uint16_t len)
{ o[0]=type; o[1]=id; o[2]=len&0xFF; o[3]=len>>8; if(len) memcpy(o+4,pl,len); return 4+len; }

static void enqueue(uint8_t type, uint8_t id, const uint8_t *pl, uint16_t len)
{ tun_out_t m; m.len = frame(m.buf, type, id, pl, len); xQueueSend(s_out, &m, 0); }

void tunnel_cli_send_write(uint8_t id, uint8_t idx, bool wr, const uint8_t *d, uint16_t len)
{
    uint8_t pl[2 + TUNNEL_MAX_PAYLOAD]; pl[0]=idx; pl[1]=wr?1:0;
    if (len > TUNNEL_MAX_PAYLOAD-2) len = TUNNEL_MAX_PAYLOAD-2;
    memcpy(pl+2, d, len); enqueue(TUN_WRITE, id, pl, len+2);
}
void tunnel_cli_send_client(uint8_t id, bool c)
{ uint8_t b = c?1:0; enqueue(TUN_CLIENT, id, &b, 1); }

/* ---- inbound ------------------------------------------------------------ */
static void on_frame(uint8_t type, uint8_t id, const uint8_t *pl, uint16_t len)
{
    switch (type) {
    case TUN_TABLE: {                         /* [char_count][descs...] */
        nb_blueprint_t bp = {0};
        bp.char_count = pl[0];
        if (bp.char_count > NB_MAX_CHARS) bp.char_count = NB_MAX_CHARS;
        memcpy(bp.chars, pl+1, bp.char_count * sizeof(tunnel_char_desc_t));
        nb_set_blueprint(&bp);
        ble_periph_rebuild_table();           /* register the single shared table */
        break;
    }
    case TUN_IDENT: {                         /* advertised name */
        char name[32] = {0}; uint16_t n = len < 31 ? len : 31;
        memcpy(name, pl, n);
        /* Clones advertise TUN instead of BMS: in-garage the real unit and
         * the clone are otherwise indistinguishable in a scan list (owner
         * request 2026-08-29), and distinct names also foreclose the whole
         * clone-confusion class (self-loops, wrong-target app sessions). */
        if (strncmp(name, "BMS", 3) == 0) memcpy(name, "TUN", 3);
        nb_set_ident_name(id, name);
        adv_mgr_set_name(id, name);
        break;
    }
    case TUN_READ_CACHE:                      /* [idx][data] */
        if (len >= 1) nb_set_cache(id, pl[0], pl+1, len-1);
        break;
    case TUN_NOTIFY:                          /* [idx][complete frame] */
        /* Cache refresh ONLY. The app stream is TUN_RAW (verbatim chunks) —
         * forwarding reassembled frames here too would duplicate the data. */
        if (len >= 1) nb_set_cache(id, pl[0], pl+1, len-1);
        break;
    case TUN_RAW:                             /* [idx][verbatim BMS chunk] */
        if (len >= 1) ble_periph_forward_notify(id, pl[0], pl+1, len-1);
        break;
    case TUN_WRITE_RESULT:                    /* [idx][status] */
        if (len >= 2) ble_periph_on_write_result(id, pl[0], pl[1]);
        break;
    case TUN_LINK:                            /* [state] */
        if (len >= 1) { nb_set_link(id, pl[0]); adv_mgr_on_link(id, pl[0]); }
        break;
    case TUN_PING:
        break;
    default:
        ESP_LOGW(TAG, "unexpected inbound 0x%02x", type);
    }
}

/* ---- resync (spec §6) --------------------------------------------------- */
static void resync(void)
{
    enqueue(TUN_TABLE_REQ, TUNNEL_BMS_ID_LINK, NULL, 0);
    /* Replay CLIENT for any identity that currently holds an app connection,
     * so A's arbitration state is reconstructed after a blip. */
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        nb_identity_t it; nb_get_identity(id, &it);
        if (it.connected) tunnel_cli_send_client(id, true);
    }
}

/* ---- socket loop -------------------------------------------------------- */
static bool read_n(int s, uint8_t *b, int n)
{ int g=0; while(g<n){int r=recv(s,b+g,n-g,0); if(r<=0) return false; g+=r;} return true; }

static int connect_a(void)
{
    /* Resolve Node A by hostname each attempt — its address comes from DHCP and
     * may change (spec §12). getaddrinfo handles DNS names, .local mDNS names,
     * and literal IPs uniformly, so NODE_A_HOST can be any of them. */
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port[8]; snprintf(port, sizeof(port), "%d", NODE_A_PORT);
    if (getaddrinfo(NODE_A_HOST, port, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "resolve '%s' failed", NODE_A_HOST);
        return -1;
    }
    int s = socket(res->ai_family, res->ai_socktype, 0);
    if (s < 0) { freeaddrinfo(res); return -1; }
    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        close(s); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return s;
}

static void serve(int s)
{
    s_up = true;
    ESP_LOGI(TAG, "tunnel up");
    resync();
    int64_t last_ping = esp_timer_get_time(), last_rx = esp_timer_get_time();

    /* Off-stack: this task also runs the NimBLE notify chain via on_frame ->
     * ble_periph_forward_notify, so keep serve()'s own frame stays small. Safe
     * as statics — only the single tunnel task touches them. */
    static tun_out_t m;
    static uint8_t   pl[TUNNEL_MAX_PAYLOAD];
    for (;;) {
        while (xQueueReceive(s_out, &m, 0) == pdTRUE)
            if (send(s, m.buf, m.len, 0) < 0) goto drop;

        struct timeval tv = { .tv_sec=0, .tv_usec=100000 };
        fd_set r; FD_ZERO(&r); FD_SET(s, &r);
        if (select(s+1, &r, NULL, NULL, &tv) > 0 && FD_ISSET(s, &r)) {
            uint8_t h[TUNNEL_HDR_LEN];
            if (!read_n(s, h, TUNNEL_HDR_LEN)) goto drop;
            uint16_t plen = h[2] | (h[3]<<8);
            if (plen > TUNNEL_MAX_PAYLOAD || (plen && !read_n(s, pl, plen))) goto drop;
            last_rx = esp_timer_get_time();
            on_frame(h[0], h[1], pl, plen);
        }
        int64_t now = esp_timer_get_time();
        if (now - last_ping > CFG_TUNNEL_PING_MS*1000LL) {
            uint8_t o[TUNNEL_HDR_LEN]; uint16_t n = frame(o, TUN_PING, TUNNEL_BMS_ID_LINK, NULL, 0);
            if (send(s, o, n, 0) < 0) goto drop;
            last_ping = now;
        }
        if (now - last_rx > CFG_TUNNEL_DEAD_MS*1000LL) { ESP_LOGW(TAG, "peer silent"); goto drop; }
    }
drop:
    s_up = false; close(s);
    ESP_LOGW(TAG, "tunnel down");
}

static void tunnel_task(void *arg)
{
    int64_t hold_deadline = 0;   /* drop app conns if still down past this */
    bool    torn_down = false;

    for (;;) {
        int s = connect_a();
        if (s >= 0) {
            /* Reconnected within (or after) grace: resync happens in serve(). */
            hold_deadline = 0; torn_down = false;
            serve(s);                                   /* returns on drop */
            /* Just dropped — start the grace window (spec §6). App connections
             * are held meanwhile; ble_periph queues app writes until resync. */
            hold_deadline = esp_timer_get_time() + CFG_TUNNEL_GRACE_MS * 1000LL;
        } else {
            /* Still down. Once past the grace window, drop once (spec §6/§11). */
            if (!torn_down && hold_deadline &&
                esp_timer_get_time() > hold_deadline) {
                ESP_LOGW(TAG, "grace expired — drop app conns, pause adv");
                ble_periph_drop_all();
                adv_mgr_pause_all();
                torn_down = true;
            }
            vTaskDelay(pdMS_TO_TICKS(CFG_TUNNEL_RECONNECT_MS));
        }
    }
}

void tunnel_cli_start(void)
{
    s_out = xQueueCreate(8, sizeof(tun_out_t));  /* 8 x ~520 B (heap guard) */
    xTaskCreatePinnedToCore(tunnel_task, "tunnel_cli", 8192, NULL, 5, NULL, 0);
}
