#!/usr/bin/env python3
"""Builds and runs the host unit tests (needs a C++ compiler: g++/clang++, or `pip install ziglang`)."""
import os, shutil, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "..", "src")
cc = shutil.which("g++") or shutil.which("clang++")
cmd = [cc] if cc else [sys.executable, "-m", "ziglang", "c++"]
exe = os.path.join(here, "test_core.exe" if os.name == "nt" else "test_core")
files = [os.path.join(here, "test_core.cpp")] + [os.path.join(src, p) for p in (
    "core/MarketClock.cpp", "core/JsonWriter.cpp", "core/FlatJson.cpp", "core/AppState.cpp", "config/AppConfig.cpp", "finance/Portfolio.cpp", "display/ViewSchedule.cpp", "core/Format.cpp", "finance/FinancialCalculator.cpp",
    "finance/JsonStream.cpp", "finance/YahooParser.cpp", "finance/ChartData.cpp")]
subprocess.check_call(cmd + ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-fsanitize=undefined", "-o", exe] + files)
sys.exit(subprocess.call([exe, os.path.join(here, "fixtures")]))
