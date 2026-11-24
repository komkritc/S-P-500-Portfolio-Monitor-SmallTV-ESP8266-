#!/usr/bin/env python3
"""Builds the host LCD preview, renders every scenario to tests/preview/out/*.png and runs layout checks.
Needs a C++ compiler (g++/clang++ or `pip install ziglang`) and Pillow."""
import glob, os, shutil, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
tests = os.path.dirname(here)
src = os.path.join(tests, "..", "src")
cc = shutil.which("g++") or shutil.which("clang++")
cmd = [cc] if cc else [sys.executable, "-m", "ziglang", "c++"]
exe = os.path.join(here, "preview.exe" if os.name == "nt" else "preview")
files = [os.path.join(here, "preview.cpp")] + [os.path.join(src, p) for p in (
    "core/MarketClock.cpp", "core/Format.cpp", "core/AppState.cpp", "finance/FinancialCalculator.cpp", "finance/Portfolio.cpp",
    "finance/JsonStream.cpp", "finance/YahooParser.cpp", "finance/ChartData.cpp", "core/JsonWriter.cpp",
    "display/UiKit.cpp", "display/Widgets.cpp", "display/PortfolioScreen.cpp", "display/PositionsScreen.cpp",
    "display/RealisedScreen.cpp",
    "display/MarketScreen.cpp", "display/ChartRenderer.cpp", "display/ViewSchedule.cpp",
    "finance/SaleEngine.cpp",
    "display/font/lcd_font.cpp", "display/font/lcd_fonts_data.cpp")]
os.makedirs(os.path.join(tests, "preview", "out"), exist_ok=True)
subprocess.check_call(cmd + ["-std=c++17", "-O1", "-g", "-w", "-I", os.path.join(here, "mock"), "-o", exe] + files)
rc = subprocess.call([exe, tests])
try:
    from PIL import Image
    for p in sorted(glob.glob(os.path.join(here, "out", "*.raw"))):
        d = open(p, "rb").read()
        im = Image.new("RGB", (240, 240)); px = im.load()
        for i in range(240 * 240):
            v = d[2 * i] | (d[2 * i + 1] << 8)
            px[i % 240, i // 240] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
        im.save(p[:-4] + ".png"); os.remove(p)
except ImportError:
    print("Pillow not installed: .raw frames left in preview/out")
sys.exit(rc)
