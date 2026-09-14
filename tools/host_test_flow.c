/* Deterministic real arbiter + state cache integration. No hardware I/O.
 * Queue sets select the oldest pending event across their two member FIFOs.
 * The scripted BLE endpoint decides when to consume/complete a request. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "../node_a/main/arbiter.c"

typedef struct {
    unsigned capacity, size, head, count;
    unsigned long serial[24];
    unsigned char data[24][sizeof(arb_msg_t)];
} fake_queue_t;
static fake_queue_t inbox, requests, responses;
QueueHandle_t g_q_arb_in = &inbox, g_q_bms_request = &requests;
QueueHandle_t g_q_bms_response = &responses, g_q_notify, g_q_decode;
EventGroupHandle_t g_evt;
static int set_token;
static int64_t test_now;
static unsigned long serial;
static unsigned selections, tests;
static jmp_buf loop_done;

int64_t esp_timer_get_time(void) { return test_now; }
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t s)
{ (void)n; (void)s; assert(0); return NULL; }
QueueSetHandle_t xQueueCreateSet(UBaseType_t n)
{ assert(n == 36); return &set_token; }
BaseType_t xQueueAddToSet(QueueHandle_t q, QueueSetHandle_t s)
{ assert(s == &set_token && (q == &inbox || q == &responses)); return pdTRUE; }
BaseType_t xQueueSend(QueueHandle_t h, const void *v, TickType_t t)
{
    (void)t;
    fake_queue_t *q = h;
    assert(q == &inbox || q == &requests || q == &responses);
    if (q->count == q->capacity) return pdFALSE;
    unsigned tail = (q->head + q->count) % q->capacity;
    memcpy(q->data[tail], v, q->size);
    q->serial[tail] = ++serial;
    q->count++;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t h, void *v, TickType_t t)
{
    (void)t;
    fake_queue_t *q = h;
    if (!q->count) return pdFALSE;
    memcpy(v, q->data[q->head], q->size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return pdTRUE;
}
QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t s, TickType_t t)
{
    assert(s == &set_token && t == pdMS_TO_TICKS(500));
    if (selections++) longjmp(loop_done, 1);
    if (inbox.count && (!responses.count ||
        inbox.serial[inbox.head] < responses.serial[responses.head])) return &inbox;
    if (responses.count) return &responses;
    test_now += 500000;
    return NULL;
}
BaseType_t xTaskCreatePinnedToCore(void (*f)(void *), const char *n,
    uint32_t st, void *a, UBaseType_t p, void *h, BaseType_t c)
{ (void)f; (void)n; (void)st; (void)a; (void)p; (void)h; (void)c; assert(0); return pdFALSE; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b)
{ (void)g; return b; }
void tunnel_send_link(uint8_t id, tunnel_link_state_t st)
{ (void)id; (void)st; assert(0); }
void tunnel_send_write_result(uint8_t id, uint8_t idx, tunnel_write_status_t st)
{ (void)id; (void)idx; (void)st; assert(0); }
void mqtt_ack(uint8_t id, const char *cmd, const char *cid, const char *st,
              const char *detail, const char *readback)
{ (void)id; (void)cmd; (void)cid; (void)st; (void)detail; (void)readback; assert(0); }
void measure_start(uint8_t id, const char *cid)
{ (void)id; (void)cid; assert(0); }

static void tick(void)
{
    selections = 0;
    if (setjmp(loop_done) == 0) arbiter_task(NULL);
    assert(selections == 2);
}
static void reset(void)
{
    inbox = (fake_queue_t){ .capacity=24, .size=sizeof(arb_msg_t) };
    requests = (fake_queue_t){ .capacity=12, .size=sizeof(bms_request_t) };
    responses = (fake_queue_t){ .capacity=12, .size=sizeof(bms_response_t) };
    memset(s_pend, 0, sizeof(s_pend));
    memset(s_rb, 0, sizeof(s_rb));
    test_now = 100000000;
    serial = 0;
    for (unsigned id=0; id<CFG_NUM_UNITS; id++)
        state_set_link_state(id, LINK_UP, true, test_now);
}
static void submit(uint8_t id, uint8_t marker)
{
    bms_request_t r = { .bms_id=id, .kind=TXN_POLL, .source=SRC_INTERNAL,
        .opcode=JK_CMD_DEVICE_INFO, .timeout_ms=3000, .payload_len=1 };
    r.payload[0] = marker;
    arbiter_submit(&r);
    tick();
}
static bms_request_t take(void)
{
    bms_request_t r;
    assert(xQueueReceive(&requests, &r, 0) == pdTRUE);
    return r;
}
static void complete(bms_request_t r, resp_status_t st)
{
    bms_response_t rsp = { .bms_id=r.bms_id, .cmd_id=r.cmd_id, .status=st };
    assert(xQueueSend(&responses, &rsp, 0) == pdTRUE);
    tick();
}

static void test_connect_waits(void)
{
    for (unsigned explicit=0; explicit<2; explicit++) {
        reset(); state_set_link_state(1,LINK_REACHABLE_IDLE,false,0);
        bms_request_t r={.bms_id=1,.kind=explicit?TXN_CONNECT:TXN_POLL,
            .source=SRC_INTERNAL,.timeout_ms=9000,.payload_len=1,.payload={11}};
        arbiter_submit(&r); tick(); bms_request_t active=take();
        assert(active.kind==TXN_CONNECT && s_pend[1].count==1);
        for (unsigned i=1; i<PEND_DEPTH; i++) submit(1,20+i);
        assert(s_pend[1].count==PEND_DEPTH);
        s_pend[1].backoff_ms=8000; s_pend[1].connect_after_us=test_now-1;
        int64_t previous_backoff=s_pend[1].connect_after_us;
        for (unsigned cycle=0; cycle<100; cycle++) {
            complete(active,RESP_CONNECT_WAIT);
            assert(!s_pend[1].busy && !requests.count && s_pend[1].count==PEND_DEPTH);
            assert(s_pend[1].backoff_ms==8000 && s_pend[1].connect_after_us==previous_backoff);
            assert(s_pend[1].dispatch_after_us==test_now+100000);
            tick(); bms_request_t retry=take();
            assert(retry.kind==TXN_CONNECT && retry.cmd_id>active.cmd_id);
            assert(s_pend[1].ring[s_pend[1].head].payload[0]==11);
            complete(active,RESP_CONNECT_WAIT); /* delayed old result cannot release retry */
            assert(s_pend[1].busy && !requests.count && s_pend[1].count==PEND_DEPTH);
            active=retry; tests++;
        }
        /* A genuine failure still escalates the existing remote history. */
        complete(active,RESP_LINK_DOWN);
        assert(s_pend[1].backoff_ms==16000 && s_pend[1].connect_after_us==test_now+16000000);
        assert(s_pend[1].count==PEND_DEPTH-explicit && !requests.count);
        tests++;
    }
    /* A retained explicit CONNECT releases only its own head, and never
     * resurrects a connect deliberately removed by CLEAR. */
    const resp_status_t ends[]={RESP_OK,RESP_LINK_DOWN,RESP_CONNECT_WAIT};
    for (unsigned i=0; i<3; i++) {
        reset();
        bms_request_t c={.bms_id=1,.kind=TXN_CONNECT,.source=SRC_INTERNAL,.timeout_ms=9000};
        arbiter_submit(&c); tick(); bms_request_t active=take();
        assert(s_pend[1].count==1);
        arbiter_clear_pending(1); tick(); assert(s_pend[1].count==0);
        submit(1,88); assert(s_pend[1].count==1);
        complete(active,ends[i]);
        if (ends[i]!=RESP_OK) tick();
        bms_request_t kept=take(); assert(kept.kind==TXN_POLL && kept.payload[0]==88);
        complete(kept,RESP_OK); assert(!s_pend[1].busy && !s_pend[1].count);
        tests++;
    }
    reset();
    bms_request_t c={.bms_id=1,.kind=TXN_CONNECT,.source=SRC_APP,.timeout_ms=9000};
    arbiter_submit(&c); tick(); bms_request_t active=take(); submit(1,77);
    complete(active,RESP_OK); bms_request_t next=take();
    assert(next.kind==TXN_POLL && next.payload[0]==77 && !s_pend[1].count);
    complete(next,RESP_OK); tests++;
}

