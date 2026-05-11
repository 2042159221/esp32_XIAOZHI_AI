# Xiaozhi Stage1 Review Report

Date: 2026-05-11

[Agent Report]

Agent: Review Agent

Branch: review/xiaozhi-stage1

Scope: Review `master...qa/xiaozhi-build-fixes` for xiaozhi stage1 integration, including OTA activation flow, shared handle, device identity, LVGL UI, build wiring, and partition sizing.

Files Changed:

- `dependencies.lock`
- `docs/xiaozhi_stage1_repo_map.md`
- `main/CMakeLists.txt`
- `main/Kconfig.projbuild`
- `main/app/app_controller.c`
- `main/app/xiaozhi_stage1.c`
- `main/app/xiaozhi_stage1.h`
- `main/idf_component.yml`
- `main/ui/display/xiaozhi_ui.c`
- `main/ui/display/xiaozhi_ui.h`
- `main/ui/screens/provisioning_screen.c`
- `main/xiaozhi/xiaozhi_device.c`
- `main/xiaozhi/xiaozhi_device.h`
- `main/xiaozhi/xiaozhi_handle.c`
- `main/xiaozhi/xiaozhi_handle.h`
- `main/xiaozhi/xiaozhi_ota.c`
- `main/xiaozhi/xiaozhi_ota.h`
- `main/xiaozhi/xiaozhi_ws.c`
- `main/xiaozhi/xiaozhi_ws.h`
- `partitions.csv`

Commits:

- `07efb0b docs(repo): map current project structure and integration points`
- `07c10c7 feat(core): add xiaozhi shared handle and device identity helpers`
- `fbc2163 feat(network): add xiaozhi ota client and websocket placeholder`
- `db60d28 feat(ui): add xiaozhi lvgl status screens`
- `cb22658 feat(integration): wire xiaozhi stage1 flow after wifi connection`
- `826ee4d fix(ui): use ascii-safe utf8 strings for xiaozhi screens`
- `efee34d fix(build): fit xiaozhi fonts within app partition`

Build Status:

- `idf.py build` passed on branch `qa/xiaozhi-build-fixes` on 2026-05-11.
- Verified app binary size fits the updated `factory` partition after font and partition adjustments.

Risks:

- `xiaozhi-fonts` currently uses the basic Puhui subset to control binary size. Future UI strings outside the covered charset may need additional font coverage.
- `partitions.csv` expands the factory app partition from `0x200000` to `0x280000`, which reduces the storage partition capacity.
- OTA activation logic is compile-verified but still needs live backend validation against the real xiaozhi service contract.

Needs From Other Agents:

- None.

Ready To Merge: yes

## Findings

No blocking defects were found in the reviewed diff.

1. Diff scope is clean and limited to stage1 delivery.
   - No unrelated user workspace changes such as `.vscode/`, `daily_Report/`, or `sdkconfig*` are part of `master...qa/xiaozhi-build-fixes`.

2. Token logging is masked.
   - `main/xiaozhi/xiaozhi_ota.c` logs only the first 8 characters plus length.
   - `main/xiaozhi/xiaozhi_ws.c` and `main/app/xiaozhi_stage1.c` log token presence or length only.

3. Memory ownership and cleanup paths are in place.
   - `main/xiaozhi/xiaozhi_handle.c` deep-copies all stored strings and releases replaced buffers.
   - `main/xiaozhi/xiaozhi_ota.c` frees response buffers, calls `cJSON_Delete(root)`, calls `cJSON_free()` for the serialized request body, and always cleans up the HTTP client handle.
   - `main/xiaozhi/xiaozhi_device.c` commits UUID writes and closes the NVS handle on the relevant paths.

4. LVGL access is guarded.
   - `main/ui/display/xiaozhi_ui.c` wraps UI mutation paths with `lvgl_port_lock()` and `lvgl_port_unlock()`.

5. OTA task startup is serialized.
   - `main/app/xiaozhi_stage1.c` uses `s_ota_task_handle` as a guard against concurrent startup and clears it before task deletion.

6. Existing provisioning and QR display path remains intact.
   - `main/ui/screens/provisioning_screen.c` still exposes QR/status display entry points.
   - The integration hook is attached at `business_start_cb()` rather than rewriting the Wi-Fi provisioning state machine.

## Open Questions

1. Runtime response compatibility with the live OTA endpoint still needs on-device validation.
2. The current build passes with the basic Chinese font subset, but any later UI copy expansion should be checked for glyph coverage.
3. The enlarged app partition reduces SPIFFS headroom and should be acknowledged before future media-heavy features are added.
