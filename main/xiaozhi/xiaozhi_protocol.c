#include "xiaozhi_protocol.h"

#include <string.h>
#include <stdbool.h>

#include "cJSON.h"
#include "esp_check.h"

static const char *TAG = "xiaozhi_protocol";

static bool add_string(cJSON *root, const char *key, const char *value)
{
    return cJSON_AddStringToObject(root, key, value) != NULL;
}

static bool add_number(cJSON *root, const char *key, int value)
{
    return cJSON_AddNumberToObject(root, key, value) != NULL;
}

static esp_err_t print_json(cJSON *root, char **out_json)
{
    ESP_RETURN_ON_FALSE(root != NULL && out_json != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid json print arguments");

    *out_json = cJSON_PrintUnformatted(root);
    ESP_RETURN_ON_FALSE(*out_json != NULL, ESP_ERR_NO_MEM, TAG, "print json failed");
    return ESP_OK;
}

static esp_err_t add_session(cJSON *root, const char *session_id)
{
    ESP_RETURN_ON_FALSE(session_id != NULL && session_id[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "session_id is required");
    ESP_RETURN_ON_FALSE(add_string(root, "session_id", session_id), ESP_ERR_NO_MEM, TAG, "add session_id failed");
    return ESP_OK;
}

static void copy_json_string(cJSON *root, const char *key, char *dest, size_t dest_size)
{
    if (dest == NULL || dest_size == 0) {
        return;
    }

    dest[0] = '\0';
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return;
    }

    strlcpy(dest, item->valuestring, dest_size);
}

static int get_json_int(cJSON *root, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static xiaozhi_protocol_msg_type_t classify_type(const char *type)
{
    if (type == NULL) {
        return XIAOZHI_PROTOCOL_MSG_UNKNOWN;
    }
    if (strcmp(type, "hello") == 0) {
        return XIAOZHI_PROTOCOL_MSG_HELLO;
    }
    if (strcmp(type, "stt") == 0) {
        return XIAOZHI_PROTOCOL_MSG_STT;
    }
    if (strcmp(type, "tts") == 0) {
        return XIAOZHI_PROTOCOL_MSG_TTS;
    }
    if (strcmp(type, "llm") == 0) {
        return XIAOZHI_PROTOCOL_MSG_LLM;
    }
    if (strcmp(type, "mcp") == 0) {
        return XIAOZHI_PROTOCOL_MSG_MCP;
    }
    if (strcmp(type, "system") == 0) {
        return XIAOZHI_PROTOCOL_MSG_SYSTEM;
    }
    if (strcmp(type, "alert") == 0) {
        return XIAOZHI_PROTOCOL_MSG_ALERT;
    }
    return XIAOZHI_PROTOCOL_MSG_UNKNOWN;
}

esp_err_t xiaozhi_protocol_build_hello_json(char **out_json)
{
    ESP_RETURN_ON_FALSE(out_json != NULL, ESP_ERR_INVALID_ARG, TAG, "out_json is NULL");
    *out_json = NULL;

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "create hello root failed");

    cJSON *audio = cJSON_CreateObject();
    cJSON *features = cJSON_CreateObject();
    esp_err_t err = ESP_OK;
    if (audio == NULL ||
        features == NULL ||
        !add_string(root, "type", "hello") ||
        !add_number(root, "version", XIAOZHI_PROTOCOL_VERSION) ||
        !add_string(root, "transport", XIAOZHI_PROTOCOL_TRANSPORT)) {
        err = ESP_ERR_NO_MEM;
    }

    cJSON *audio_params = audio;
    if (err == ESP_OK) {
        if (!cJSON_AddItemToObject(root, "audio_params", audio_params)) {
            err = ESP_ERR_NO_MEM;
        } else {
            audio = NULL;
        }
    }

    if (err == ESP_OK) {
        if (!cJSON_AddItemToObject(root, "features", features)) {
            err = ESP_ERR_NO_MEM;
        } else {
            features = NULL;
        }
    }

    if (err == ESP_OK &&
        (!add_string(audio_params, "format", XIAOZHI_PROTOCOL_AUDIO_FORMAT) ||
         !add_number(audio_params, "sample_rate", XIAOZHI_PROTOCOL_AUDIO_SAMPLE_RATE) ||
         !add_number(audio_params, "channels", XIAOZHI_PROTOCOL_AUDIO_CHANNELS) ||
         !add_number(audio_params, "frame_duration", XIAOZHI_PROTOCOL_AUDIO_FRAME_DURATION_MS))) {
        err = ESP_ERR_NO_MEM;
    }

    cJSON *features_obj = cJSON_GetObjectItem(root, "features");
    if (err == ESP_OK &&
        (features_obj == NULL || cJSON_AddBoolToObject(features_obj, "mcp", true) == NULL)) {
        err = ESP_ERR_NO_MEM;
    }

    if (err == ESP_OK) {
        err = print_json(root, out_json);
    }

    cJSON_Delete(audio);
    cJSON_Delete(features);
    cJSON_Delete(root);
    return err;
}

esp_err_t xiaozhi_protocol_build_listen_start_json(const char *session_id, const char *mode, char **out_json)
{
    ESP_RETURN_ON_FALSE(out_json != NULL, ESP_ERR_INVALID_ARG, TAG, "out_json is NULL");
    *out_json = NULL;

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "create listen start root failed");

    esp_err_t err = add_session(root, session_id);
    if (err == ESP_OK &&
        (!add_string(root, "type", "listen") ||
         !add_string(root, "state", "start") ||
         !add_string(root, "mode", (mode != NULL && mode[0] != '\0') ? mode : "auto"))) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = print_json(root, out_json);
    }

    cJSON_Delete(root);
    return err;
}

