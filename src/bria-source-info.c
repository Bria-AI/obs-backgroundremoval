#include "bria-source.h"

struct obs_source_info bria_source_info = {
	.id = "bria_camera_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO,
	.get_name = bria_source_getname,
	.create = bria_source_create,
	.destroy = bria_source_destroy,
	.get_defaults = bria_source_defaults,
	.get_properties = bria_source_properties,
	.update = bria_source_update,
	.activate = bria_source_activate,
	.deactivate = bria_source_deactivate,
	.video_tick = bria_source_video_tick,
	.get_width = bria_source_getwidth,
	.get_height = bria_source_getheight,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};
