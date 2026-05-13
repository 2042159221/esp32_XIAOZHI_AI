#include "xiaozhi_handle.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "xiaozhi_handle";

xiaozhi_handle_t g_xiaozhi_handle;

static bool s_initialized;

static char *safe_strdup(const char *value)
{
    if (value == NULL) {
        return NULL;
    }

    size_t len = strlen(value) + 1;
    char *copy = (char *)heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, value, len);
    return copy;
}

static void replace_owned_string(char **target, char *next)
{
    if (target == NULL) {
        heap_caps_free(next);
        return;
    }

    heap_caps_free(*target);
    *target = next;
}

esp_err_t xiaozhi_handle_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    memset(&g_xiaozhi_handle, 0, sizeof(g_xiaozhi_handle));
    s_initialized = true;
    ESP_LOGI(TAG, "xiaozhi shared handle initialized");
    return ESP_OK;
}

void xiaozhi_handle_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    xiaozhi_handle_clear_runtime();
    s_initialized = false;
    ESP_LOGI(TAG, "xiaozhi shared handle deinitialized");
}

void xiaozhi_handle_clear_runtime(void)
{
    replace_owned_string(&g_xiaozhi_handle.websocket_url, NULL);
    replace_owned_string(&g_xiaozhi_handle.websocket_token, NULL);
    replace_owned_string(&g_xiaozhi_handle.activation_code, NULL);
    replace_owned_string(&g_xiaozhi_handle.activation_message, NULL);
    replace_owned_string(&g_xiaozhi_handle.activation_challenge, NULL);
    g_xiaozhi_handle.is_activated = false;
}

esp_err_t xiaozhi_handle_set_websocket(const char *url, const char *token)
{
    if (url == NULL || token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *url_copy = safe_strdup(url);
    if (url_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *token_copy = safe_strdup(token);
    if (token_copy == NULL) {
        heap_caps_free(url_copy);
        return ESP_ERR_NO_MEM;
    }

    replace_owned_string(&g_xiaozhi_handle.websocket_url, url_copy);
    replace_owned_string(&g_xiaozhi_handle.websocket_token, token_copy);
    return ESP_OK;
}

esp_err_t xiaozhi_handle_set_activation(const char *code, const char *message, const char *challenge)
{
    char *code_copy = safe_strdup(code);
    if (code != NULL && code_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *message_copy = safe_strdup(message);
    if (message != NULL && message_copy == NULL) {
        heap_caps_free(code_copy);
        return ESP_ERR_NO_MEM;
    }

    char *challenge_copy = safe_strdup(challenge);
    if (challenge != NULL && challenge_copy == NULL) {
        heap_caps_free(code_copy);
        heap_caps_free(message_copy);
        return ESP_ERR_NO_MEM;
    }

    replace_owned_string(&g_xiaozhi_handle.activation_code, code_copy);
    replace_owned_string(&g_xiaozhi_handle.activation_message, message_copy);
    replace_owned_string(&g_xiaozhi_handle.activation_challenge, challenge_copy);
    return ESP_OK;
}

void xiaozhi_handle_set_activated(bool activated)
{
    g_xiaozhi_handle.is_activated = activated;
}

const char *xiaozhi_handle_get_websocket_url(void)
{
    return g_xiaozhi_handle.websocket_url;
}

const char *xiaozhi_handle_get_websocket_token(void)
{
    return g_xiaozhi_handle.websocket_token;
}

const char *xiaozhi_handle_get_activation_code(void)
{
    return g_xiaozhi_handle.activation_code;
}

const char *xiaozhi_handle_get_activation_message(void)
{
    return g_xiaozhi_handle.activation_message;
}

const char *xiaozhi_handle_get_activation_challenge(void)
{
    return g_xiaozhi_handle.activation_challenge;
}

bool xiaozhi_handle_is_activated(void)
{
    return g_xiaozhi_handle.is_activated;
}
