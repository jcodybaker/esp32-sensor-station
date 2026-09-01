#include "ha_discovery.h"
#include "mqtt_publisher.h"
#include "sensors.h"
#include "wifi.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "ha_discovery";

// esp-mqtt copies payloads into its outbox, so all the buffers below can be
// stack/static locals. Discovery config JSON with a full device block runs
// ~700 bytes worst case.
#define HA_JSON_BUF_SIZE 1024
#define HA_TOPIC_BUF_SIZE 192
#define HA_NODE_MAX 64
#define HA_OBJECT_ID_MAX 96
#define HA_MAX_CMD_ENTITIES 8
#define HA_CMD_STATE_MAX 24

typedef struct {
    bool used;
    char object_id[40];
    char component[16];
    char config_extra[224];
    char state[HA_CMD_STATE_MAX];
    ha_command_cb_t cb;
    void *ctx;
} ha_cmd_entity_t;

static settings_t *s_settings = NULL;
static char s_node[HA_NODE_MAX] = "";
static char s_availability_topic[HA_TOPIC_BUF_SIZE] = "";
static char s_fw_version[32] = "";
static ha_cmd_entity_t s_cmd_entities[HA_MAX_CMD_ENTITIES];
// Sensor count at the last full config publish; -1 forces a republish.
static int s_published_sensor_count = -1;

// Lowercase src into dst, replacing anything outside [a-z0-9_-] with '_'.
static void sanitize(char *dst, size_t dst_size, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src != NULL && src[i] != '\0' && j + 1 < dst_size; i++) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            dst[j++] = c;
        } else {
            dst[j++] = '_';
        }
    }
    dst[j] = '\0';
}

void ha_discovery_init(settings_t *settings) {
    s_settings = settings;
    memset(s_cmd_entities, 0, sizeof(s_cmd_entities));

    const char *hostname = (settings != NULL && settings->hostname != NULL && settings->hostname[0] != '\0')
                               ? settings->hostname : "esp32-sensor-station";
    sanitize(s_node, sizeof(s_node), hostname);
    snprintf(s_availability_topic, sizeof(s_availability_topic), "%s/availability", s_node);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    strncpy(s_fw_version, app_desc->version, sizeof(s_fw_version) - 1);
    s_fw_version[sizeof(s_fw_version) - 1] = '\0';

    ESP_LOGI(TAG, "Initialized (node='%s', enabled=%d)", s_node, ha_discovery_enabled());
}

bool ha_discovery_enabled(void) {
    return s_settings != NULL && s_settings->ha_discovery_enabled && s_node[0] != '\0';
}

const char *ha_discovery_availability_topic(void) {
    return ha_discovery_enabled() ? s_availability_topic : "";
}

static const char *ha_prefix(void) {
    if (s_settings != NULL && s_settings->ha_discovery_prefix != NULL &&
        s_settings->ha_discovery_prefix[0] != '\0') {
        return s_settings->ha_discovery_prefix;
    }
    return "homeassistant";
}

// object_id = "<base>" or "<base>_<device_id>" (both sanitized), the latter for
// sensors that share a base across devices (BTHome beacons, DS18B20 probes).
// base is the metric_name when set, otherwise the display_name -- a
// display-only reading (e.g. a Fahrenheit temperature whose Celsius shadow
// carries the Prometheus metric) still needs a stable topic/uniq_id.
static void object_id_for_sensor(char *dst, size_t dst_size, const sensor_data_t *sensor) {
    // sanitize() is length-preserving, so this mirrors the source field size.
    char base[SENSOR_DISPLAY_NAME_MAX_LEN];
    sanitize(base, sizeof(base),
             sensor->metric_name[0] != '\0' ? sensor->metric_name : sensor->display_name);
    if (sensor->device_id[0] != '\0') {
        char dev[SENSOR_DEVICE_ID_MAX_LEN];
        sanitize(dev, sizeof(dev), sensor->device_id);
        snprintf(dst, dst_size, "%s_%s", base, dev);
    } else {
        snprintf(dst, dst_size, "%s", base);
    }
}

// Discovery node segment / uniq_id + topic prefix for a sensor. Station-owned
// sensors are namespaced under the station's node so two stations never collide;
// external sensors (BTHome beacons) use a fixed "bthome" namespace plus the
// beacon's device_id, so every station that hears the same beacon publishes the
// identical retained config and state topic and Home Assistant sees one device.
static const char *entity_ns(const sensor_data_t *sensor) {
    return (sensor != NULL && sensor->external) ? "bthome" : s_node;
}

