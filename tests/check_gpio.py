"""Protect the existing camera/LCD wiring when changing cabinet GPIOs."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
main = (root / "src/main.c").read_text(encoding="utf-8")
header = (root / "src/classhub.h").read_text(encoding="utf-8")
pins = dict(re.findall(r"^#define\s+((?:CAM_PIN_|TFT_PIN_)\w+)\s+(-?\d+)", main, re.M))
pins = {key: int(value) for key, value in pins.items()}
expected = {
    "CAM_PIN_PWDN": -1, "CAM_PIN_RESET": -1,
    "CAM_PIN_SIOD": 9, "CAM_PIN_SIOC": 10, "CAM_PIN_XCLK": 11,
    "CAM_PIN_D0": 4, "CAM_PIN_D1": 5, "CAM_PIN_D2": 8, "CAM_PIN_D3": 7,
    "CAM_PIN_D4": 15, "CAM_PIN_D5": 16, "CAM_PIN_D6": 17, "CAM_PIN_D7": 18,
    "CAM_PIN_VSYNC": 14, "CAM_PIN_HREF": 12, "CAM_PIN_PCLK": 13,
    "TFT_PIN_CS": 38, "TFT_PIN_RST": 39, "TFT_PIN_DC": 40,
    "TFT_PIN_MOSI": 41, "TFT_PIN_SCK": 42, "TFT_PIN_BL": -1,
}
assert pins == expected, "Camera or LCD wiring changed"
cabinet = [int(value) for value in re.findall(r"^#define\s+CLASSHUB_\w+_PIN\s+(\d+)", header, re.M)]
assert len(cabinet) == 4 and len(set(cabinet)) == 4, "Cabinet pins missing/duplicated"
assert "CLASSHUB_LED_PIN" not in header, "LED is no longer used"
assert not set(cabinet).intersection(value for value in pins.values() if value >= 0), "Cabinet overlaps camera/LCD"
reserved = {19, 20, *range(22, 38)}
assert not set(cabinet).intersection(reserved), "Cabinet overlaps USB or flash/PSRAM pins"
rfid_header = (root / "src/rc522_reader.h").read_text(encoding="utf-8")
rfid = [int(v) for v in re.findall(r"^#define\s+RFID_PIN_\w+\s+(\d+)", rfid_header, re.M)]
assert len(rfid) == 3 and len(set(rfid)) == 3, "RFID pins missing/duplicated"
# This board uses external UART; GPIO19 (native USB D-) is reassigned to MISO.
assert "#define RFID_PIN_MISO 19" in rfid_header, "RFID MISO migration missing"
assert not set(rfid).intersection((reserved - {19}) | {0, 3, 6, 43, 44, 45, 46}), "RFID uses reserved/strapping/UART or retired GPIO6 pins"
assert not set(rfid).intersection(cabinet + [v for v in pins.values() if v >= 0]), "RFID pin collision"
assert ".miso_io_num = RFID_PIN_MISO" in main, "RFID MISO not attached to LCD bus"
print("PASS: camera/LCD wiring preserved; cabinet GPIOs do not overlap")
