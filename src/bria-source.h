#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *bria_source_getname(void *unused);
void *bria_source_create(obs_data_t *settings, obs_source_t *source);
void bria_source_destroy(void *data);
void bria_source_defaults(obs_data_t *settings);
obs_properties_t *bria_source_properties(void *data);
void bria_source_update(void *data, obs_data_t *settings);
void bria_source_activate(void *data);
void bria_source_deactivate(void *data);
void bria_source_video_tick(void *data, float seconds);
uint32_t bria_source_getwidth(void *data);
uint32_t bria_source_getheight(void *data);

#ifdef __cplusplus
}
#endif