// Home Assistant marks an external sensor's entity unavailable if no station
// reports a fresh value within this many seconds (matches the sensor registry's
// stale timeout with headroom). Station-owned entities use the MQTT LWT instead.
#define HA_EXTERNAL_EXPIRE_AFTER_S 900

// Maps a sensor's unit / metric_name to a Home Assistant device_class and
// state_class. Returns the device_class (or NULL) and writes the state_class.
// *unit_out is set to the unit string to advertise: HA silently drops a
// discovery config whose unit_of_measurement is not valid for its device_class
// (e.g. "ml" vs "mL", "C" vs "°C"), so classes that need it get a
// normalized unit here; otherwise the sensor's raw unit is passed through.
#define DEG "\xC2\xB0" // UTF-8 degree sign
static const char *classify(const sensor_data_t *sensor, const char **state_class_out,
                            const char **unit_out) {
    const char *unit = sensor->unit;
    const char *metric = sensor->metric_name;
    *state_class_out = "measurement";
    *unit_out = unit;

    if (strcmp(unit, "C") == 0 || strcmp(unit, "c") == 0 || strcmp(unit, DEG "C") == 0) {
        *unit_out = DEG "C";
        return "temperature";
    }
    if (strcmp(unit, "F") == 0 || strcmp(unit, "f") == 0 || strcmp(unit, DEG "F") == 0) {
        *unit_out = DEG "F";
        return "temperature";
    }
    if (strcmp(unit, "%") == 0) {
        if (strstr(metric, "batt") != NULL) return "battery";
        if (strstr(metric, "humid") != NULL) return "humidity";
        return NULL;
    }
    if (strcmp(unit, "V") == 0) return "voltage";
    if (strcmp(unit, "g") == 0) return "weight";
    if (strcmp(unit, "kg") == 0) return "weight";
    if (strcmp(unit, "cm") == 0 || strcmp(unit, "mm") == 0 || strcmp(unit, "km") == 0) return "distance";
    if (strcmp(unit, "dBm") == 0) return "signal_strength";
    if (strcmp(unit, "ml") == 0 || strcmp(unit, "mL") == 0) {
        *unit_out = "mL";
        return "volume";
    }
    if (strcmp(unit, "L") == 0) {
        *state_class_out = "total_increasing";
        return "water";
    }
    if (strcmp(unit, "gal") == 0) {
        *unit_out = "gal";
        *state_class_out = "total_increasing";
        return "water";
    }
    return NULL;
}

static esp_err_t ha_publish_config(const char *component, const char *node,
                                   const char *object_id, const char *json, int json_len) {
    char topic[HA_TOPIC_BUF_SIZE];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", ha_prefix(), component, node, object_id);
    // QoS 0: these are retained and republished on every connect, and a burst of
    // ~20 of them at QoS 1 would sit in the MQTT outbox awaiting PUBACK.
    return mqtt_publisher_publish(topic, json, json_len, 0, true);
}

// Appends the "dev" object. External sensors (BTHome beacons) get a
// station-independent device keyed on the beacon id, so multiple observing
// stations converge on one Home Assistant device. Other sensors with a
// device_id (DS18B20 probes, flow meters) get their own child device linked to
// the station via "via_device"; everything else attaches to the station device.
static int append_device_block(char *buf, int offset, int size, const sensor_data_t *sensor) {
    if (sensor != NULL && sensor->device_id[0] != '\0') {
        char dev[SENSOR_DEVICE_ID_MAX_LEN * 2];
        sanitize(dev, sizeof(dev), sensor->device_id);
        const char *name = sensor->device_name[0] != '\0' ? sensor->device_name : sensor->device_id;
        if (sensor->external) {
            return snprintf(buf + offset, size - offset,
                            "\"dev\":{\"ids\":[\"bthome_%s\"],\"name\":\"%s\"}", dev, name);
        }
        return snprintf(buf + offset, size - offset,
                        "\"dev\":{\"ids\":[\"%s_%s\"],\"name\":\"%s\",\"via_device\":\"%s\"}",
                        s_node, dev, name, s_node);
    }
    const char *hostname = (s_settings != NULL && s_settings->hostname != NULL && s_settings->hostname[0] != '\0')
                               ? s_settings->hostname : s_node;
    return snprintf(buf + offset, size - offset,
                    "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mdl\":\"esp32-sensor-station\",\"sw\":\"%s\"}",
                    s_node, hostname, s_fw_version);
}

