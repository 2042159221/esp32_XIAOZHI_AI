@echo off
setlocal EnableExtensions

rem Project-local ESP-IDF environment for ai-xiaozhi-sample.
rem This project was created with ESP-IDF v5.3.1, so keep it isolated here.
set "IDF_TOOLS_PATH=E:\Espressif"
set "IDF_PATH=E:\Espressif\frameworks\esp-idf-v5.3.1"
set "IDF_TARGET=esp32s3"
set "IDF_PYTHON_ENV_PATH=E:\Espressif\python_env\idf5.3_py3.11_env"
set "IDF_PYTHON=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe"

if not exist "%IDF_PYTHON%" (
  echo [ESP-IDF] Python not found: %IDF_PYTHON%
  exit /b 1
)

if not exist "%IDF_PATH%\tools\idf.py" (
  echo [ESP-IDF] idf.py not found under: %IDF_PATH%
  exit /b 1
)

rem Put the project ESP-IDF Python before Anaconda/system Python.
set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;%IDF_PATH%\tools;E:\Espressif\tools\idf-exe\1.0.3;E:\Espressif\tools\cmake\3.24.0\bin;E:\Espressif\tools\ninja\1.11.1;E:\Espressif\tools\ccache\4.8\ccache-4.8-windows-x86_64;E:\Espressif\tools\xtensa-esp-elf\esp-13.2.0_20240530\xtensa-esp-elf\bin;%PATH%"

rem Avoid leaking packages from another Python installation.
set "PYTHONPATH="
set "PYTHONHOME="
set "PYTHONNOUSERSITE=True"

doskey idf.py="%IDF_PYTHON%" "%IDF_PATH%\tools\idf.py" $*
doskey esptool.py="%IDF_PYTHON%" "%IDF_PATH%\components\esptool_py\esptool\esptool.py" $*
doskey espefuse.py="%IDF_PYTHON%" "%IDF_PATH%\components\esptool_py\esptool\espefuse.py" $*
doskey espsecure.py="%IDF_PYTHON%" "%IDF_PATH%\components\esptool_py\esptool\espsecure.py" $*
doskey otatool.py="%IDF_PYTHON%" "%IDF_PATH%\components\app_update\otatool.py" $*
doskey parttool.py="%IDF_PYTHON%" "%IDF_PATH%\components\partition_table\parttool.py" $*

echo ESP-IDF project environment loaded.
echo IDF_PATH=%IDF_PATH%
echo IDF_PYTHON=%IDF_PYTHON%
"%IDF_PYTHON%" --version

endlocal & (
  set "IDF_TOOLS_PATH=%IDF_TOOLS_PATH%"
  set "IDF_PATH=%IDF_PATH%"
  set "IDF_TARGET=%IDF_TARGET%"
  set "IDF_PYTHON_ENV_PATH=%IDF_PYTHON_ENV_PATH%"
  set "PATH=%PATH%"
  set "PYTHONPATH=%PYTHONPATH%"
  set "PYTHONHOME=%PYTHONHOME%"
  set "PYTHONNOUSERSITE=%PYTHONNOUSERSITE%"
)