esp_err_t xiaozhi_protocol_build_listen_stop_json(const char *session_id, char **out_json)
{
    ESP_RETURN_ON_FALSE(out_json != NULL, ESP_ERR_INVALID_ARG, TAG, "out_json is NULL");
    *out_json = NULL;

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "create listen stop root failed");

    esp_err_t err = add_session(root, session_id);
    if (err == ESP_OK &&
        (!add_string(root, "type", "listen") ||
         !add_string(root, "state", "stop"))) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = print_json(root, out_json);
    }

    cJSON_Delete(root);
    return err;
}

esp_err_t xiaozhi_protocol_build_abort_json(const char *session_id, const char *reason, char **out_json)
{
    ESP_RETURN_ON_FALSE(out_json != NULL, ESP_ERR_INVALID_ARG, TAG, "out_json is NULL");
    *out_json = NULL;

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "create abort root failed");

    esp_err_t err = add_session(root, session_id);
    if (err == ESP_OK &&
        (!add_string(root, "type", "abort") ||
         !add_string(root, "reason", (reason != NULL && reason[0] != '\0') ? reason : "manual"))) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = print_json(root, out_json);
    }

    cJSON_Delete(root);
    return err;
}

esp_err_t xiaozhi_protocol_parse_server_message(const char *json, size_t len, xiaozhi_protocol_msg_t *out_msg)
{
    ESP_RETURN_ON_FALSE(json != NULL && len > 0 && out_msg != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid parse arguments");
    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->type = XIAOZHI_PROTOCOL_MSG_UNKNOWN;

    cJSON *root = cJSON_ParseWithLength(json, len);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_FAIL, TAG, "parse server json failed");

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    out_msg->type = classify_type(type->valuestring);
    copy_json_string(root, "session_id", out_msg->session_id, sizeof(out_msg->session_id));
    copy_json_string(root, "transport", out_msg->transport, sizeof(out_msg->transport));
    copy_json_string(root, "state", out_msg->state, sizeof(out_msg->state));
    copy_json_string(root, "text", out_msg->text, sizeof(out_msg->text));
    if (out_msg->text[0] == '\0') {
        copy_json_string(root, "message", out_msg->text, sizeof(out_msg->text));
    }

    cJSON *audio = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio)) {
        copy_json_string(audio, "format", out_msg->audio.format, sizeof(out_msg->audio.format));
        out_msg->audio.sample_rate = get_json_int(audio, "sample_rate", 0);
        out_msg->audio.channels = get_json_int(audio, "channels", 0);
        out_msg->audio.frame_duration_ms = get_json_int(audio, "frame_duration", 0);
    }

    cJSON_Delete(root);
    return ESP_OK;
}
