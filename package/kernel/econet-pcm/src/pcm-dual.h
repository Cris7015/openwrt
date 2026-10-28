/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XR500V_PCM_DUAL_H
#define XR500V_PCM_DUAL_H
#include <linux/types.h>
int pcm_en751221_dual_start(void);
void pcm_en751221_dual_stop(void);
int pcm_en751221_dual_open(unsigned int index);
void pcm_en751221_dual_close(unsigned int index);
/* Replace this line's captured audio with silence while the loop is open. */
void pcm_en751221_dual_mute(unsigned int index, bool mute);
/* Capture frames (10 ms each) the shared engine has completed since start. */
u32 pcm_en751221_dual_rx_frames(void);
ssize_t pcm_en751221_dual_io(unsigned int index, char __user *buf,
			   size_t len, bool capture);
/* Reads return capture + played-TX frame pairs (echo reference). */
int pcm_en751221_dual_set_ref(unsigned int index, bool on);
#endif
