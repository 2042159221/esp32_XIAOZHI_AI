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
    stage1 = read("main/app/xiaozhi_stage1.c")
    kconfig = read("main/Kconfig.projbuild")
    sdkconfig_defaults = read("sdkconfig.defaults")

    finish_body = function_body(provisioning, "finish_provisioning_stop")
    request_body = function_body(provisioning, "request_provisioning_stop_now")
    wifi_prov_end = case_body(provisioning, "case WIFI_PROV_END:")

    require("esp_prov_adapter_deinit();" in finish_body,
            "provisioning deinit must happen in finish_provisioning_stop")
    require("PROV_STOPPED_BIT set, PROV_DEINITED=1" in finish_body,
            "finish_provisioning_stop must log stopped/deinited gate")
    require("maybe_start_business_after_provisioning();" in finish_body,
            "finish_provisioning_stop must trigger stage1 gate")
    require("schedule_provisioning_finalize();" in request_body,
            "app-requested provisioning stop must schedule finalize fallback")
    require("schedule_provisioning_finalize();" in wifi_prov_end,
            "WIFI_PROV_END must schedule async finalize")
    require("esp_prov_adapter_deinit();" not in wifi_prov_end,
            "WIFI_PROV_END callback must not call deinit under manager lock")

    require("config XIAOZHI_STAGE1_AUTO_SR_ENABLE" in kconfig,
            "missing stage1 SR auto-init Kconfig switch")
    require("default n" in kconfig[kconfig.find("config XIAOZHI_STAGE1_AUTO_SR_ENABLE"):],
            "stage1 SR auto-init must default to disabled")
    require("CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE" in stage1,
            "stage1 must gate SR init on config")
    require("websocket READY; SR auto init disabled" in stage1,
            "stage1 must log SR auto-init disabled after READY")
    require("CONFIG_XIAOZHI_STAGE1_AUTO_SR_ENABLE" in sdkconfig_defaults,
            "sdkconfig.defaults must explicitly keep SR auto-init disabled")


if __name__ == "__main__":
    main()
