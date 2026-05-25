#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *bria_filter_getname(void *unused);
void *bria_filter_create(obs_data_t *settings, obs_source_t *source);
void bria_filter_destroy(void *data);
void bria_filter_defaults(obs_data_t *settings);
obs_properties_t *bria_filter_properties(void *data);
void bria_filter_update(void *data, obs_data_t *settings);
void bria_filter_activate(void *data);
void bria_filter_deactivate(void *data);
void bria_filter_video_tick(void *data, float seconds);
void bria_filter_video_render(void *data, gs_effect_t *_effect);

#ifdef __cplusplus
}
#endif