int main(void)
{
    state_cache_init();
    /* No producer consumes the next bank's response; each bank serializes. */
    reset();
    bms_request_t first[CFG_NUM_UNITS];
    for (unsigned id=0; id<CFG_NUM_UNITS; id++) {
        submit(id, 1); first[id] = take();
        submit(id, 2); assert(requests.count == 0 && s_pend[id].count == 1);
    }
    for (int id=CFG_NUM_UNITS-1; id>=0; id--) {
        complete(first[id], RESP_OK);
        bms_request_t second = take();
        assert(second.bms_id == id && second.payload[0] == 2);
        assert(second.cmd_id != first[id].cmd_id);
        complete(second, RESP_OK);
        assert(!s_pend[id].busy);
        tests++;
    }

    /* Implicit connect consumes no pending opcode until its exact result. */
    reset();
    state_set_link_state(1, LINK_REACHABLE_IDLE, false, 0);
    submit(1, 7);
    bms_request_t connect = take();
    assert(connect.kind == TXN_CONNECT && s_pend[1].count == 1);
    complete(connect, RESP_LINK_DOWN);
    assert(!requests.count && s_pend[1].backoff_ms == 2000);
    tick(); assert(!requests.count);
    test_now += 2000000; tick();
    connect = take(); assert(connect.kind == TXN_CONNECT);
    state_set_link_state(1, LINK_UP, true, test_now);
    complete(connect, RESP_OK);
    bms_request_t poll = take(); assert(poll.payload[0] == 7);
    complete(poll, RESP_OK); tests++;

    /* Delayed duplicate of the previous operation must not release this one,
     * change retry policy, or dispatch another request. */
    for (unsigned status=RESP_OK; status<=RESP_CONNECT_WAIT; status++) {
        reset(); submit(1, 1); bms_request_t old = take();
        complete(old, RESP_OK);
        submit(1, 2); bms_request_t active = take();
        submit(1, 3);
        complete(old, (resp_status_t)status);
#ifdef FLOW_LEGACY_CORRELATION
        assert(!s_pend[1].busy || requests.count);
#else
        assert(s_pend[1].busy && requests.count == 0 && s_pend[1].count == 1);
        assert(s_pend[1].backoff_ms == 0 && s_pend[1].dispatch_after_us == 0);
        complete(active, RESP_OK);
        bms_request_t next = take(); assert(next.payload[0] == 3);
        complete(next, RESP_OK);
#endif
        (void)active;
        tests++;
    }

#ifndef FLOW_LEGACY_CORRELATION
    /* No alias at the old 16-bit boundary, and fail closed at true exhaustion. */
    const bms_cmd_id_t starts[] = { UINT16_MAX-1, UINT32_MAX, UINT64_MAX-1 };
    for (unsigned i=0; i<sizeof(starts)/sizeof(starts[0]); i++) {
        reset(); s_pend[1].next_cmd_id = starts[i];
        submit(1, 1); bms_request_t r = take();
        assert(r.cmd_id == starts[i]+1 && r.cmd_id != 0);
        complete(r, RESP_OK);
        complete(r, RESP_LINK_DOWN); /* duplicate while idle is also inert */
        assert(!s_pend[1].busy && s_pend[1].backoff_ms == 0);
        if (starts[i] == UINT64_MAX-1) {
            submit(1, 2); tick();
            assert(!requests.count && s_pend[1].count == 1 && !s_pend[1].busy);
        }
        tests++;
    }
#endif

    /* A full BLE queue must leave the oldest pending request at the head. */
    reset(); submit(1, 1); bms_request_t active = take();
    submit(1, 2); submit(1, 3); submit(1, 4);
    bms_request_t filler = { .bms_id=99 };
    for (unsigned i=0; i<requests.capacity; i++)
        assert(xQueueSend(&requests, &filler, 0) == pdTRUE);
    complete(active, RESP_OK); /* response handler and end-of-tick both retry */
#ifdef FLOW_LEGACY_FIFO
    assert(s_pend[1].ring[s_pend[1].head].payload[0] != 2);
#else
    assert(s_pend[1].ring[s_pend[1].head].payload[0] == 2);
    pend_t retained = s_pend[1];
    for (unsigned i=0; i<100; i++) tick();
    assert(!memcmp(&retained, &s_pend[1], sizeof(retained)));
    while (requests.count) assert(take().bms_id == 99);
    for (unsigned i=2; i<=4; i++) {
        tick(); bms_request_t r = take(); assert(r.payload[0] == i);
        complete(r, RESP_OK);
        /* on_response dispatches the next request immediately */
    }
#endif
    tests++;
#ifndef FLOW_LEGACY_FIFO
    /* Repeatedly wrap the physical FIFO with maximum pending depth. */
    for (unsigned round=0; round<32; round++) {
        reset(); submit(1, 0); active = take();
        for (unsigned i=1; i<=PEND_DEPTH; i++) submit(1, i);
        assert(s_pend[1].count == PEND_DEPTH);
        for (unsigned i=0; i<PEND_DEPTH; i++) {
            /* Saturate, complete, retain, drain, and resume in exact order. */
            for (unsigned j=0; j<requests.capacity; j++)
                assert(xQueueSend(&requests, &filler, 0) == pdTRUE);
            complete(active, RESP_OK);
            assert(s_pend[1].count == PEND_DEPTH-i);
            while (requests.count) assert(take().bms_id == 99);
            tick(); active = take(); assert(active.payload[0] == i+1);
            tests++;
        }
        complete(active, RESP_OK);
        assert(!requests.count && !s_pend[1].busy && !s_pend[1].count);
    }
#endif
    test_connect_waits();
    printf("PASS: %u deterministic production arbiter/queue/clock scenarios", tests);
#ifdef FLOW_LEGACY_CORRELATION
    printf(" (stale-completion defect expected)");
#endif
#ifdef FLOW_LEGACY_FIFO
    printf(" (queue-reordering defect expected)");
#endif
    puts("");
    return 0;
}
