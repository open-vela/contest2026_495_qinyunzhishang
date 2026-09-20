#ifndef __BT_CALL_HANDLER_H__
#define __BT_CALL_HANDLER_H__

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BT_CALL_IDLE = 0,
    BT_CALL_INCOMING,
    BT_CALL_DIALING,
    BT_CALL_ALERTING,
    BT_CALL_ACTIVE,
    BT_CALL_HELD,
} bt_call_state_t;

int bt_call_handler_init(void);
void bt_call_handler_deinit(void);
bool bt_call_handler_is_available(void);
bt_call_state_t bt_call_handler_get_call_state(void);
const char* bt_call_handler_get_call_number(void);
const char* bt_call_handler_get_call_name(void);

int bt_call_accept(void);
int bt_call_reject(void);
int bt_call_terminate(void);

#endif