static void publish_sensor_config(const sensor_data_t *sensor) {
    char object_id[HA_OBJECT_ID_MAX];
    object_id_for_sensor(object_id, sizeof(object_id), sensor);

    const char *state_class = NULL;
    const char *unit = NULL;
    const char *device_class = classify(sensor, &state_class, &unit);
    const char *ns = entity_ns(sensor);

    char json[HA_JSON_BUF_SIZE];
    int n = 0;
    n += snprintf(json + n, sizeof(json) - n,
                  "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s/%s/state\"",
                  sensor->display_name, ns, object_id, ns, object_id);
    if (sensor->external) {
        n += snprintf(json + n, sizeof(json) - n, ",\"exp_aft\":%d", HA_EXTERNAL_EXPIRE_AFTER_S);
    } else {
        n += snprintf(json + n, sizeof(json) - n, ",\"avty_t\":\"%s\"", s_availability_topic);
    }
    if (unit != NULL && unit[0] != '\0') {
        n += snprintf(json + n, sizeof(json) - n, ",\"unit_of_meas\":\"%s\"", unit);
    }
    if (device_class != NULL) {
        n += snprintf(json + n, sizeof(json) - n, ",\"dev_cla\":\"%s\"", device_class);
    }
    n += snprintf(json + n, sizeof(json) - n, ",\"stat_cla\":\"%s\",", state_class);
    n += append_device_block(json, n, sizeof(json), sensor);
    n += snprintf(json + n, sizeof(json) - n, "}");

    if (n >= (int)sizeof(json)) {
        ESP_LOGW(TAG, "sensor '%s' config truncated, skipping", sensor->metric_name);
        return;
    }
    ha_publish_config("sensor", entity_ns(sensor), object_id, json, n);
}

// A fixed diagnostic sensor attached to the station device.
static void publish_diag_config(const char *object_id, const char *name,
                                const char *unit, const char *device_class) {
    char json[HA_JSON_BUF_SIZE];
    int n = 0;
    n += snprintf(json + n, sizeof(json) - n,
                  "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s/%s/state\","
                  "\"avty_t\":\"%s\",\"ent_cat\":\"diagnostic\",\"stat_cla\":\"measurement\"",
                  name, s_node, object_id, s_node, object_id, s_availability_topic);
    if (unit[0] != '\0') {
        n += snprintf(json + n, sizeof(json) - n, ",\"unit_of_meas\":\"%s\"", unit);
    }
    if (device_class[0] != '\0') {
        n += snprintf(json + n, sizeof(json) - n, ",\"dev_cla\":\"%s\"", device_class);
    }
    n += snprintf(json + n, sizeof(json) - n, ",");
    n += append_device_block(json, n, sizeof(json), NULL);
    n += snprintf(json + n, sizeof(json) - n, "}");
    ha_publish_config("sensor", s_node, object_id, json, n);
}

static void publish_cmd_config(const ha_cmd_entity_t *e) {
    bool is_button = strcmp(e->component, "button") == 0;
    char json[HA_JSON_BUF_SIZE];
    int n = 0;
    n += snprintf(json + n, sizeof(json) - n,
                  "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"cmd_t\":\"%s/%s/set\",\"avty_t\":\"%s\"",
                  e->object_id, s_node, e->object_id, s_node, e->object_id, s_availability_topic);
    if (!is_button) {
        n += snprintf(json + n, sizeof(json) - n, ",\"stat_t\":\"%s/%s/state\"", s_node, e->object_id);
    }
    if (e->config_extra[0] != '\0') {
        n += snprintf(json + n, sizeof(json) - n, ",%s", e->config_extra);
    }
    n += snprintf(json + n, sizeof(json) - n, ",");
    n += append_device_block(json, n, sizeof(json), NULL);
    n += snprintf(json + n, sizeof(json) - n, "}");

    if (n >= (int)sizeof(json)) {
        ESP_LOGW(TAG, "command entity '%s' config truncated, skipping", e->object_id);
        return;
    }
    ha_publish_config(e->component, s_node, e->object_id, json, n);
}

