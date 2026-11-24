# PlatformIO pre-build script: give back ~6.9 KB of RAM.
#
# On the ESP8266 every non-PROGMEM `const` table lives in RAM. GFX Library for Arduino ships YCbCr->RGB lookup
# tables (YCbCr2RGB.h, ~6.9 KB) used by one JPEG-video helper, Arduino_DataBus::writeYCbCrPixels(), which this
# firmware never calls (it only draws RGB565). BearSSL's handshake needs every byte of that headroom: with the
# tables the heap drops to ~3.6 KB during a Yahoo request and the Wi-Fi driver crashes (Exception 29).
# We stub that single function in the *build copy* of the library; nothing outside .pio/libdeps is touched.
Import("env")
import os, re

p = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "GFX Library for Arduino", "src", "Arduino_DataBus.cpp")
if not os.path.exists(p):
    print("patch_gfx_lib: library not installed yet (run again after the first build)")
else:
    s = open(p, encoding="utf-8").read()
    if "SP500: YCbCr stubbed" not in s:
        new, n = re.subn(
            r"void Arduino_DataBus::writeYCbCrPixels\([^)]*\)\s*\{.*?\n\}\n",
            "void Arduino_DataBus::writeYCbCrPixels(uint8_t *, uint8_t *, uint8_t *, uint16_t, uint16_t)\n"
            "{\n  // SP500: YCbCr stubbed to drop 6.9 KB of RAM lookup tables (see tools/patch_gfx_lib.py)\n}\n",
            s, count=1, flags=re.S)
        if n:
            open(p, "w", encoding="utf-8").write(new)
            print("patch_gfx_lib: stubbed writeYCbCrPixels (-6.9 KB RAM)")
        else:
            print("patch_gfx_lib: WARNING - function not found, library version changed?")
