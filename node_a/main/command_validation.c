#include "command_validation.h"
#include <math.h>
#include <string.h>

bool command_event_valid(bool retained, int offset, int total_len,
                         int data_len, int topic_len)
{
    return !retained && offset == 0 && total_len == data_len &&
           data_len > 0 && data_len < 192 && topic_len > 0 && topic_len < 64;
}

const char *balance_command_validate(const cJSON *root,
                                    const cJSON **setting, double *value)
{
    *setting = NULL;
    *value = 0;
    if (!cJSON_IsObject(root)) return "bad_json";
    bool have_id = false;
    for (const cJSON *it = root->child; it; it = it->next) {
        if (!it->string) return "bad_json";
        if (!strcmp(it->string, "id")) {
            if (have_id || !cJSON_IsString(it) || !it->valuestring ||
                strlen(it->valuestring) >= 32) return "bad_id";
            have_id = true;
        } else {
            if (*setting) return "one_key_per_command";
            *setting = it;
        }
    }
    if (!*setting) return "one_key_per_command";
    const cJSON *it = *setting;
    bool enabled = !strcmp(it->string, "balancing_enabled");
    if (enabled && cJSON_IsBool(it)) {
        *value = cJSON_IsTrue(it) ? 1.0 : 0.0;
    } else {
        if (!cJSON_IsNumber(it) || !isfinite(it->valuedouble)) return "bad_value";
        *value = it->valuedouble;
    }
    if (enabled && *value != 0.0 && *value != 1.0) return "bad_value";
    if (!strcmp(it->string, "cell_count") && trunc(*value) != *value)
        return "bad_value";
    return NULL;
}