// Publishes config + subscribes the set topic + (re)publishes stored state for
// one command entity. Safe to call only while MQTT is connected.
static void publish_cmd_entity(const ha_cmd_entity_t *e) {
    publish_cmd_config(e);

    char topic[HA_TOPIC_BUF_SIZE];
    snprintf(topic, sizeof(topic), "%s/%s/set", s_node, e->object_id);
    mqtt_publisher_subscribe(topic, 1);

    if (e->state[0] != '\0' && strcmp(e->component, "button") != 0) {
        char st[HA_TOPIC_BUF_SIZE];
        snprintf(st, sizeof(st), "%s/%s/state", s_node, e->object_id);
        mqtt_publisher_publish(st, e->state, strlen(e->state), 0, true);
    }
}

static void publish_all_sensor_configs(void) {
    int count = sensors_get_count();
    for (int i = 0; i < count; i++) {
        const sensor_data_t *sensor = sensors_get_by_index(i);
        // Needs a display_name to be a Home Assistant entity, plus something to
        // build a stable object_id from: a metric_name, or a device_id (the
        // case for a display-only Fahrenheit reading whose Celsius shadow holds
        // the Prometheus metric).
        if (sensor == NULL || sensor->display_name[0] == '\0' ||
            (sensor->metric_name[0] == '\0' && sensor->device_id[0] == '\0')) {
            continue;
        }
        publish_sensor_config(sensor);
    }
    s_published_sensor_count = count;
}

