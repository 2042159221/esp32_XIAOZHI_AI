# AGENTS.md

## Project Context

- This is an ESP-IDF project targeting ESP32-S3.
- ESP-IDF path in VS Code settings: `E:/Espressif/frameworks/esp-idf-v5.3.1/`.
- Python environment in VS Code settings: `E:/Espressif/python_env/idf5.3_py3.11_env`.
- Main application component lives in `main/`.

## Agent Notes

- Prefer official ESP-IDF tooling (`idf.py`, ESP-IDF VS Code/CMake integration) for dependency and build operations.
- If `idf.py` is missing from PATH, load the ESP-IDF environment first and explicitly set `IDF_PYTHON_ENV_PATH` to the Python 3.11 environment above.
- Keep component dependencies in `idf_component.yml` files under the relevant component directory.
- If CMake Tools reports that `/tools/cmake/project.cmake` cannot be found, check that `IDF_PATH` is present in the CMake configure/build environment; the ESP-IDF `CMakeLists.txt` include line is expected.
- For CMake Tools, keep the generator set to Ninja and ensure `PYTHON` points to `E:/Espressif/python_env/idf5.3_py3.11_env/Scripts/python.exe`; otherwise CMake may fall back to Anaconda Python and fail to import `idf_component_manager`.
- CMake Tools also needs the Xtensa toolchain path (`E:/Espressif/tools/xtensa-esp-elf/esp-13.2.0_20240530/xtensa-esp-elf/bin`) in its configure/build environment.

