#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int source_application_os_init(void);
int source_application_os_poll(void);
int source_application_os_exit(void);
int source_application_os_is_new_3ds(void);
int source_application_os_is_old_3ds(void);
#ifdef __cplusplus
}
#endif
