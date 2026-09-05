/* Exercise the production ARB_CLEAR message handler, not a copied algorithm.
 * Single-threaded queue/time adapters drive one real task-loop iteration.
 * Every outbound operation is trapped; this executable has no device I/O. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "../node_a/main/arbiter.c"

static int arb_token, request_token, response_token, set_token;
QueueHandle_t g_q_arb_in = &arb_token;
QueueHandle_t g_q_bms_request = &request_token;
QueueHandle_t g_q_bms_response = &response_token;
QueueHandle_t g_q_notify, g_q_decode;
EventGroupHandle_t g_evt;
static jmp_buf loop_done;
static arb_msg_t incoming;
static unsigned selections, sends;

int64_t esp_timer_get_time(void) { return 1000000; }
QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t size)
{ (void)count; (void)size; assert(0); return NULL; }
QueueSetHandle_t xQueueCreateSet(UBaseType_t count)
{ assert(count == 36); return &set_token; }
BaseType_t xQueueAddToSet(QueueHandle_t queue, QueueSetHandle_t set)
{ assert(set == &set_token); assert(queue == g_q_arb_in || queue == g_q_bms_response); return pdTRUE; }
QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t set, TickType_t ticks)
{
    assert(set == &set_token && ticks == pdMS_TO_TICKS(500));
    if (selections++) longjmp(loop_done, 1);
    return g_q_arb_in;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{ assert(queue == g_q_arb_in && ticks == 0); memcpy(item, &incoming, sizeof(incoming)); return pdTRUE; }
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks)
{ (void)queue; (void)item; (void)ticks; sends++; assert(0); return pdFALSE; }
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                 uint32_t stack, void *arg, UBaseType_t priority,
                                 void *handle, BaseType_t core)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; (void)core; assert(0); return pdFALSE; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{ (void)group; (void)bits; assert(0); return 0; }
void state_get_runtime(uint8_t id, bms_runtime_t *out)
{ (void)id; *out = (bms_runtime_t){ .link = LINK_UP, .link_held = true }; }
bool state_snapshot(uint8_t id, bms_state_t *out)
{ (void)id; (void)out; assert(0); return false; }
void state_set_app_connected(uint8_t id, bool connected, int64_t now)
{ (void)id; (void)connected; (void)now; assert(0); }
bool state_mark_idle_if_unheld(uint8_t id)
{ (void)id; assert(0); return false; }
void tunnel_send_link(uint8_t id, tunnel_link_state_t state)
{ (void)id; (void)state; assert(0); }
void tunnel_send_write_result(uint8_t id, uint8_t idx, tunnel_write_status_t status)
{ (void)id; (void)idx; (void)status; assert(0); }
void mqtt_ack(uint8_t id, const char *cmd, const char *cid, const char *status,
              const char *detail, const char *readback)
{ (void)id; (void)cmd; (void)cid; (void)status; (void)detail; (void)readback; assert(0); }
void measure_start(uint8_t id, const char *cid)
{ (void)id; (void)cid; assert(0); }

static void clear_once(uint8_t id)
{
    incoming = (arb_msg_t){ .kind = ARB_CLEAR, .bms_id = id };
    selections = 0;
    if (setjmp(loop_done) == 0) arbiter_task(NULL);
    assert(selections == 2 && sends == 0);
}

int main(void)
{
    unsigned cases = 0;
    /* Every occupancy, physical head and internal-poll placement; cycle every
     * transaction kind/source pairing for the entries that must survive. */
    for (unsigned busy = 0; busy < 2; busy++)
    for (unsigned variant = 0; variant < 15; variant++)
    for (unsigned head = 0; head < PEND_DEPTH; head++)
    for (unsigned count = 0; count <= PEND_DEPTH; count++)
    for (unsigned mask = 0; mask < (1u << count); mask++) {
        memset(s_pend, 0, sizeof(s_pend));
        for (unsigned bank = 0; bank < CFG_NUM_UNITS; bank++) s_pend[bank].busy = true;
        uint8_t id = cases % CFG_NUM_UNITS;
        pend_t *p = &s_pend[id];
        p->busy = busy;
        p->head = p->tail = head;
        p->next_cmd_id = 4321;
        p->link_wait_deadline_us = 2000000;
        p->connect_after_us = 3000000;
        p->dispatch_after_us = 4000000;
        p->backoff_ms = 8000;
        bms_request_t expected[PEND_DEPTH];
        unsigned retained = 0;
        for (unsigned i = 0; i < count; i++) {
            bms_request_t req = {0};
            req.bms_id = id;
            req.kind = (variant + i) % 5;
            req.source = ((variant + i) / 5) % 3;
            if (req.kind == TXN_POLL && req.source == SRC_INTERNAL) req.kind = TXN_RAW_WRITE;
            if (mask & (1u << i)) { req.kind = TXN_POLL; req.source = SRC_INTERNAL; }
            req.cmd_id = 100 + i;
            req.opcode = i & 1 ? JK_CMD_CELL_INFO : JK_CMD_DEVICE_INFO;
            req.idx = i & 1;
            req.with_response = i & 1;
            req.response_needed = true;
            req.timeout_ms = 3000 + i;
            req.payload_len = REQ_PAYLOAD_MAX;
            memset(req.payload, i + variant, sizeof(req.payload));
            assert(ring_push(p, &req));
            if (!(mask & (1u << i))) expected[retained++] = req;
        }
        pend_t before[CFG_NUM_UNITS];
        memcpy(before, s_pend, sizeof(before));
        clear_once(id);
        if (p->count != retained) {
            fprintf(stderr, "FAIL: ARB_CLEAR retained %u requests, expected %u (head=%u count=%u mask=%u)\n",
                    p->count, retained, head, count, mask);
            return 1;
        }
        assert(p->busy == before[id].busy && p->next_cmd_id == 4321);
        assert(p->link_wait_deadline_us == 2000000 && p->connect_after_us == 3000000);
        assert(p->dispatch_after_us == 4000000 && p->backoff_ms == 8000);
        for (unsigned bank = 0; bank < CFG_NUM_UNITS; bank++)
            if (bank != id) assert(memcmp(&before[bank], &s_pend[bank], sizeof(pend_t)) == 0);
        clear_once(id); /* idempotent, no second loss/reordering */
        assert(p->count == retained);
        bms_request_t out;
        for (unsigned i = 0; i < retained; i++) {
            assert(ring_pop(p, &out));
            assert(memcmp(&out, &expected[i], sizeof(out)) == 0);
        }
        assert(!ring_pop(p, &out));
        /* Queue still supports a complete fill/drain after filtering. */
        for (unsigned i = 0; i < PEND_DEPTH; i++) {
            bms_request_t r = { .cmd_id = i };
            assert(ring_push(p, &r));
        }
        assert(!ring_push(p, &out));
        for (unsigned i = 0; i < PEND_DEPTH; i++) {
            assert(ring_pop(p, &out) && out.cmd_id == i);
        }
        cases++;
    }
    printf("PASS: production ARB_CLEAR handler, %u queue/order/isolation cases\n", cases);
    return 0;
}
