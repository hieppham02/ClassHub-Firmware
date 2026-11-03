$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force .codex-temp | Out-Null
    & g++ -std=c++11 -Wall -Wextra -Werror tests/wifi_fallback.cpp -o .codex-temp/wifi-fallback.exe
    if ($LASTEXITCODE -ne 0) { throw 'Wi-Fi fallback compilation failed' }
    & .codex-temp/wifi-fallback.exe
    if ($LASTEXITCODE -ne 0) { throw 'Wi-Fi fallback checks failed' }
    & g++ -std=c++11 -Wall -Wextra -Werror -I src tests/rfid_uid.cpp -o .codex-temp/rfid-uid.exe
    if ($LASTEXITCODE -ne 0) { throw 'RFID UID compilation failed' }
    & .codex-temp/rfid-uid.exe
    if ($LASTEXITCODE -ne 0) { throw 'RFID UID checks failed' }
    & gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-function tests/rc522_probe.c -o .codex-temp/rc522-probe.exe
    if ($LASTEXITCODE -ne 0) { throw 'RC522 probe compilation failed' }
    & .codex-temp/rc522-probe.exe
    if ($LASTEXITCODE -ne 0) { throw 'RC522 probe checks failed' }
    & g++ -std=c++11 -Wall -Wextra -Werror -I src tests/qr_rules.cpp -o .codex-temp/qr-rules.exe
    if ($LASTEXITCODE -ne 0) { throw 'QR rules compilation failed' }
    & .codex-temp/qr-rules.exe
    if ($LASTEXITCODE -ne 0) { throw 'QR rules checks failed' }
    & g++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -I managed_components/espressif__cjson/cJSON tests/cabinet_runtime.cpp managed_components/espressif__cjson/cJSON/cJSON.c -o .codex-temp/cabinet-runtime.exe
    if ($LASTEXITCODE -ne 0) { throw 'Cabinet runtime compilation failed' }
    & .codex-temp/cabinet-runtime.exe
    if ($LASTEXITCODE -ne 0) { throw 'Cabinet runtime checks failed' }
    & g++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -I managed_components/espressif__cjson/cJSON tests/ota_http.cpp managed_components/espressif__cjson/cJSON/cJSON.c -o .codex-temp/ota-http.exe
    if ($LASTEXITCODE -ne 0) { throw 'OTA upload compilation failed' }
    & .codex-temp/ota-http.exe
    if ($LASTEXITCODE -ne 0) { throw 'OTA upload checks failed' }
    & py tests/check_gpio.py
    if ($LASTEXITCODE -ne 0) { throw 'GPIO wiring checks failed' }
    & g++ -std=c++11 -Wall -Wextra -Werror -I src tests/camera_stream_io.cpp -o .codex-temp/camera-stream-io.exe
    if ($LASTEXITCODE -ne 0) { throw 'Camera transport compilation failed' }
    & .codex-temp/camera-stream-io.exe
    if ($LASTEXITCODE -ne 0) { throw 'Camera transport checks failed' }
    & node tests/camera_web.cjs
    if ($LASTEXITCODE -ne 0) { throw 'Camera browser checks failed' }
    & node tests/camera_jpeg_web.cjs
    if ($LASTEXITCODE -ne 0) { throw 'JPEG camera browser checks failed' }
    & node tests/device_web.cjs
    if ($LASTEXITCODE -ne 0) { throw 'Device browser checks failed' }
    & node tests/ota_web.cjs
    if ($LASTEXITCODE -ne 0) { throw 'OTA browser checks failed' }
    & py tests/ota_layout.py
    if ($LASTEXITCODE -ne 0) { throw 'OTA partition layout checks failed' }
    & py tools/embed_web.py --check
    if ($LASTEXITCODE -ne 0) { throw 'Embedded web page is stale' }
} finally {
    Pop-Location
}