esp_err_t ha_discovery_register_command_entity(const char *object_id,
                                               const char *component,
                                               const char *config_extra_json,
                                               const char *initial_state,
                                               ha_command_cb_t cb, void *ctx) {
    for (int i = 0; i < HA_MAX_CMD_ENTITIES; i++) {
        ha_cmd_entity_t *e = &s_cmd_entities[i];
        if (e->used) {
            continue;
        }
        e->used = true;
        strncpy(e->object_id, object_id, sizeof(e->object_id) - 1);
        e->object_id[sizeof(e->object_id) - 1] = '\0';
        strncpy(e->component, component, sizeof(e->component) - 1);
        e->component[sizeof(e->component) - 1] = '\0';
        if (config_extra_json != NULL) {
            strncpy(e->config_extra, config_extra_json, sizeof(e->config_extra) - 1);
            e->config_extra[sizeof(e->config_extra) - 1] = '\0';
        }
        if (initial_state != NULL) {
            strncpy(e->state, initial_state, sizeof(e->state) - 1);
            e->state[sizeof(e->state) - 1] = '\0';
        }
        e->cb = cb;
        e->ctx = ctx;
        ESP_LOGI(TAG, "Registered command entity '%s' (%s)", e->object_id, e->component);

        // If MQTT is already up (this entity was registered after the initial
        // connect), publish/subscribe now; otherwise the connect hook handles it.
        if (ha_discovery_enabled() && mqtt_is_enabled()) {
            publish_cmd_entity(e);
        }
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Command entity table full, dropping '%s'", object_id);
    return ESP_ERR_NO_MEM;
}

esp_err_t ha_discovery_publish_command_state(const char *object_id, const char *payload) {
    if (!ha_discovery_enabled()) {
        return ESP_FAIL;
    }
    for (int i = 0; i < HA_MAX_CMD_ENTITIES; i++) {
        ha_cmd_entity_t *e = &s_cmd_entities[i];
        if (e->used && strcmp(e->object_id, object_id) == 0) {
            strncpy(e->state, payload, sizeof(e->state) - 1);
            e->state[sizeof(e->state) - 1] = '\0';
            break;
        }
    }
    char topic[HA_TOPIC_BUF_SIZE];
    snprintf(topic, sizeof(topic), "%s/%s/state", s_node, object_id);
    return mqtt_publisher_publish(topic, payload, strlen(payload), 0, true);
}

void ha_discovery_on_mqtt_connected(void) {
    if (!ha_discovery_enabled()) {
        return;
    }

    // QoS 0 (see mqtt_publisher_publish): the retained LWT paired with this is
    // still QoS 1, configured on the client in mqtt_publisher_init().
    mqtt_publisher_publish(s_availability_topic, "online", 6, 0, true);

    publish_all_sensor_configs();

    publish_diag_config("wifi_rssi", "WiFi RSSI", "dBm", "signal_strength");
    publish_diag_config("uptime", "Uptime", "s", "duration");
    publish_diag_config("heap_free", "Free heap", "B", "data_size");

    for (int i = 0; i < HA_MAX_CMD_ENTITIES; i++) {
        if (s_cmd_entities[i].used) {
            publish_cmd_entity(&s_cmd_entities[i]);
        }
    }

    ESP_LOGI(TAG, "Published discovery for %d sensors + diagnostics + command entities",
             s_published_sensor_count);
}

void ha_discovery_periodic(void) {
    if (!ha_discovery_enabled() || !mqtt_is_enabled()) {
        return;
    }
    if (sensors_get_count() != s_published_sensor_count) {
        ESP_LOGI(TAG, "Sensor count changed (%d -> %d), republishing discovery configs",
                 s_published_sensor_count, sensors_get_count());
        publish_all_sensor_configs();
    }
}

void ha_discovery_on_mqtt_data(const char *topic, int topic_len,
                               const char *data, int data_len) {
    if (!ha_discovery_enabled() || topic == NULL || topic_len <= 0) {
        return;
    }
    char t[HA_TOPIC_BUF_SIZE];
    if (topic_len >= (int)sizeof(t)) {
        return;
    }
    memcpy(t, topic, topic_len);
    t[topic_len] = '\0';

    for (int i = 0; i < HA_MAX_CMD_ENTITIES; i++) {
        ha_cmd_entity_t *e = &s_cmd_entities[i];
        if (!e->used || e->cb == NULL) {
            continue;
        }
        char want[HA_TOPIC_BUF_SIZE];
        snprintf(want, sizeof(want), "%s/%s/set", s_node, e->object_id);
        if (strcmp(t, want) != 0) {
            continue;
        }
        char payload[64];
        int n = (data_len < (int)sizeof(payload) - 1) ? data_len : (int)sizeof(payload) - 1;
        if (n < 0) {
            n = 0;
        }
        if (n > 0 && data != NULL) {
            memcpy(payload, data, n);
        }
        payload[n] = '\0';
        ESP_LOGI(TAG, "Command '%s' <- '%s'", e->object_id, payload);
        e->cb(payload, n, e->ctx);
        return;
    }
}

void ha_discovery_publish_sensor_state(const sensor_data_t *sensor) {
    if (!ha_discovery_enabled() || sensor == NULL) {
        return;
    }
    // Mirror the publish_all_sensor_configs() filter: only sensors that also get
    // a discovery config. A display_name-less sensor like load_cell_raw or a
    // Celsius Prometheus shadow is internal-only.
    if (sensor->display_name[0] == '\0' || !sensor->available ||
        (sensor->metric_name[0] == '\0' && sensor->device_id[0] == '\0')) {
        return;
    }
    char object_id[HA_OBJECT_ID_MAX];
    object_id_for_sensor(object_id, sizeof(object_id), sensor);

    char topic[HA_TOPIC_BUF_SIZE];
    snprintf(topic, sizeof(topic), "%s/%s/state", entity_ns(sensor), object_id);

    char value[24];
    int len = snprintf(value, sizeof(value), "%.4g", sensor->value);
    mqtt_publisher_publish(topic, value, len, 0, true);
}

void ha_discovery_publish_diagnostics(void) {
    if (!ha_discovery_enabled()) {
        return;
    }
    char topic[HA_TOPIC_BUF_SIZE];
    char value[24];
    int len;

    int8_t rssi = wifi_get_rssi();
    if (rssi != 0) {
        snprintf(topic, sizeof(topic), "%s/wifi_rssi/state", s_node);
        len = snprintf(value, sizeof(value), "%d", rssi);
        mqtt_publisher_publish(topic, value, len, 0, true);
    }

    snprintf(topic, sizeof(topic), "%s/uptime/state", s_node);
    len = snprintf(value, sizeof(value), "%lld", (long long)(esp_timer_get_time() / 1000000));
    mqtt_publisher_publish(topic, value, len, 0, true);

    snprintf(topic, sizeof(topic), "%s/heap_free/state", s_node);
    len = snprintf(value, sizeof(value), "%lu", (unsigned long)esp_get_free_heap_size());
    mqtt_publisher_publish(topic, value, len, 0, true);
}
