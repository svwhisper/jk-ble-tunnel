/* Offline boundary checks. See tools/run_host_tests.sh. Never sends commands. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "command_validation.h"
#include "tunnel_validate.h"
#include "jk_proto.h"
#include "synth_frames.h"

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void json_case(const char *json, const char *error, double expected)
{
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    const cJSON *it;
    double value;
    const char *actual = balance_command_validate(root, &it, &value);
    CHECK(error ? actual && !strcmp(error, actual) : actual == NULL);
    if (!error) { CHECK(it != NULL); CHECK(value == expected); }
    cJSON_Delete(root);
}

int main(void)
{
    json_case("[1]", "bad_json", 0);
    json_case("null", "bad_json", 0);
    json_case("true", "bad_json", 0);
    json_case("{", "bad_json", 0);
    json_case("{}", "one_key_per_command", 0);
    json_case("{\"id\":\"a\"}", "one_key_per_command", 0);
    json_case("{\"balancing_enabled\":\"true\"}", "bad_value", 0);
    json_case("{\"balancing_enabled\":null}", "bad_value", 0);
    json_case("{\"balancing_enabled\":{}}", "bad_value", 0);
    json_case("{\"balancing_enabled\":[]}", "bad_value", 0);
    json_case("{\"balancing_enabled\":0.5}", "bad_value", 0);
    json_case("{\"balancing_enabled\":1e999}", "bad_value", 0);
    json_case("{\"balance_current\":true}", "bad_value", 0);
    json_case("{\"cell_count\":15.5}", "bad_value", 0);
    json_case("{\"balancing_enabled\":1,\"balancing_enabled\":0}", "one_key_per_command", 0);
    json_case("{\"balancing_enabled\":1,\"balance_current\":1}", "one_key_per_command", 0);
    json_case("{\"id\":4,\"balancing_enabled\":1}", "bad_id", 0);
    json_case("{\"id\":\"a\",\"id\":\"b\",\"balancing_enabled\":1}", "bad_id", 0);
    json_case("{\"id\":\"12345678901234567890123456789012\",\"balancing_enabled\":1}", "bad_id", 0);
    json_case("{\"balancing_enabled\":1} garbage", "bad_json", 0);
    json_case("{\"balancing_enabled\":true}", NULL, 1);
    json_case("{\"balancing_enabled\":false}", NULL, 0);
    json_case("{\"balancing_enabled\":0}", NULL, 0);
    json_case("{\"balancing_enabled\":1}", NULL, 1);
    json_case("{\"balance_current\":1.5,\"id\":\"abc\"}", NULL, 1.5);
    json_case("{\"id\":\"abc\",\"balance_trigger_voltage\":0.01}", NULL, 0.01);
    json_case("{\"cell_count\":16}", NULL, 16);
    json_case("{\"cell_count\":15}", NULL, 15);

    CHECK(command_event_valid(false, 0, 30, 30, 20));
    CHECK(!command_event_valid(true, 0, 30, 30, 20));
    CHECK(!command_event_valid(false, 10, 30, 20, 0));
    CHECK(!command_event_valid(false, 0, 60, 30, 20));
    CHECK(!command_event_valid(false, 0, 192, 192, 20));
    CHECK(!command_event_valid(false, 0, 0, 0, 20));
    CHECK(!command_event_valid(false, 0, 30, 30, 64));
    CHECK(!command_event_valid(false, 0, -1, -1, 20));

    uint8_t p[513] = {0};
    CHECK(tunnel_to_a_valid(TUN_WRITE, 0, p, 22, 4));
    CHECK(tunnel_to_a_valid(TUN_WRITE, 3, p, 34, 4));
    CHECK(!tunnel_to_a_valid(TUN_WRITE, 4, p, 22, 4));
    CHECK(!tunnel_to_a_valid(TUN_WRITE, 255, p, 22, 4));
    CHECK(!tunnel_to_a_valid(TUN_WRITE, 0, p, 2, 4));
    CHECK(!tunnel_to_a_valid(TUN_WRITE, 0, p, 35, 4));
    CHECK(!tunnel_to_a_valid(TUN_WRITE, 0, p, 257, 4));
    CHECK(!tunnel_to_a_valid(TUN_WRITE, 0, p, 258, 4));
    p[0] = 2; CHECK(!tunnel_to_a_valid(TUN_WRITE, 0, p, 22, 4));
    p[0] = 0; p[1] = 2; CHECK(!tunnel_to_a_valid(TUN_WRITE, 0, p, 22, 4));
    p[1] = 0;
    CHECK(tunnel_to_a_valid(TUN_CLIENT, 0, p, 1, 4));
    CHECK(!tunnel_to_a_valid(TUN_CLIENT, 0, p, 2, 4));
    CHECK(tunnel_to_a_valid(TUN_PING, 255, NULL, 0, 4));
    CHECK(!tunnel_to_a_valid(TUN_PING, 0, NULL, 0, 4));
    CHECK(tunnel_to_a_valid(TUN_TABLE_REQ, 255, NULL, 0, 4));
    CHECK(!tunnel_to_b_valid(TUN_TABLE, 255, NULL, 0, 4, 8, 320));
    p[0] = 2;
    CHECK(tunnel_to_b_valid(TUN_TABLE, 255, p, 11, 4, 8, 320));
    CHECK(!tunnel_to_b_valid(TUN_TABLE, 255, p, 10, 4, 8, 320));
    p[0] = 9; CHECK(!tunnel_to_b_valid(TUN_TABLE, 255, p, 46, 4, 8, 320));
    p[0] = 0;
    CHECK(tunnel_to_b_valid(TUN_NOTIFY, 3, p, 301, 4, 8, 320));
    CHECK(!tunnel_to_b_valid(TUN_NOTIFY, 4, p, 301, 4, 8, 320));
    CHECK(!tunnel_to_b_valid(TUN_NOTIFY, 3, p, 322, 4, 8, 320));

    /* ASAN checks every payload read with an exact-size allocation, including
     * zero-length frames and all type/id combinations at each wire length. */
    unsigned ids[] = {0, 3, 4, 255};
    for (size_t len = 0; len <= 513; len++) {
        uint8_t *data = len ? malloc(len) : NULL;
        if (len) memset(data, 0xff, len);
        for (unsigned type = 0; type < 256; type++) {
            for (unsigned i = 0; i < 4; i++) {
                (void)tunnel_to_a_valid(type, ids[i], data, len, 4);
                (void)tunnel_to_b_valid(type, ids[i], data, len, 4, 8, 320);
            }
        }
        free(data);
    }

    uint8_t frame[300];
    CHECK(synth_settings(frame, sizeof(frame)) == 300);
    jk_settings_t settings;
    CHECK(jk_decode_settings(JK_FRAME_JK02_32S, frame, 300, &settings) == 0);
    for (size_t len = 0; len < 300; len++) {
        uint8_t *short_frame = len ? malloc(len) : NULL;
        if (len) memcpy(short_frame, frame, len);
        CHECK(jk_decode_settings(JK_FRAME_JK02_32S, short_frame, len, &settings) < 0);
        free(short_frame);
    }
    uint8_t out[20];
    CHECK(jk_build_balance_write(JK_FRAME_JK02_32S, "balance_current", NAN, out, sizeof(out)) < 0);
    CHECK(jk_build_balance_write(JK_FRAME_JK02_32S, "balance_current", INFINITY, out, sizeof(out)) < 0);
    CHECK(jk_build_balance_write(JK_FRAME_JK02_32S, NULL, 1, out, sizeof(out)) < 0);
    CHECK(jk_build_balance_write(JK_FRAME_JK02_32S, "balance_current", 1, NULL, sizeof(out)) < 0);
    printf("PASS: %u assertions + 1,052,672 exact-buffer wire validator calls\n", checks);
    return 0;
}
