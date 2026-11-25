#!/usr/bin/env python3
"""Builds and runs the host unit tests (needs a C++ compiler: g++/clang++, or `pip install ziglang`)."""
import os, shutil, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "..", "src")
cc = shutil.which("g++") or shutil.which("clang++")
cmd = [cc] if cc else [sys.executable, "-m", "ziglang", "c++"]

common = (
    "core/MarketClock.cpp", "core/JsonWriter.cpp", "core/FlatJson.cpp", "core/AppState.cpp",
    "config/AppConfig.cpp", "finance/Portfolio.cpp", "display/ViewSchedule.cpp", "core/Format.cpp",
    "finance/FinancialCalculator.cpp", "finance/JsonStream.cpp", "finance/YahooParser.cpp",
    "finance/ChartData.cpp")
suites = (("test_core", ()), ("test_sales", ("finance/SaleEngine.cpp",)))

rc = 0
for name, extra in suites:
    exe = os.path.join(here, name + (".exe" if os.name == "nt" else ""))
    files = [os.path.join(here, name + ".cpp")] + [os.path.join(src, p) for p in common + extra]
    subprocess.check_call(cmd + ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-fsanitize=undefined",
                                 "-o", exe] + files)
    rc |= subprocess.call([exe, os.path.join(here, "fixtures")])

# Dashboard integrity: the page is one inline script, so a duplicate top-level declaration ships a
# blank dashboard while every API endpoint still answers (fw 1.2.3 did exactly that). These need
# node; they are reported as skipped rather than silently omitted when node is missing.
node = shutil.which("node")
for js in ("web_ui_check.js", "bbands_check.js", "add_flow_check.js"):
    p = os.path.join(here, js)
    if not os.path.exists(p):
        continue
    if node:
        rc |= subprocess.call([node, p])
    else:
        print("note: node not on PATH - skipped", js)
sys.exit(rc)
