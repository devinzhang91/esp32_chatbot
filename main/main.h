
#ifndef MAIN_H_
#define MAIN_H_




#include "esp_log.h"
#include "pipline_work.h"
#include "wwe_work.h"
#include "wifi_work.h"


enum _main_msg_id {
    FILE2HTTP = 1,
    HTTP2FILE,
    FILE2PLAYER,
    HTTP2PLAYER,
    EXIT
};

typedef struct {
	int				msg_id;
    char			*msg;
    char			*src;
    char			*dst;
} main_msg_t;

extern QueueHandle_t main_q;


#endif /* MAIN_H_ */
