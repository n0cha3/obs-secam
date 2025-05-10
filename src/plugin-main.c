/*
obs-secam
Copyright (C) 2025 n0cha3

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>
#define LIBSECAM_IMPLEMENTATION
#define LIBSECAM_USE_THREADS
#include "libsecam/libsecam.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")


static const char *filter_name(void *data);
static void *filter_create(obs_data_t *settings, obs_source_t *source);
static void filter_destroy(void *data);
static uint32_t get_x(void *data);
static uint32_t get_y(void *data);
static void filter_defaults(obs_data_t *settings);
//static obs_properties_t *filter_props(void *data);
static void filter_update(void *data, obs_data_t *settings);
static void filter_render(void *data, gs_effect_t *effect);

typedef struct secam_filter_info {
	obs_source_t *source;
	gs_texture_t *dst;
	libsecam_t *secam_fire;
	libsecam_options_t *secam_fire_opt;
	uint8_t *image_buffer;
	uint32_t bufsize;
	gs_stagesurf_t *stag;
	gs_texrender_t *texdst;
	uint32_t x_targ, y_targ;
} secam_info;

struct obs_source_info secam_filter = {
	.id = "secam_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.create = filter_create,
	.destroy = filter_destroy,
	.update = filter_update,
	.video_render = filter_render,
	.get_defaults = filter_defaults,
	.get_name = filter_name
};

static const char *filter_name(void *data) {
	UNUSED_PARAMETER(data);
	return ("SECAM Fire");
}


static void *filter_create(obs_data_t *settings, obs_source_t *source) {
	UNUSED_PARAMETER(settings);
	secam_info *filter_info = bzalloc(sizeof(secam_info));
	filter_info->x_targ = obs_source_get_base_width(source);
	filter_info->y_targ = obs_source_get_base_height(source);
	filter_info->source = source;
	filter_info->dst = NULL; 
	filter_info->stag = NULL;
	filter_info->secam_fire = NULL;

	obs_enter_graphics();
	filter_info->texdst = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

	return filter_info;
}

static void filter_destroy(void *data) {
	secam_info *filterprops = data;
	obs_enter_graphics();
	gs_texrender_destroy(filterprops->texdst);
	obs_leave_graphics();

	if (filterprops->secam_fire != NULL) libsecam_close(filterprops->secam_fire);

	bfree(data);
}

static uint32_t get_x(void *data) {
	secam_info *filterprops = data;
	return filterprops->x_targ;
}

static uint32_t get_y(void *data) {
	secam_info *filterprops = data;
	return filterprops->y_targ;
}

static void filter_defaults(obs_data_t *settings) {
	UNUSED_PARAMETER(settings);
}


static void filter_update(void *data, obs_data_t *settings) {
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(data);
}


static void filter_render(void *data, gs_effect_t *effect) {
	UNUSED_PARAMETER(effect);

	secam_info *filterprops = data;
	
	if (!filterprops->source) return;

	filterprops->image_buffer = NULL;
	
	obs_source_t *target = obs_filter_get_target(filterprops->source), 
		*parent = obs_filter_get_parent(filterprops->source);

	if (parent == NULL) {
		obs_source_skip_video_filter(filterprops->source);
		return;
	}

	uint32_t x_rend = obs_source_get_base_width(filterprops->source);
	uint32_t y_rend = obs_source_get_base_height(filterprops->source);
	
	if (filterprops->secam_fire == NULL) {
		filterprops->secam_fire = libsecam_init(filterprops->x_targ, filterprops->y_targ);
		filterprops->secam_fire_opt = libsecam_options(filterprops->secam_fire);
	}

	if (x_rend != filterprops->x_targ || y_rend != filterprops->y_targ) {
		filterprops->image_buffer = NULL;
	    filterprops->x_targ = x_rend;
	    filterprops->y_targ = y_rend;

		if (filterprops->secam_fire == NULL) {
			filterprops->secam_fire = libsecam_init(filterprops->x_targ, filterprops->y_targ);
			filterprops->secam_fire_opt = libsecam_options(filterprops->secam_fire);
		}

		else {
			libsecam_close(filterprops->secam_fire);
			filterprops->secam_fire = libsecam_init(filterprops->x_targ, filterprops->y_targ);
			filterprops->secam_fire_opt = libsecam_options(filterprops->secam_fire);
		}
	}

	filterprops->secam_fire_opt->chroma_fire = 0.3;
	filterprops->secam_fire_opt->chroma_noise = 0.0;
	filterprops->secam_fire_opt->luma_noise = 0.0;
	filterprops->secam_fire_opt->echo = 0;
	filterprops->secam_fire_opt->skew = 3;
	filterprops->secam_fire_opt->wobble = 90;


	gs_texrender_reset(filterprops->texdst);

	gs_blend_state_push();

	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(filterprops->texdst, filterprops->x_targ, filterprops->y_targ)) {
		uint32_t source_params = obs_source_get_output_flags(target);
		bool custom_draw = (source_params & OBS_SOURCE_CUSTOM_DRAW),
		async_draw = (source_params & OBS_SOURCE_ASYNC);

		struct vec4 col;

		vec4_zero(&col);
		gs_clear(GS_CLEAR_COLOR, &col, 0.0f, 0);
		gs_ortho(0.0f, (float)filterprops->x_targ, 0.0f, (float)filterprops->y_targ, -100.0f, 100.0f);

		if (target == parent && !custom_draw && !async_draw)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_texrender_end(filterprops->texdst);
		
	}
	gs_blend_state_pop();

	gs_texture_t *srctexture = gs_texrender_get_texture(filterprops->texdst);

	gs_effect_t *source_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *eff_eparam = gs_effect_get_param_by_name(source_effect, "image");

	if (srctexture) {
		uint8_t *pix_data = NULL;
		gs_texture_t *src2 = NULL;

		obs_enter_graphics();
		if (filterprops->stag != NULL) gs_stagesurface_destroy(filterprops->stag);
			filterprops->stag = gs_stagesurface_create(filterprops->x_targ, filterprops->y_targ, GS_RGBA);
		gs_stage_texture(filterprops->stag, srctexture);
		obs_leave_graphics();	

		if (gs_stagesurface_map(filterprops->stag, &filterprops->image_buffer, &filterprops->bufsize)) {
			size_t buffersize = filterprops->y_targ * filterprops->bufsize;

			pix_data = bzalloc(buffersize);

			libsecam_filter_to_buffer(filterprops->secam_fire, filterprops->image_buffer, pix_data);

			obs_enter_graphics();
			src2 = gs_texture_create(filterprops->x_targ, filterprops->y_targ, GS_RGBA, 1, &pix_data, 0);
			obs_leave_graphics();
						
			bfree(pix_data);

			gs_stagesurface_unmap(filterprops->stag);
		}

		while (gs_effect_loop(source_effect, "Draw"))
			obs_source_draw(src2, 0, 0, filterprops->x_targ, filterprops->y_targ, false);

			obs_enter_graphics();
			gs_texture_destroy(src2);
			obs_leave_graphics();
	}
}

bool obs_module_load(void) {
	obs_log(LOG_INFO, "SECAM plugin loaded successfully (version %s)", PLUGIN_VERSION);
	obs_register_source(&secam_filter);
	return true;
}

void obs_module_unload(void) {
	obs_log(LOG_INFO, "SECAM plugin unloaded");
}
