#include "bria-filter.h"

struct obs_source_info bria_filter_info = {
	.id = "bria_background_removal",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = bria_filter_getname,
	.create = bria_filter_create,
	.destroy = bria_filter_destroy,
	.get_defaults = bria_filter_defaults,
	.get_properties = bria_filter_properties,
	.update = bria_filter_update,
	.activate = bria_filter_activate,
	.deactivate = bria_filter_deactivate,
	.video_tick = bria_filter_video_tick,
	.video_render = bria_filter_video_render,
};
