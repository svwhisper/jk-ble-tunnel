#ifndef HOST_EVENT_GROUPS_H
#define HOST_EVENT_GROUPS_H
typedef void *EventGroupHandle_t;
typedef unsigned EventBits_t;
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
#endif
