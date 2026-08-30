#ifndef HA_DISCOVERY_H
#define HA_DISCOVERY_H

#include "settings.h"
#include "sensors.h"
#include <esp_err.h>
#include <stdbool.h>

// Callback invoked when Home Assistant writes to a command entity's set topic.
// payload is NUL-terminated (len excludes the terminator); ctx is whatever was
// passed to ha_discovery_register_command_entity().
typedef void (*ha_command_cb_t)(const char *payload, int len, void *ctx);

// Initialize the discovery module. Safe to call even when discovery is disabled
// in settings; the on_* hooks below then become no-ops.
void ha_discovery_init(settings_t *settings);

// True when Home Assistant discovery is enabled in settings and a hostname is set.
bool ha_discovery_enabled(void);

// Retained availability topic ("<node>/availability"). Valid after
// ha_discovery_init(); returns "" when discovery is disabled.
const char *ha_discovery_availability_topic(void);

// Register a controllable entity (e.g. "number", "button"). config_extra_json is
// spliced verbatim into the discovery config object (no leading/trailing comma,
// no braces) and carries only the type-specific keys (min/max/unit,
// payload_press, ...). The command topic "<node>/<object_id>/set" is subscribed
// on every (re)connect and inbound messages are routed to cb. initial_state (may
// be NULL) seeds the retained state republished on each connect for non-button
// entities. Returns ESP_ERR_NO_MEM if the entity table is full.
esp_err_t ha_discovery_register_command_entity(const char *object_id,
                                               const char *component,
                                               const char *config_extra_json,
                                               const char *initial_state,
                                               ha_command_cb_t cb, void *ctx);

// Publish (retained) a new state for a registered command entity to
// "<node>/<object_id>/state" and remember it for reconnect republishing.
esp_err_t ha_discovery_publish_command_state(const char *object_id, const char *payload);

// --- Hooks called from mqtt_publisher.c ----------------------------------

// On MQTT_EVENT_CONNECTED: publish "online", (re)publish every discovery config,
// and (re)subscribe every command topic.
void ha_discovery_on_mqtt_connected(void);

// On MQTT_EVENT_DATA: route the message to a matching command entity.
void ha_discovery_on_mqtt_data(const char *topic, int topic_len,
                               const char *data, int data_len);

// Publish the bare value of one sensor to its discovery state topic. Called
// after the legacy per-sensor publish.
void ha_discovery_publish_sensor_state(const sensor_data_t *sensor);

// Publish wifi_rssi / uptime / free-heap diagnostic state values. Called from
// the periodic MQTT status task.
void ha_discovery_publish_diagnostics(void);

// Called periodically from the MQTT status task. Republishes sensor discovery
// configs when the sensor count has grown (e.g. a BTHome device first seen after
// the initial connect), so newly registered sensors show up in Home Assistant
// without waiting for an MQTT reconnect.
void ha_discovery_periodic(void);

#endif // HA_DISCOVERY_H
