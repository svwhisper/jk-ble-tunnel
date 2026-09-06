#include <string.h>
#include "stream_start.h"

static bool possible_header(const stream_start_t *s)
{
    static const uint8_t magic[] = {0x55, 0xAA, 0xEB, 0x90};
    for (unsigned i = 0; i < s->len && i < sizeof(magic); i++)
        if (s->data[i] != magic[i]) return false;
    return s->len < 5 || (s->data[4] >= 1 && s->data[4] <= 3);
}

bool stream_start_push(stream_start_t *s, const uint8_t *data, uint16_t len,
                       uint16_t *consumed)
{
    *consumed = 0;
    while (*consumed < len) {
        s->data[s->len++] = data[(*consumed)++];
        if (s->len == sizeof(s->data)) {
            uint8_t sum = 0;
            for (unsigned i = 0; i + 1 < sizeof(s->data); i++) sum += s->data[i];
            if (sum == s->data[sizeof(s->data) - 1]) return true;
            /* Slide, don't discard the whole failed candidate: a genuine
             * header may be embedded in a corrupt/false-start candidate. */
            memmove(s->data, s->data + 1, --s->len);
        }
        while (s->len && !possible_header(s))
            memmove(s->data, s->data + 1, --s->len);
    }
    return false;
}
