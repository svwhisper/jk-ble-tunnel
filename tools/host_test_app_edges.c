/* Real arbiter task handler plus real locked state cache. Queue adapters
 * capture requests but never execute BLE, network, serial or battery I/O. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "../node_a/main/arbiter.c"

static int arb_token, request_token, response_token, set_token;
QueueHandle_t g_q_arb_in = &arb_token, g_q_bms_request = &request_token;
QueueHandle_t g_q_bms_response = &response_token, g_q_notify, g_q_decode;
EventGroupHandle_t g_evt;
static jmp_buf loop_done;
static arb_msg_t incoming, emitted;
static unsigned selections, sends, active_signals;
static int64_t test_now = 1000000;

int64_t esp_timer_get_time(void) { return test_now; }
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t size)
{ (void)n; (void)size; assert(0); return NULL; }
QueueSetHandle_t xQueueCreateSet(UBaseType_t n)
{ assert(n == 36); return &set_token; }
BaseType_t xQueueAddToSet(QueueHandle_t q, QueueSetHandle_t s)
{ assert(s == &set_token && (q == g_q_arb_in || q == g_q_bms_response)); return pdTRUE; }
QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t s, TickType_t t)
{
    assert(s == &set_token && t == pdMS_TO_TICKS(500));
    if (selections++) longjmp(loop_done, 1);
    return g_q_arb_in;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t t)
{ assert(q == g_q_arb_in && t == 0); memcpy(out, &incoming, sizeof(incoming)); return pdTRUE; }
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t)
{
    assert(q == g_q_arb_in && t == pdMS_TO_TICKS(20));
    emitted = *(const arb_msg_t *)item;
    sends++;
    return pdTRUE;
}
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                 uint32_t stack, void *arg, UBaseType_t priority,
                                 void *handle, BaseType_t core)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; (void)core; assert(0); return pdFALSE; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b)
{ (void)g; assert(b == EVT_APP_ACTIVE); active_signals++; return b; }
void tunnel_send_link(uint8_t id, tunnel_link_state_t st)
{ (void)id; (void)st; assert(0); }
void tunnel_send_write_result(uint8_t id, uint8_t idx, tunnel_write_status_t st)
{ (void)id; (void)idx; (void)st; assert(0); }
void mqtt_ack(uint8_t id, const char *cmd, const char *cid, const char *st,
              const char *detail, const char *readback)
{ (void)id; (void)cmd; (void)cid; (void)st; (void)detail; (void)readback; assert(0); }
void measure_start(uint8_t id, const char *cid)
{ (void)id; (void)cid; assert(0); }

static bms_runtime_t runtime(uint8_t id)
{ bms_runtime_t rt; state_get_runtime(id, &rt); return rt; }

static void client_message(uint8_t id, bool connected)
{
    incoming = (arb_msg_t){ .kind = ARB_APP_CONN, .bms_id = id, .connected = connected };
    selections = sends = 0;
    memset(&emitted, 0, sizeof(emitted));
    if (setjmp(loop_done) == 0) arbiter_task(NULL);
    assert(selections == 2);
}

int main(void)
{
    state_cache_init();
    /* Exact regression: B's idle CLIENT=false resync must not create demand. */
    client_message(1, false);
    if (sends || runtime(1).app_left_us) {
        fprintf(stderr, "FAIL: idle CLIENT=false emitted %u requests and set departure=%lld\n",
                sends, (long long)runtime(1).app_left_us);
        return 1;
    }

    unsigned cases = 1;
    for (uint8_t id = 0; id < CFG_NUM_UNITS; id++) {
        for (unsigned held = 0; held < 2; held++) {
            state_set_link_state(id, held ? LINK_UP : LINK_REACHABLE_IDLE, held, test_now);
            /* Real attach still requests a connection; true resync behaviour
             * is deliberately unchanged by this false-edge-only stage. */
            for (unsigned repeat = 0; repeat < 2; repeat++) {
                test_now += 1000;
                unsigned previous_signals = active_signals;
                client_message(id, true);
                assert(sends == 1 && active_signals == previous_signals + 1);
                assert(emitted.kind == ARB_REQ && emitted.bms_id == id);
                assert(emitted.req.kind == TXN_CONNECT && emitted.req.source == SRC_APP);
                assert(runtime(id).app_connected && runtime(id).app_left_us == 0);
                assert(s_pend[id].link_wait_deadline_us == test_now + CFG_APP_LINK_TIMEOUT_MS * 1000LL);
                cases++;
            }
            /* A genuine departure preserves the existing single post-app
             * read (opcode policy belongs to its own later stage). */
            test_now += 1000;
            client_message(id, false);
            assert(sends == 1 && emitted.req.kind == TXN_POLL);
            assert(emitted.req.source == SRC_INTERNAL && emitted.req.opcode == JK_CMD_DEVICE_INFO);
            assert(!runtime(id).app_connected && runtime(id).app_left_us == test_now);
            assert(s_pend[id].link_wait_deadline_us == 0);
            int64_t departure = test_now;
            cases++;

            bms_runtime_t other[CFG_NUM_UNITS];
            for (unsigned k = 0; k < CFG_NUM_UNITS; k++) other[k] = runtime(k);
            for (unsigned cleared = 0; cleared < 2; cleared++) {
                if (cleared) state_clear_app_left_if(id, departure);
                for (unsigned repeat = 0; repeat < 1000; repeat++) {
                    test_now += 100000;
                    client_message(id, false);
                    assert(sends == 0 && !runtime(id).app_connected);
                    assert(runtime(id).app_left_us == (cleared ? 0 : departure));
                    assert(runtime(id).link_held == (bool)held);
                    for (unsigned k = 0; k < CFG_NUM_UNITS; k++) {
                        if (k == id) continue;
                        bms_runtime_t rt = runtime(k);
                        assert(memcmp(&rt, &other[k], sizeof(rt)) == 0);
                    }
                    cases++;
                }
            }
        }
    }
    printf("PASS: production app handler/state cache, %u attach/departure/resync cases\n", cases);
    return 0;
}
