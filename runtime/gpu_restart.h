#ifndef WT_GPU_RESTART_H
#define WT_GPU_RESTART_H
#ifdef __ANDROID__
void wt_gpu_restart_enter(void);
void wt_gpu_restart_leave(void);
int wt_gpu_restart_prepare(unsigned timeout_ms);
void wt_gpu_restart_cancel(void);
#endif
#endif
