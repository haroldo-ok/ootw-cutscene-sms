#ifndef AWCAP_HOOKS_H
#define AWCAP_HOOKS_H

/* Capture instrumentation, called from the copied rawgl script.cpp. */
#ifdef __cplusplus
extern "C" {
#endif

void awcap_var_read(unsigned pc, int var, int kind); /* kind: 0=cond 1=arith 2=idx */
void awcap_note_pal(int num);
void awcap_play_sound(int res, int freq, int vol, int ch);
void awcap_play_music(int res, int delay, int pos);
void awcap_update_res(int num);
void awcap_install_task(int task, unsigned off);
void awcap_pre_display(int slices);

#ifdef __cplusplus
}
#endif

#endif
