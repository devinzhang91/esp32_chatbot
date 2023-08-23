#ifndef MAIN_WWE_WORK_H_
#define MAIN_WWE_WORK_H_

#include "board.h"
#include "esp_audio.h"
#include "audio_recorder.h"

void init_wwe_work();

void enable_wwe_pipeline(bool enable);
void enable_wwe_trigger(bool enable);

#endif /* MAIN_WWE_WORK_H_ */
