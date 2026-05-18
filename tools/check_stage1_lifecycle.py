from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8", errors="ignore")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def function_body(source, name):
    marker = f"static void {name}"
    start = -1
    search_from = 0
    while True:
        candidate = source.find(marker, search_from)
        require(candidate >= 0, f"missing function {name}")
        brace = source.find("{", candidate)
        semi = source.find(";", candidate)
        require(brace >= 0, f"missing body for {name}")
        if semi < 0 or brace < semi:
            start = candidate
            break
        search_from = semi + 1
    require(start >= 0, f"missing function {name}")
    brace = source.find("{", start)
    require(brace >= 0, f"missing body for {name}")
    depth = 0
    for index in range(brace, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated body for {name}")


def case_body(source, case_label):
    start = source.find(case_label)
    require(start >= 0, f"missing {case_label}")
    end = source.find("break;", start)
    require(end >= 0, f"missing break for {case_label}")
    return source[start:end]


def main():
    provisioning = read("main/services/provisioning/provisioning_service.c")
    wifi_sta = read("main/services/network/wifi_sta_service.c")
    prov_adapter = read("main/infrastructure/esp_provisioning/esp_prov_adapter.c")
    prov_strategy = read("main/infrastructure/esp_provisioning/esp_prov_strategy.c")
    stage1 = read("main/app/xiaozhi_stage1.c")
    kconfig = read("main/Kconfig.projbuild")
    sdkconfig_defaults = read("sdkconfig.defaults")

    finish_body = function_body(provisioning, "finish_provisioning_stop")
    request_body = function_body(provisioning, "request_provisioning_stop_now")
    maybe_start_body = function_body(provisioning, "maybe_start_business_after_provisioning")
    start_business_body = function_body(provisioning, "start_business")
    wifi_prov_end = case_body(provisioning, "case WIFI_PROV_END:")
    wifi_cred_success = case_body(provisioning, "case WIFI_PROV_CRED_SUCCESS:")
    wifi_cred_fail = case_body(provisioning, "case WIFI_PROV_CRED_FAIL:")

    require("esp_prov_adapter_deinit();" in finish_body,
            "provisioning deinit must happen in finish_provisioning_stop")
    require("provisioning deinit complete" in finish_body,
            "finish_provisioning_stop must log provisioning deinit completion")
    require("stop_residual_wifi_scan" in finish_body,
            "finish_provisioning_stop must stop any residual provisioning Wi-Fi scan before deinit")
    require("PROV_STOPPED_BIT set, PROV_DEINITED=1" in finish_body,
            "finish_provisioning_stop must log stopped/deinited gate")
    require("maybe_start_business_after_provisioning();" in finish_body,
            "finish_provisioning_stop must trigger stage1 gate")
    require("business start gate passed" in maybe_start_body,
            "business startup must log that provisioning stop/deinit gate passed")
    require("before stage1 start" in start_business_body,
            "business startup must log immediately before xiaozhi stage1")
    require("schedule_provisioning_finalize();" in request_body,
            "app-requested provisioning stop must schedule finalize fallback")
    require("WIFI_PROV_CRED_SUCCESS" in wifi_cred_success,
            "WIFI_PROV_CRED_SUCCESS must be logged before stopping provisioning")
    require("s_restart_provisioning_after_stop" not in wifi_cred_fail,
            "credential failure must not restart BLE provisioning after BTDM release")
    require("esp_prov_adapter_stop_provisioning();" not in wifi_cred_fail,
            "credential failure must keep provisioning active for corrected credentials")
    require("schedule_provisioning_finalize();" not in wifi_cred_fail,
            "credential failure must not deinit BLE provisioning")
    require("schedule_provisioning_finalize();" in wifi_prov_end,
            "WIFI_PROV_END must schedule async finalize")
    require("WIFI_PROV_END" in wifi_prov_end,
            "WIFI_PROV_END must be logged when the provisioning manager reports stop complete")
    require("esp_prov_adapter_deinit();" not in wifi_prov_end,
            "WIFI_PROV_END callback must not call deinit under manager lock")
    require("wifi_prov_mgr_stop_provisioning();" in prov_adapter,
            "provisioning adapter must stop wifi_prov_mgr explicitly")
    require("wifi_prov_mgr_deinit();" in prov_adapter,
            "provisioning adapter must deinit wifi_prov_mgr explicitly")
    require("wifi_prov_mgr_start_provisioning" in prov_strategy,
            "provisioning manager start must stay isolated to the provisioning strategy")
    require("esp_wifi_scan_start" not in read("main/services/provisioning/provisioning_service.c"),
            "provisioning service must not start periodic Wi-Fi scans")
    require("esp_wifi_scan_start" not in wifi_sta,
            "business Wi-Fi STA service must not start periodic Wi-Fi scans")
    require("#define WIFI_STA_SERVICE_COUNTRY_CODE \"CN\"" in wifi_sta,
            "business Wi-Fi STA service must explicitly configure the local 2.4G country code")
    require("#define WIFI_STA_SERVICE_IEEE80211D_ENABLED false" in wifi_sta,
            "business Wi-Fi STA service must disable 802.11d auto country override")
    require("esp_wifi_set_country_code(WIFI_STA_SERVICE_COUNTRY_CODE" in wifi_sta,
            "business Wi-Fi STA service must set Wi-Fi country before connecting")
    require("esp_wifi_get_country(&configured)" in wifi_sta,
            "business Wi-Fi STA service must log the effective Wi-Fi country")
    require("WIFI_COUNTRY_POLICY_MANUAL" in wifi_sta,
            "business Wi-Fi STA service must verify manual country policy")
    country_pos = wifi_sta.find("ESP_RETURN_ON_ERROR(configure_wifi_country()")
    mode_pos = wifi_sta.find("ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA)")
    require(country_pos >= 0 and mode_pos >= 0 and country_pos < mode_pos,
            "business Wi-Fi STA service must configure country before setting STA mode")

    require("config XIAOZHI_STAGE1_AUTO_SR_ENABLE" in kconfig,
            "missing stage1 SR auto-init Kconfig switch")
    require("default n" in kconfig[kconfig.find("config XIAOZHI_STAGE1_AUTO_SR_ENABLE"):],
            "stage1 SR auto-init must default to disabled")
    require("CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE" in stage1,
            "stage1 must gate SR init on config")
    require("SR auto init disabled" in stage1,
            "stage1 must log SR auto-init disabled after READY")
    require("CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE" in sdkconfig_defaults,
            "sdkconfig.defaults must explicitly keep SR auto-init disabled")


if __name__ == "__main__":
    main()
