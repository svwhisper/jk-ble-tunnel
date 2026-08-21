#include "queues.h"
#include "na_types.h"

QueueHandle_t g_q_arb_in;
QueueHandle_t g_q_bms_request;
QueueHandle_t g_q_bms_response;
QueueHandle_t g_q_notify;
QueueHandle_t g_q_decode;
EventGroupHandle_t g_evt;

/* Sized for ~1 Hz per unit x 4 units with headroom. g_q_arb_in is created by
 * arbiter_start() where its private arb_msg_t size is known (item size must
 * match exactly, or xQueueReceive overruns the destination). */
void queues_init(void)
{
    g_q_bms_request  = xQueueCreate(12, sizeof(bms_request_t));
    g_q_bms_response = xQueueCreate(12, sizeof(bms_response_t));
    g_q_notify       = xQueueCreate(8,  sizeof(notify_item_t));
    g_q_decode       = xQueueCreate(8,  sizeof(notify_item_t));
    g_evt            = xEventGroupCreate();
}
