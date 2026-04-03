#include "telemy_source.h"

#include <obs-module.h>
#include <obs-source.h>
#include <obs-data.h>
#include <obs-properties.h>
#include <string>

struct telemy_source_data {
    obs_source_t* source;
    obs_source_t* media_source;  // internal ffmpeg media source
    std::string connection_id;
    int srt_port;
    bool auto_reconnect;
    int reconnect_delay_ms;
};

static const char* telemy_source_get_name(void*) {
    return "Telemy";
}

static void telemy_source_update(void* data, obs_data_t* settings);

static void* telemy_source_create(obs_data_t* settings, obs_source_t* source) {
    auto* d = new telemy_source_data();
    d->source = source;
    d->media_source = nullptr;
    telemy_source_update(d, settings);
    return d;
}

static void telemy_source_destroy(void* data) {
    auto* d = static_cast<telemy_source_data*>(data);
    if (d->media_source) {
        obs_source_release(d->media_source);
    }
    delete d;
}

static void telemy_source_update(void* data, obs_data_t* settings) {
    auto* d = static_cast<telemy_source_data*>(data);
    d->connection_id = obs_data_get_string(settings, "connection_id");
    d->srt_port = (int)obs_data_get_int(settings, "srt_port");
    d->auto_reconnect = obs_data_get_bool(settings, "auto_reconnect");
    d->reconnect_delay_ms = (int)obs_data_get_int(settings, "reconnect_delay_ms");

    // Build SRT URL for local ingest
    std::string srt_url = "srt://127.0.0.1:" + std::to_string(d->srt_port) + "?mode=caller";

    // Create/update internal media source with SRT input
    if (d->media_source) {
        obs_data_t* ms = obs_source_get_settings(d->media_source);
        obs_data_set_string(ms, "local_file", srt_url.c_str());
        obs_data_set_bool(ms, "is_local_file", false);
        obs_data_set_string(ms, "input", srt_url.c_str());
        obs_data_set_string(ms, "input_format", "mpegts");
        obs_data_set_bool(ms, "restart_on_activate", true);
        obs_data_set_bool(ms, "close_when_inactive", false);
        obs_source_update(d->media_source, ms);
        obs_data_release(ms);
    } else {
        obs_data_t* ms = obs_data_create();
        obs_data_set_string(ms, "local_file", srt_url.c_str());
        obs_data_set_bool(ms, "is_local_file", false);
        obs_data_set_string(ms, "input", srt_url.c_str());
        obs_data_set_string(ms, "input_format", "mpegts");
        obs_data_set_bool(ms, "restart_on_activate", true);
        obs_data_set_bool(ms, "close_when_inactive", false);
        d->media_source = obs_source_create_private("ffmpeg_source", "telemy_internal", ms);
        obs_data_release(ms);
    }
}

static obs_properties_t* telemy_source_get_properties(void*) {
    obs_properties_t* props = obs_properties_create();

    // Relay selector dropdown — will be populated by dock/plugin
    obs_property_t* relay_list = obs_properties_add_list(props, "connection_id",
        "Relay", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    // Add a default entry
    obs_property_list_add_string(relay_list, "(Select a relay)", "");

    obs_properties_add_int(props, "srt_port", "SRT Port", 1024, 65535, 1);
    obs_properties_add_bool(props, "auto_reconnect", "Auto Reconnect");
    obs_properties_add_int(props, "reconnect_delay_ms", "Reconnect Delay (ms)", 500, 30000, 100);

    return props;
}

static void telemy_source_get_defaults(obs_data_t* settings) {
    obs_data_set_default_int(settings, "srt_port", 4001);
    obs_data_set_default_bool(settings, "auto_reconnect", true);
    obs_data_set_default_int(settings, "reconnect_delay_ms", 2000);
    obs_data_set_default_string(settings, "connection_id", "");
}

static void telemy_source_video_tick(void* data, float seconds) {
    (void)seconds;
    (void)data;
}

static void telemy_source_video_render(void* data, gs_effect_t* effect) {
    auto* d = static_cast<telemy_source_data*>(data);
    if (d->media_source) {
        obs_source_video_render(d->media_source);
    }
}

static uint32_t telemy_source_get_width(void* data) {
    auto* d = static_cast<telemy_source_data*>(data);
    return d->media_source ? obs_source_get_width(d->media_source) : 0;
}

static uint32_t telemy_source_get_height(void* data) {
    auto* d = static_cast<telemy_source_data*>(data);
    return d->media_source ? obs_source_get_height(d->media_source) : 0;
}

static void telemy_source_activate(void* data) {
    auto* d = static_cast<telemy_source_data*>(data);
    if (d->media_source) {
        obs_source_set_enabled(d->media_source, true);
    }
}

static void telemy_source_deactivate(void* data) {
    auto* d = static_cast<telemy_source_data*>(data);
    if (d->media_source) {
        obs_source_set_enabled(d->media_source, false);
    }
}

void telemy_source_register() {
    struct obs_source_info info = {};
    info.id = "telemy_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO |
                        OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = telemy_source_get_name;
    info.create = telemy_source_create;
    info.destroy = telemy_source_destroy;
    info.update = telemy_source_update;
    info.get_properties = telemy_source_get_properties;
    info.get_defaults = telemy_source_get_defaults;
    info.video_tick = telemy_source_video_tick;
    info.video_render = telemy_source_video_render;
    info.get_width = telemy_source_get_width;
    info.get_height = telemy_source_get_height;
    info.activate = telemy_source_activate;
    info.deactivate = telemy_source_deactivate;
    obs_register_source(&info);
}
