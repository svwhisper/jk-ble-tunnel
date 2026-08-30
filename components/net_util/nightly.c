/*
 * nightly.c — scheduled maintenance reboot (owner request 2026-08-31).
 * A day of accumulated NimBLE/session state was the root of several
 * 2026-08-30 mysteries (ghost connections, wedged adv sets); a nightly
 * reboot clears the slate. Fires once per night at the configured local
 * time. Guard: uptime must exceed 2 h (no boot loops; a recently rebooted
 * node skips that night). busy callback optional (NULL = unconditional,
 * owner choice 2026-08-31).
 */
#include <time.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "net_util.h"

static const char *TAG = "nightly";
static int  s_hour, s_min;
static bool (*s_busy)(void);

static void tick(void *arg)
{
    (void)arg;
    if (esp_timer_get_time() < 2LL * 3600 * 1000000) return;  /* uptime > 2 h */
    time_t now = time(NULL);
    if (now < 1600000000) return;                              /* SNTP not synced */
    struct tm lt; localtime_r(&now, &lt);
    if (lt.tm_hour != s_hour || lt.tm_min < s_min || lt.tm_min >= s_min + 5)
        return;                                                /* 5-min window */
    if (s_busy && s_busy()) {
        ESP_LOGW(TAG, "nightly reboot deferred — app session active");
        return;                                                /* retry next min */
    }
    ESP_LOGW(TAG, "nightly maintenance reboot (%02d:%02d)", s_hour, s_min);
    esp_restart();
}

void nightly_reboot_start(const char *tz, int hour, int min, bool (*busy)(void))
{
    setenv("TZ", tz, 1);
    tzset();
    s_hour = hour; s_min = min; s_busy = busy;
    const esp_timer_create_args_t a = { .callback = tick, .name = "nightly" };
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) == ESP_OK)
        esp_timer_start_periodic(t, 60LL * 1000000);           /* check every minute */
    ESP_LOGI(TAG, "nightly reboot armed for %02d:%02d local (%s)", hour, min, tz);
}
