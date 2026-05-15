#include <string.h>

#include "cJSON.h"
#include "unity.h"
#include "xiaozhi_device.h"
#include "xiaozhi_protocol.h"

static void assert_json_string(cJSON *root, const char *key, const char *expected)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    TEST_ASSERT_TRUE(cJSON_IsString(item));
    TEST_ASSERT_EQUAL_STRING(expected, item->valuestring);
}

static void assert_json_number(cJSON *root, const char *key, int expected)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    TEST_ASSERT_TRUE(cJSON_IsNumber(item));
    TEST_ASSERT_EQUAL_INT(expected, item->valueint);
}

static void assert_json_bool(cJSON *root, const char *key, bool expected)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    TEST_ASSERT_TRUE(cJSON_IsBool(item));
    TEST_ASSERT_EQUAL(expected, cJSON_IsTrue(item));
}

TEST_CASE("hello json announces websocket opus audio params", "[xiaozhi_protocol]")
{
    char *json = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, xiaozhi_protocol_build_hello_json(&json));
    TEST_ASSERT_NOT_NULL(json);

    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    assert_json_string(root, "type", "hello");
    assert_json_number(root, "version", 1);
    assert_json_string(root, "transport", "websocket");

    cJSON *audio = cJSON_GetObjectItem(root, "audio_params");
    TEST_ASSERT_TRUE(cJSON_IsObject(audio));
    assert_json_string(audio, "format", "opus");
    assert_json_number(audio, "sample_rate", 16000);
    assert_json_number(audio, "channels", 1);
    assert_json_number(audio, "frame_duration", 60);

    cJSON *features = cJSON_GetObjectItem(root, "features");
    TEST_ASSERT_TRUE(cJSON_IsObject(features));
    assert_json_bool(features, "mcp", true);

    cJSON_Delete(root);
    cJSON_free(json);
}

TEST_CASE("device mac formatter uses lowercase websocket compatible form", "[xiaozhi_device]")
{
    const unsigned char mac_bytes[6] = {0x20, 0x6E, 0xF1, 0x34, 0xE5, 0xB4};
    char mac[XIAOZHI_MAC_STR_LEN] = {0};

    TEST_ASSERT_EQUAL(ESP_OK, xiaozhi_device_format_mac_str(mac_bytes, mac, sizeof(mac)));
    TEST_ASSERT_EQUAL_STRING("20:6e:f1:34:e5:b4", mac);
}

TEST_CASE("listen start and stop json include session", "[xiaozhi_protocol]")
{
    char *json = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, xiaozhi_protocol_build_listen_start_json("sid-1", "auto", &json));
    TEST_ASSERT_NOT_NULL(json);

    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    assert_json_string(root, "session_id", "sid-1");
    assert_json_string(root, "type", "listen");
    assert_json_string(root, "state", "start");
    assert_json_string(root, "mode", "auto");
    cJSON_Delete(root);
    cJSON_free(json);

    json = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, xiaozhi_protocol_build_listen_stop_json("sid-1", &json));
    root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    assert_json_string(root, "session_id", "sid-1");
    assert_json_string(root, "type", "listen");
    assert_json_string(root, "state", "stop");
    cJSON_Delete(root);
    cJSON_free(json);
}

TEST_CASE("abort json includes default reason fallback", "[xiaozhi_protocol]")
{
    char *json = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, xiaozhi_protocol_build_abort_json("sid-1", NULL, &json));
    TEST_ASSERT_NOT_NULL(json);

    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    assert_json_string(root, "session_id", "sid-1");
    assert_json_string(root, "type", "abort");
    assert_json_string(root, "reason", "manual");

    cJSON_Delete(root);
    cJSON_free(json);
}

TEST_CASE("server hello parser saves session and audio params", "[xiaozhi_protocol]")
{
    const char *json = "{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":\"abc\","
                       "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":24000,\"channels\":1,\"frame_duration\":60}}";
    xiaozhi_protocol_msg_t msg = {0};

    TEST_ASSERT_EQUAL(ESP_OK, xiaozhi_protocol_parse_server_message(json, strlen(json), &msg));
    TEST_ASSERT_EQUAL(XIAOZHI_PROTOCOL_MSG_HELLO, msg.type);
    TEST_ASSERT_EQUAL_STRING("abc", msg.session_id);
    TEST_ASSERT_EQUAL_STRING("websocket", msg.transport);
    TEST_ASSERT_EQUAL_STRING("opus", msg.audio.format);
    TEST_ASSERT_EQUAL_INT(24000, msg.audio.sample_rate);
    TEST_ASSERT_EQUAL_INT(1, msg.audio.channels);
    TEST_ASSERT_EQUAL_INT(60, msg.audio.frame_duration_ms);
}

TEST_CASE("server message parser classifies text states", "[xiaozhi_protocol]")
{
    xiaozhi_protocol_msg_t msg = {0};

    const char *stt = "{\"type\":\"stt\",\"text\":\"hello\"}";
    TEST_ASSERT_EQUAL(ESP_OK, xiaozhi_protocol_parse_server_message(stt, strlen(stt), &msg));
    TEST_ASSERT_EQUAL(XIAOZHI_PROTOCOL_MSG_STT, msg.type);
    TEST_ASSERT_EQUAL_STRING("hello", msg.text);

    const char *tts = "{\"type\":\"tts\",\"state\":\"start\"}";
    TEST_ASSERT_EQUAL(ESP_OK, xiaozhi_protocol_parse_server_message(tts, strlen(tts), &msg));
    TEST_ASSERT_EQUAL(XIAOZHI_PROTOCOL_MSG_TTS, msg.type);
    TEST_ASSERT_EQUAL_STRING("start", msg.state);

    const char *unknown = "{\"type\":\"custom\",\"payload\":{}}";
    TEST_ASSERT_EQUAL(ESP_OK, xiaozhi_protocol_parse_server_message(unknown, strlen(unknown), &msg));
    TEST_ASSERT_EQUAL(XIAOZHI_PROTOCOL_MSG_UNKNOWN, msg.type);
}

TEST_CASE("server message parser accepts alert message field", "[xiaozhi_protocol]")
{
    const char *alert = "{\"type\":\"alert\",\"message\":\"battery low\"}";
    xiaozhi_protocol_msg_t msg = {0};

    TEST_ASSERT_EQUAL(ESP_OK, xiaozhi_protocol_parse_server_message(alert, strlen(alert), &msg));
    TEST_ASSERT_EQUAL(XIAOZHI_PROTOCOL_MSG_ALERT, msg.type);
    TEST_ASSERT_EQUAL_STRING("battery low", msg.text);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
