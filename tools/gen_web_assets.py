#!/usr/bin/env python3
"""Generate web/web_assets.h (gzipped dashboard) from web/index.html."""
import gzip
import os
import re
import sys

LIMIT = 22528
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "web", "index.html")
DST = os.path.join(ROOT, "web", "web_assets.h")


def minify(text: str) -> str:
    # Strip HTML comments only (never /* */ or // inside JS/CSS) ...
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    # ... leading indentation and blank lines.
    lines = [ln.lstrip(" \t").rstrip() for ln in text.replace("\r\n", "\n").split("\n")]
    return "\n".join(ln for ln in lines if ln) + "\n"


def main() -> int:
    with open(SRC, "r", encoding="utf-8") as f:
        raw = f.read()
    data = minify(raw).encode("utf-8")
    gz = gzip.compress(data, compresslevel=9, mtime=0)
    rows = []
    for i in range(0, len(gz), 16):
        rows.append("  " + ", ".join("0x%02x" % b for b in gz[i:i + 16]))
    out = (
        "#pragma once\n"
        "#include <Arduino.h>\n"
        "static const uint32_t WEB_INDEX_GZ_LEN = %d;\n"
        "static const uint8_t WEB_INDEX_GZ[] PROGMEM = {\n%s\n};\n" % (len(gz), ",\n".join(rows))
    )
    with open(DST, "w", newline="\n") as f:
        f.write(out)
    print("index.html raw=%d minified=%d gz=%d (limit %d)" % (len(raw.encode("utf-8")), len(data), len(gz), LIMIT))
    if len(gz) > LIMIT:
        print("ERROR: gzipped index exceeds %d bytes" % LIMIT, file=sys.stderr)
        return 1
    print("wrote", DST)
    return 0


if __name__ == "__main__":
    sys.exit(main())
