#ifndef STUB_KMOD_H
#define STUB_KMOD_H
typedef int kern_return_t;
typedef kern_return_t kmod_start_func_t(struct kmod_info*, void*);
typedef kern_return_t kmod_stop_func_t(struct kmod_info*, void*);
typedef struct kmod_info { unsigned a; int ver; unsigned b; const char *name; const char *version; int c; unsigned d[4]; kmod_start_func_t *start; kmod_stop_func_t *stop; } kmod_info_t;
#define KMOD_INFO_VERSION 1
#define KERN_SUCCESS 0
#endif
