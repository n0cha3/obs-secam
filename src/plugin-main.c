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
obs_properties_t *filter_properties(void *data);
static void filter_update(void *data, obs_data_t *settings);
static void filter_render(void *data, gs_effect_t *effect);

typedef struct secam_filter_info {
	obs_source_t *source;
	libsecam_t *secam_fire;
	libsecam_options_t *secam_fire_opt;
	gs_texrender_t *texdst;
	uint32_t x_targ, y_targ;
	libsecam_options_t load_secam_options;
} secam_info;

struct obs_source_info secam_filter = {
	.id = "secam_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.create = filter_create,
	.destroy = filter_destroy,
	.update = filter_update,
	.video_render = filter_render,
	.get_properties = filter_properties,
	.get_defaults = filter_defaults,
	.get_name = filter_name
};

static const char *filter_name(void *data) {
	UNUSED_PARAMETER(data);
	return ("SECAM Fire");
}

obs_properties_t *filter_properties(void *data) {
	UNUSED_PARAMETER(data);

	obs_properties_t *properties = obs_properties_create();

	obs_properties_add_float_slider(properties, "Luma", "Luma noise", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(properties, "Chroma_noise", "Chroma noise", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(properties, "Chroma_fire", "Chroma fire", 0.0, 1.0, 0.01);
	obs_properties_add_int_slider(properties, "Echo", "Echo", 0, 100, 1);
	obs_properties_add_int_slider(properties, "Skew", "Skew", 0, 100, 1);
	obs_properties_add_int_slider(properties, "Wobble", "Wobble", 0, 100, 1); 

	return properties;
}

static void *filter_create(obs_data_t *settings, obs_source_t *source) {
	secam_info *filter_info = bzalloc(sizeof(secam_info));

	filter_info->x_targ = obs_source_get_base_width(source);
	filter_info->y_targ = obs_source_get_base_height(source);
	filter_info->source = source;
	filter_info->secam_fire = NULL;
	
	filter_info->load_secam_options.chroma_fire = obs_data_get_double(settings, "Chroma_fire");
	filter_info->load_secam_options.chroma_noise = obs_data_get_double(settings, "Chroma_noise");
	filter_info->load_secam_options.luma_noise = obs_data_get_double(settings, "Luma");
	filter_info->load_secam_options.echo = (int32_t)obs_data_get_int(settings, "Echo");
	filter_info->load_secam_options.skew = (int32_t)obs_data_get_int(settings, "Skew");
	filter_info->load_secam_options.wobble = (int32_t)obs_data_get_int(settings, "Wobble");

	obs_enter_graphics();
	filter_info->texdst = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

	return filter_info;
}

static void filter_destroy(void *data) {
	secam_info *filter_info = data;
	obs_enter_graphics();
	gs_texrender_destroy(filter_info->texdst);
	obs_leave_graphics();

	if (filter_info->secam_fire != NULL) libsecam_close(filter_info->secam_fire);

	bfree(data);
}

static uint32_t get_x(void *data) {
	secam_info *filter_info = data;
	return filter_info->x_targ;
}

static uint32_t get_y(void *data) {
	secam_info *filter_info = data;
	return filter_info->y_targ;
}

static void filter_defaults(obs_data_t *settings) {
	obs_data_set_default_double(settings, "Chroma_fire", 0.0);
	obs_data_set_default_double(settings, "Chroma_noise", 0.0);
	obs_data_set_default_double(settings, "Luma", 0.0);
	obs_data_set_default_int(settings, "Echo", 0);
	obs_data_set_default_int(settings, "Skew", 0);
	obs_data_set_default_int(settings, "Wobble", 0);
}

static void filter_update(void *data, obs_data_t *settings) {
	secam_info *filter_info = data;

	if (filter_info->secam_fire_opt != NULL) {
		filter_info->load_secam_options.chroma_fire = obs_data_get_double(settings, "Chroma_fire");
		filter_info->load_secam_options.chroma_noise = obs_data_get_double(settings, "Chroma_noise");
		filter_info->load_secam_options.luma_noise = obs_data_get_double(settings, "Luma");
		filter_info->load_secam_options.echo = (int32_t)obs_data_get_int(settings, "Echo");
		filter_info->load_secam_options.skew = (int32_t)obs_data_get_int(settings, "Skew");
		filter_info->load_secam_options.wobble = (int32_t)obs_data_get_int(settings, "Wobble");
	}
}

static void update_options(secam_info *filter_info) {
	if (filter_info->secam_fire_opt != NULL) {
		filter_info->secam_fire_opt->chroma_fire = filter_info->load_secam_options.chroma_fire;
		filter_info->secam_fire_opt->chroma_noise = filter_info->load_secam_options.chroma_noise;
		filter_info->secam_fire_opt->luma_noise = filter_info->load_secam_options.luma_noise;
		filter_info->secam_fire_opt->echo = filter_info->load_secam_options.echo;
		filter_info->secam_fire_opt->wobble = filter_info->load_secam_options.wobble;
		filter_info->secam_fire_opt->skew = filter_info->load_secam_options.skew;
	}
}

static void filter_render(void *data, gs_effect_t *effect) {
	UNUSED_PARAMETER(effect);

	secam_info *filter_info = data;

	if (filter_info->source == NULL) return;

	uint8_t *source_buffer = NULL;
	uint32_t source_buf_pitch;
	
	gs_stagesurf_t *source_stage = NULL;

	obs_source_t *target = obs_filter_get_target(filter_info->source), 
		*parent = obs_filter_get_parent(filter_info->source);

	if (parent == NULL || target == NULL) {
		obs_source_skip_video_filter(filter_info->source);
		return;
	}

	uint32_t x_rend = obs_source_get_base_width(filter_info->source);
	uint32_t y_rend = obs_source_get_base_height(filter_info->source);
	
	if (filter_info->secam_fire == NULL) {
		filter_info->secam_fire = libsecam_init(filter_info->x_targ, filter_info->y_targ);
		filter_info->secam_fire_opt = libsecam_options(filter_info->secam_fire);
	}

	if (x_rend != filter_info->x_targ || y_rend != filter_info->y_targ) {
	    filter_info->x_targ = x_rend;
	    filter_info->y_targ = y_rend;

		if (filter_info->secam_fire != NULL) libsecam_close(filter_info->secam_fire);

		filter_info->secam_fire = libsecam_init(filter_info->x_targ, filter_info->y_targ);
		filter_info->secam_fire_opt = libsecam_options(filter_info->secam_fire);
	}

	update_options(filter_info);
	
	gs_texrender_reset(filter_info->texdst);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(filter_info->texdst, filter_info->x_targ, filter_info->y_targ)) {
		uint32_t source_params = obs_source_get_output_flags(target);
		bool custom_draw = (source_params & OBS_SOURCE_CUSTOM_DRAW),
		async_draw = (source_params & OBS_SOURCE_ASYNC);

		struct vec4 col;

		vec4_zero(&col);
		gs_clear(GS_CLEAR_COLOR, &col, 0.0f, 0);
		gs_ortho(0.0f, (float)filter_info->x_targ, 0.0f, (float)filter_info->y_targ, -100.0f, 100.0f);


		if (target == parent && !custom_draw && !async_draw)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_texrender_end(filter_info->texdst);	
	}

	gs_blend_state_pop();

	gs_texture_t *source_texture = gs_texrender_get_texture(filter_info->texdst);

	gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_get_param_by_name(default_effect, "image");

	if (source_texture) {
		uint8_t *target_buffer = NULL;
		gs_texture_t *target_texture = NULL;
		obs_enter_graphics();
		
		source_stage = gs_stagesurface_create(filter_info->x_targ, filter_info->y_targ, GS_RGBA);
		
		if (source_stage == NULL) {
			obs_leave_graphics();
			obs_source_skip_video_filter(filter_info->source);
			return;
		}

		gs_stage_texture(source_stage, source_texture);
		obs_leave_graphics();	

		if (gs_stagesurface_map(source_stage, &source_buffer, &source_buf_pitch)) {
			size_t buffer_size = filter_info->y_targ * source_buf_pitch;

			target_buffer = bzalloc(buffer_size);

			libsecam_filter_to_buffer(filter_info->secam_fire, source_buffer, target_buffer);

			obs_enter_graphics();
			target_texture = gs_texture_create(filter_info->x_targ, filter_info->y_targ, GS_RGBA, 1, (const uint8_t**)&target_buffer, 0);
			obs_leave_graphics();
						
			bfree(target_buffer);

			gs_stagesurface_unmap(source_stage);
			gs_stagesurface_destroy(source_stage);
		}

		while (gs_effect_loop(default_effect, "Draw"))
			obs_source_draw(target_texture, 0, 0, filter_info->x_targ, filter_info->y_targ, false);

			obs_enter_graphics();
			gs_texture_destroy(target_texture);
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
