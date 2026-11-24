#!/usr/bin/env python3
"""Mock of the SP500 SmallTV HTTP API (docs/API.md) for developing web/index.html.

    python tools/mock_server.py            # http://127.0.0.1:8080/
    MOCK_SCENARIO=normal|stale|missing|offline|empty  MOCK_LAT=1.5  MOCK_PORT=8080
"""
import json
import os
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")
SCENARIO = os.environ.get("MOCK_SCENARIO", "normal")
LAT = float(os.environ.get("MOCK_LAT", "0"))
PORT = int(os.environ.get("MOCK_PORT", "8080"))
BOOT = time.time()
LOCK = threading.Lock()


def load(name):
    with open(os.path.join(FIX, name + ".json"), encoding="utf-8") as f:
        r = json.load(f)["chart"]["result"][0]
    ts, cl = r["timestamp"], r["indicators"]["quote"][0]["close"]
    pts = [(t, round(c, 2)) for t, c in zip(ts, cl) if c is not None]
    return r["meta"], pts


M1D, P1D = load("c1d")
M5D, P5D = load("c5d")
M1M, P1M = load("c1m")
META = M1D
LAST = META["regularMarketPrice"]
PREV = META["previousClose"]
QTIME = META["regularMarketTime"]


def now():
    return int(time.time())


def downsample(pts, mx):
    """Min/max preserving bucket downsampling."""
    if mx is None or len(pts) <= mx:
        return pts
    nb = max(mx // 2, 4)
    out = [pts[0]]
    inner = pts[1:-1]
    size = len(inner) / nb
    for b in range(nb):
        seg = inner[int(b * size):int((b + 1) * size)]
        if not seg:
            continue
        lo = min(seg, key=lambda p: p[1])
        hi = max(seg, key=lambda p: p[1])
        out.extend(sorted({lo, hi}, key=lambda p: p[0]))
    out.append(pts[-1])
    return out


def chart(rng, mx):
    src, base_mode = {"1d": (P1D, "prev"), "5d": (P5D, "first"), "1mo": (P1M, "first")}[rng]
    interval = {"1d": "5m", "5d": "15m", "1mo": "1d"}[rng]
    if SCENARIO in ("offline", "empty"):
        return {"range": rng, "interval": interval, "valid": False, "stale": False, "updated": 0, "count": 0,
                "first": None, "last": None, "base": None, "change": None, "changePct": None,
                "min": None, "max": None, "t": [], "v": []}
    pts = downsample(src, mx)
    vals = [p[1] for p in pts]
    base = PREV if base_mode == "prev" else vals[0]
    last = vals[-1]
    return {"range": rng, "interval": interval, "valid": True, "stale": SCENARIO == "stale",
            "updated": now() - (900 if SCENARIO == "stale" else 20), "count": len(pts),
            "first": vals[0], "last": last, "base": base, "change": round(last - base, 2),
            "changePct": round((last - base) / base * 100, 2), "min": min(vals), "max": max(vals),
            "t": [p[0] for p in pts], "v": vals}


def market():
    if SCENARIO in ("offline", "empty"):
        return {"symbol": "^GSPC", "valid": False, "stale": False, "last": None, "prevClose": None,
                "change": None, "changePct": None, "open": None, "high": None, "low": None,
                "status": "N/A", "sessionDate": None, "quoteTime": None, "quoteTimeText": None,
                "timezone": None, "tzName": None, "fetchedEpoch": None}
    s = SETTINGS
    q = QTIME
    if s["tzMode"] == "exchange":
        text = time.strftime("%b %d, %H:%M", time.gmtime(q - 18000)) + " EST"
    else:
        o = s["localOffsetMin"]
        text = time.strftime("%b %d, %H:%M", time.gmtime(q + o * 60)) + " UTC%+d:%02d" % (o // 60 if o >= 0 else -(-o // 60), abs(o) % 60)
    return {"symbol": "^GSPC", "valid": True, "stale": SCENARIO == "stale", "last": LAST, "prevClose": PREV,
            "change": round(LAST - PREV, 2), "changePct": round((LAST - PREV) / PREV * 100, 2),
            "open": 7150.39, "high": M1D["regularMarketDayHigh"], "low": M1D["regularMarketDayLow"],
            "status": "CLOSED", "sessionDate": time.strftime("%Y-%m-%d", time.gmtime(q - 18000)),
            "quoteTime": q, "quoteTimeText": text, "timezone": "EST", "tzName": "America/New_York",
            "fetchedEpoch": now() - (900 if SCENARIO == "stale" else 6)}


POSITIONS = [
    {"symbol": "^GSPC", "date": "2025-11-20", "qty": 39.17, "buy": 6382.10},
    {"symbol": "^GSPC", "date": "2026-02-10", "qty": 12.5, "buy": 6850.25},
    {"symbol": "^NDX", "date": "2026-05-04", "qty": 2.0, "buy": 20150.00},
]
if SCENARIO == "empty":
    POSITIONS.clear()
PRICES = {"^GSPC": LAST, "^SPX": LAST, "^NDX": 20840.12}


def pos_view():
    rows, inv, nav, priced, invalid, miss = [], 0.0, 0.0, 0, 0, 0
    for i, p in enumerate(POSITIONS):
        cost = round(p["qty"] * p["buy"], 2)
        price = PRICES.get(p["symbol"])
        quote = "fresh"
        if SCENARIO == "stale":
            quote = "stale"
        if SCENARIO in ("offline",) or (SCENARIO == "missing" and i == 1):
            quote, price = "missing", None
        if p["qty"] <= 0 or p["buy"] <= 0:
            quote, price = "invalid", None
        r = {"i": i, "id": p.get("id", i + 1), "symbol": p["symbol"], "date": p["date"], "qty": p["qty"],
             "oqty": p.get("oqty", p["qty"]), "buy": p["buy"], "price": price,
             "cost": cost, "nav": None, "pl": None, "returnPct": None, "quote": quote,
             "quoteEpoch": QTIME if price is not None else 0}
        if quote == "invalid":
            invalid += 1
            r["cost"] = None
        else:
            inv += cost
            if price is not None:
                priced += 1
                r["nav"] = round(p["qty"] * price, 2)
                r["pl"] = round(r["nav"] - cost, 2)
                r["returnPct"] = round(r["pl"] / cost * 100, 2)
                nav += r["nav"]
            else:
                miss += 1
        rows.append(r)
    complete = miss == 0
    s = {"invested": round(inv, 2), "nav": round(nav, 2) if complete and POSITIONS else None,
         "pl": None, "returnPct": None, "positions": len(POSITIONS), "priced": priced, "invalid": invalid,
         "complete": complete, "stale": SCENARIO == "stale"}
    if s["nav"] is not None:
        s["pl"] = round(nav - inv, 2)
        s["returnPct"] = round(s["pl"] / inv * 100, 2) if inv else None
    s["realisedPl"] = realised_view()["pl"]
    s["lifetimePl"] = round(s["pl"] + s["realisedPl"], 6) if s["pl"] is not None else None
    return rows, s


LIMITS = {"portfolioRefreshS": [15, 3600], "quoteRefreshS": [15, 3600], "chartRefreshS": [60, 3600],
          "pageRotateS": [3, 120], "localOffsetMin": [-720, 840]}
SETTINGS = {"portfolioRefreshS": 60, "quoteRefreshS": 60, "chartRefreshS": 300, "pageRotateS": 8,
            "defaultRange": "1d", "tzMode": "exchange", "localOffsetMin": 420, "hostname": "sp500"}
ADMIN = {"pass": ""}


def settings_view():
    d = dict(SETTINGS)
    d["adminPassSet"] = bool(ADMIN["pass"])
    d["limits"] = LIMITS
    return d


def apply_settings(b):
    new = dict(SETTINGS)
    for k, (lo, hi) in LIMITS.items():
        if k in b:
            v = b[k]
            if not isinstance(v, int) or isinstance(v, bool) or v < lo or v > hi:
                raise ValueError("%s must be %d..%d" % (k, lo, hi))
            new[k] = v
    if "defaultRange" in b:
        if b["defaultRange"] not in ("1d", "5d", "1mo"):
            raise ValueError("defaultRange must be 1d, 5d or 1mo")
        new["defaultRange"] = b["defaultRange"]
    if "tzMode" in b:
        if b["tzMode"] not in ("exchange", "local"):
            raise ValueError("tzMode must be exchange or local")
        new["tzMode"] = b["tzMode"]
    restart = False
    if "hostname" in b:
        if not isinstance(b["hostname"], str) or not re.fullmatch(r"[A-Za-z0-9]([A-Za-z0-9-]{0,29}[A-Za-z0-9])?", b["hostname"]):
            raise ValueError("hostname invalid")
        restart = b["hostname"] != SETTINGS["hostname"]
        new["hostname"] = b["hostname"]
    SETTINGS.update(new)
    if b.get("clearAdminPass"):
        ADMIN["pass"] = ""
    elif b.get("adminPass"):
        ADMIN["pass"] = b["adminPass"]
    return restart


# ---- wifi ----
WIFI = {"ssid": "home", "ip": "192.168.1.50", "rssi": -61, "saved": True, "mode": "sta",
        "connect": {"state": "idle", "ssid": "", "error": ""}, "scan_start": 0, "scan_done": time.time() - 12}
NETS = [("home", -52, True, 6), ("CoffeeShop Guest", -67, False, 1), ("TrueGigaTex_5G", -71, True, 36),
        ("AIS Fibre 2.4G", -76, True, 11), ("<img src=x onerror=alert(1)>", -80, True, 3),
        ("NETGEAR-Lab", -88, True, 9)]


def wifi_tick():
    c = WIFI["connect"]
    if c["state"] == "connecting" and time.time() >= WIFI.get("conn_end", 0):
        if WIFI.get("conn_pw") == "wrong":
            c.update(state="failed", error="authentication failed (wrong password?)")
        else:
            WIFI.update(ssid=c["ssid"], ip="192.168.1.77", saved=True, mode="sta")
            c["state"] = "ok"


def wifi_status():
    wifi_tick()
    ap = WIFI["mode"] != "sta"
    c = WIFI["connect"]
    scan = "scanning" if time.time() < WIFI["scan_start"] + 3 and WIFI["scan_start"] else "idle"
    state = "connecting" if c["state"] == "connecting" else ("disconnected" if SCENARIO == "offline" else "connected")
    return {"mode": WIFI["mode"], "state": state, "ssid": WIFI["ssid"], "ip": WIFI["ip"],
            "rssi": WIFI["rssi"], "mdns": "sp500.local", "saved": WIFI["saved"],
            "apSsid": "sp500-setup-A1B2C3" if ap else "", "apIp": "192.168.4.1" if ap else "",
            "connect": dict(c), "scan": {"state": scan, "ageSec": int(time.time() - WIFI["scan_done"]), "count": len(NETS)}}


def wifi_scan(refresh):
    if refresh and not (WIFI["scan_start"] and time.time() < WIFI["scan_start"] + 3):
        WIFI["scan_start"] = time.time()
    scanning = bool(WIFI["scan_start"]) and time.time() < WIFI["scan_start"] + 3
    if WIFI["scan_start"] and not scanning:
        WIFI["scan_done"] = WIFI["scan_start"] + 3
    nets = [] if (scanning and WIFI["scan_done"] < 0) else [{"ssid": s, "rssi": r + (int(time.time()) % 3), "enc": e, "ch": c} for s, r, e, c in NETS]
    return {"scanning": scanning, "ageSec": int(time.time() - WIFI["scan_done"]), "networks": nets}


# ---- positions job ----
JOB = {"state": "idle", "error": "", "symbol": "^GSPC", "updated": False, "end": 0, "p": None}


def job_view():
    if JOB["state"] == "pending" and time.time() >= JOB["end"]:
        if JOB["p"]["symbol"] not in PRICES:
            JOB.update(state="error", error="no price data for %s on %s" % (JOB["p"]["symbol"], JOB["p"]["date"]))
        else:
            upsert(JOB["p"])
            JOB.update(state="ok", updated=True)
    return {k: JOB[k] for k in ("state", "error", "symbol", "updated")}


def upsert(p):
    for e in POSITIONS:
        if e["symbol"] == p["symbol"] and e["date"] == p["date"]:
            e.update(qty=p["qty"], buy=p["buy"], oqty=p["qty"])
            return
    p.setdefault("id", max([x.get("id", 0) for x in POSITIONS] + [0]) + 1)
    p.setdefault("oqty", p["qty"])
    POSITIONS.append(p)


def close_on(date):
    best = None
    for t, c in P1M:
        if time.strftime("%Y-%m-%d", time.gmtime(t)) <= date:
            best = c
    return best or P1M[0][1]


# ---- sales (mirrors the device's /sells.jsonl behaviour) ----
import zlib

LEDGER = []                       # append-only records, oldest first
META = {"ver": 1, "appliedSeq": 0, "nextLot": 1, "nextTxn": 1}
SALEJOB = {"state": "idle", "error": "", "symbol": "", "date": "", "price": None, "end": 0}
for _i, _p in enumerate(POSITIONS, 1):
    _p["id"] = _i
    _p.setdefault("oqty", _p["qty"])


def token():
    lots = "|".join("%s,%s,%s,%s" % (p.get("id"), p["qty"], p["buy"], p["date"])
                    for p in sorted(POSITIONS, key=lambda x: x.get("id", 0)))
    return zlib.crc32(("%d,%d,%s" % (META["ver"], META["appliedSeq"], lots)).encode()) or 1


def reversed_seqs():
    return {r["refSeq"] for r in LEDGER if r["type"] == 1}


def realised_view():
    ids = reversed_seqs()
    sales = [r for r in LEDGER if r["type"] == 0]
    live = [r for r in sales if r["seq"] not in ids]
    years = {}
    for r in LEDGER:
        years[r["date"][:4]] = round(years.get(r["date"][:4], 0) + r["realised"], 6)
    last = live[-1] if live else None
    return {"valid": True, "pl": round(sum(r["realised"] for r in LEDGER), 6),
            "count": len(live), "reversed": len([r for r in LEDGER if r["type"] == 1]),
            "records": len(LEDGER), "limit": 2000, "warn": len(LEDGER) >= 1800, "full": len(LEDGER) >= 2000,
            "recovery": False, "badLine": False,
            "hasLast": last is not None, "lastId": last["id"] if last else "",
            "lastSymbol": last["symbol"] if last else "", "lastDate": last["date"] if last else "",
            "lastRealised": last["realised"] if last else None, "lastTs": last["ts"] if last else 0,
            "years": [{"year": int(y), "pl": p} for y, p in sorted(years.items(), reverse=True)]}


def sale_preview(b):
    sym = str(b.get("symbol", "")).upper()
    date = str(b.get("date", ""))
    if not re.fullmatch(r"[A-Z0-9^.=-]{1,15}", sym):
        raise ValueError("invalid symbol")
    if not re.fullmatch(r"\d{4}-\d\d-\d\d", date):
        raise ValueError("invalid date (YYYY-MM-DD)")
    if date > time.strftime("%Y-%m-%d"):
        raise ValueError("sale date is in the future")
    method, mode = b.get("method", "fifo"), b.get("mode", "qty")
    if method not in ("fifo", "specific"):
        raise ValueError("method must be fifo or specific")
    if mode not in ("qty", "thb", "all"):
        raise ValueError("mode must be thb, qty or all")
    lots = [p for p in POSITIONS if p["symbol"] == sym and p["qty"] > 0]
    if not lots:
        raise ValueError("no open position for that symbol")
    avail = sum(p["qty"] for p in lots)
    price = b.get("price")
    src = "manual"
    if price is None:
        if SALEJOB["state"] == "ok" and SALEJOB["symbol"] == sym and SALEJOB["date"] == date:
            price, src = SALEJOB["price"], "yahoo"
        else:
            return "pending"
    if not isinstance(price, (int, float)) or price <= 0:
        raise ValueError("execution price must be > 0")
    fee = b.get("fee", 0) or 0
    if not isinstance(fee, (int, float)) or fee < 0:
        raise ValueError("fee must be >= 0")
    if mode == "all":
        qty = avail
    elif mode == "thb":
        if not isinstance(b.get("amount"), (int, float)) or b["amount"] <= 0:
            raise ValueError("amount must be > 0")
        qty = b["amount"] / price
    else:
        if not isinstance(b.get("qty"), (int, float)) or b["qty"] <= 0:
            raise ValueError("quantity must be > 0")
        qty = b["qty"]
    if qty > avail + 1e-7:
        raise ValueError("only %.8g available" % avail)
    qty = min(qty, avail)
    if method == "specific":
        lot = next((p for p in lots if p.get("id") == b.get("lotId")), None)
        if lot is None:
            raise ValueError("lot not found for that symbol")
        if lot["qty"] < qty - 1e-7:
            raise ValueError("lot has only %.8g" % lot["qty"])
        picks = [(lot, qty)]
    else:
        need, picks = qty, []
        for p in lots:
            take = min(p["qty"], need)
            if take > 0:
                picks.append((p, take))
                need -= take
            if need <= 1e-9:
                break
        if need > 1e-7:
            raise ValueError("not enough quantity in the open lots")
    cost = sum(t * p["buy"] for p, t in picks)
    gross, net = qty * price, qty * price - fee
    return {"ok": True, "state": "ready",
            "preview": {"symbol": sym, "date": date, "price": price,
                        "refPrice": SALEJOB["price"] if src == "yahoo" else None, "priceSource": src,
                        "qty": qty, "available": avail, "fee": fee,
                        "gross": round(gross, 6), "net": round(net, 6), "cost": round(cost, 6),
                        "realised": round(net - cost, 6), "token": token(),
                        "alloc": [{"lot": p["id"], "date": p["date"], "buy": p["buy"], "oqty": p.get("oqty", p["qty"]),
                                   "before": p["qty"], "after": round(p["qty"] - t, 8)} for p, t in picks]}}


def sale_commit(b):
    if b.get("token") != token():
        raise ValueError("preview no longer matches the portfolio - preview again")
    pv = sale_preview(b)
    if pv == "pending":
        raise ValueError("execution price must be > 0")
    rec = {"seq": len(LEDGER) + 1, "ts": now(), "id": "TX-%06d" % META["nextTxn"], "type": 0,
           "symbol": pv["preview"]["symbol"], "date": pv["preview"]["date"], "qty": pv["preview"]["qty"],
           "price": pv["preview"]["price"], "refPrice": pv["preview"]["refPrice"],
           "priceSource": pv["preview"]["priceSource"], "fee": pv["preview"]["fee"],
           "gross": pv["preview"]["gross"], "net": pv["preview"]["net"], "cost": pv["preview"]["cost"],
           "realised": pv["preview"]["realised"], "alloc": pv["preview"]["alloc"]}
    for a in rec["alloc"]:
        lot = next((p for p in POSITIONS if p.get("id") == a["lot"]), None)
        if lot is not None:
            lot["qty"] = a["after"]
    POSITIONS[:] = [p for p in POSITIONS if p["qty"] > 1e-9]
    LEDGER.append(rec)
    META["ver"] += 1
    META["appliedSeq"] = rec["seq"]
    META["nextTxn"] += 1
    return {"ok": True, "state": "committed", "id": rec["id"], "seq": rec["seq"], "preview": pv["preview"]}


def sale_reverse(b):
    seq = b.get("seq")
    orig = next((r for r in LEDGER if r["seq"] == seq), None)
    if orig is None:
        raise ValueError("unknown transaction")
    if orig["type"] == 1:
        raise ValueError("only a sale can be reversed")
    if seq in reversed_seqs():
        raise ValueError("this sale is already reversed")
    rec = {"seq": len(LEDGER) + 1, "ts": now(), "id": "TX-%06d" % META["nextTxn"], "type": 1,
           "refSeq": orig["seq"], "refId": orig["id"], "symbol": orig["symbol"], "date": orig["date"],
           "qty": orig["qty"], "price": orig["price"], "refPrice": orig["refPrice"],
           "priceSource": orig["priceSource"], "fee": 0, "gross": -orig["gross"], "net": -orig["net"],
           "cost": -orig["cost"], "realised": -orig["realised"], "alloc": []}
    for a in orig["alloc"]:
        sold = a["before"] - a["after"]
        lot = next((p for p in POSITIONS if p.get("id") == a["lot"]), None)
        if lot is None:
            lot = {"id": a["lot"], "symbol": orig["symbol"], "date": a["date"], "qty": 0.0,
                   "oqty": a.get("oqty", a["before"]), "buy": a["buy"]}
            POSITIONS.append(lot)
        lot["qty"] = round(lot["qty"] + sold, 8)
        rec["alloc"].append({"lot": a["lot"], "date": a["date"], "buy": a["buy"], "oqty": a.get("oqty", a["before"]),
                             "before": lot["qty"] - sold, "after": lot["qty"]})
    LEDGER.append(rec)
    META["ver"] += 1
    META["appliedSeq"] = rec["seq"]
    META["nextTxn"] += 1
    return {"ok": True}


def sale_delete(b):
    seq = b.get("seq")
    rec = next((r for r in LEDGER if r["seq"] == seq), None)
    if rec is None:
        raise ValueError("record not found")
    sale_seq = rec["refSeq"] if rec["type"] == 1 else rec["seq"]
    if sale_seq not in reversed_seqs():
        sale_reverse({"seq": sale_seq})            # restores the position first, like the device
    rev_seq = next(r["seq"] for r in LEDGER if r["type"] == 1 and r["refSeq"] == sale_seq)
    gone = {sale_seq, rev_seq}
    def remap(n):
        return n - sum(1 for g in gone if g <= n) if n else 0
    LEDGER[:] = [r for r in LEDGER if r["seq"] not in gone]
    for r in LEDGER:
        r["seq"] = remap(r["seq"])
        if r.get("refSeq"):
            r["refSeq"] = remap(r["refSeq"])
    META["appliedSeq"] = remap(META["appliedSeq"])
    META["ver"] += 1
    return {"ok": True}

def sale_page(q):
    off = int(q.get("offset", ["0"])[0] or 0)
    lim = min(max(int(q.get("limit", ["8"])[0] or 8), 1), 8)
    sym = q.get("symbol", [""])[0]
    ids = reversed_seqs()
    rows = [r for r in reversed(LEDGER) if not sym or r["symbol"] == sym]
    page = [dict(r, status="reversal" if r["type"] == 1 else ("reversed" if r["seq"] in ids else "valid"),
                 refId=r.get("refId", "")) for r in rows[off:off + lim]]
    rv = realised_view()
    return {"symbol": sym, "total": len(rows), "offset": off, "count": len(page), "limit": lim,
            "records": rv["records"], "limitRecords": 2000, "warn": rv["warn"], "full": rv["full"],
            "pl": rv["pl"], "sales": page}


def sale_csv(sym):
    ids = reversed_seqs()
    out = ["seq,date,id,type,status,symbol,qty,price,fee,gross,net,cost,realised"]
    for r in LEDGER:
        if sym and r["symbol"] != sym:
            continue
        st = "reversal" if r["type"] == 1 else ("reversed" if r["seq"] in ids else "valid")
        out.append(",".join(str(x) for x in (r["seq"], r["date"], r["id"], r["type"], st, r["symbol"],
                                             r["qty"], r["price"], r["fee"], r["gross"], r["net"],
                                             r["cost"], r["realised"])))
    return "\r\n".join(out) + "\r\n"


ALIASES = {"SP500": "^GSPC", "S&P500": "^GSPC", "GSPC": "^GSPC", "SPX": "^SPX", "NDX": "^NDX", "NASDAQ": "^NDX"}


def status():
    off = SCENARIO == "offline"
    stale = SCENARIO in ("stale", "offline")
    return {"fw": "1.1.1", "uptime": int(time.time() - BOOT), "time": {"epoch": now(), "synced": True},
            "heap": {"free": 21000, "maxBlock": 17000, "frag": 12},
            "chip": {"id": "A1B2C3", "cpuMHz": 160, "flashKB": 4096, "sketchKB": 512, "freeSketchKB": 1019, "sdk": "3.1.2"},
            "fs": {"ok": True, "totalKB": 1024, "usedKB": 16 + len(POSITIONS)},
            "wifi": {"mode": "ap-rescue" if off else WIFI["mode"], "connected": not off, "ssid": WIFI["ssid"],
                     "ip": "192.168.4.1" if off else WIFI["ip"], "rssi": WIFI["rssi"], "mdns": "sp500.local"},
            "data": {"lastMarketOk": 0 if off else now() - (900 if stale else 6),
                     "lastError": "HTTP 429" if stale else "", "lastErrorEpoch": now() - 300 if stale else 0,
                     "quoteStale": stale, "portfolioStale": stale},
            "ota": {"active": False}}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        print("%s %s" % (self.command, self.path), *a[1:2])

    def send(self, code, body, ctype="application/json", extra=None):
        data = body if isinstance(body, bytes) else (body if isinstance(body, str) else json.dumps(body, separators=(",", ":"))).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype + ("; charset=utf-8" if ctype != "application/json" else ""))
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        if self.command == "GET" and self.path.startswith("/api/"):
            self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)

    def err(self, code, msg):
        self.send(code, {"ok": False, "error": msg})

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        return raw

    def do_GET(self):
        if LAT:
            time.sleep(LAT)
        u = urlparse(self.path)
        q = parse_qs(u.query)
        p = u.path
        with LOCK:
            if p in ("/", "/index.html"):
                with open(os.path.join(ROOT, "web", "index.html"), "rb") as f:
                    return self.send(200, f.read(), "text/html")
            if p == "/api/status":
                return self.send(200, status())
            if p == "/api/portfolio":
                rows, s = pos_view()
                return self.send(200, {"currency": "THB", "updated": 0 if SCENARIO == "offline" else now() - (900 if SCENARIO == "stale" else 15), "summary": s})
            if p == "/api/positions":
                rows, s = pos_view()
                return self.send(200, {"currency": "THB", "summary": s, "positions": rows})
            if p == "/api/market":
                return self.send(200, market())
            if p == "/api/chart":
                r = q.get("range", [""])[0]
                if r not in ("1d", "5d", "1mo"):
                    return self.err(400, "range must be 1d, 5d or 1mo")
                mx = q.get("max", [None])[0]
                mx = max(int(mx), 8) if mx and mx.isdigit() else None
                return self.send(200, chart(r, mx))
            if p == "/api/wifi/status":
                return self.send(200, wifi_status())
            if p == "/api/wifi/scan":
                return self.send(200, wifi_scan(q.get("refresh", ["0"])[0] == "1" or not WIFI["scan_start"]))
            if p == "/api/settings":
                return self.send(200, settings_view())
            if p == "/api/positions/job":
                return self.send(200, job_view())
            if p == "/api/realised":
                return self.send(200, realised_view())
            if p == "/api/sales":
                return self.send(200, sale_page(q))
            if p == "/api/sales/export":
                data = sale_csv(q.get("symbol", [""])[0]).encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/csv; charset=utf-8")
                self.send_header("Content-Disposition", 'attachment; filename="sells.csv"')
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                return self.wfile.write(data)
            if p == "/api/sales/job":
                if SALEJOB["state"] == "pending" and time.time() >= SALEJOB["end"]:
                    if SALEJOB["symbol"] in PRICES:
                        SALEJOB.update(state="ok", price=close_on(SALEJOB["date"]))
                    else:
                        SALEJOB.update(state="error", error="no price data for %s on %s" % (SALEJOB["symbol"], SALEJOB["date"]))
                return self.send(200, {k: SALEJOB[k] for k in ("state", "error", "symbol", "date", "price")})
            if p.startswith("/api/"):
                return self.err(404, "not found")
        self.err(404, "not found")

    def do_POST(self):
        if LAT:
            time.sleep(LAT)
        p = urlparse(self.path).path
        raw = self.body()
        if p == "/update":
            print("OTA upload: %d bytes" % len(raw))
            time.sleep(1)
            return self.send(200, "OK, rebooting", "text/plain")
        try:
            b = json.loads(raw or b"{}")
            assert isinstance(b, dict)
        except Exception:
            return self.err(400, "invalid JSON")
        with LOCK:
            if p == "/api/settings":
                try:
                    restart = apply_settings(b)
                except ValueError as e:
                    return self.err(400, str(e))
                r = {"ok": True, "settings": settings_view()}
                if restart:
                    r["restart"] = True
                return self.send(200, r)
            if p == "/api/refresh":
                if b.get("what") not in ("all", "quote", "charts", "portfolio"):
                    return self.err(400, "bad what")
                return self.send(202, {"ok": True})
            if p == "/api/cache/clear":
                return self.send(200, {"ok": True})
            if p == "/api/positions":
                sym = str(b.get("symbol", "")).upper()
                sym = ALIASES.get(sym, sym)
                if not re.fullmatch(r"[A-Z0-9^.=-]{1,15}", sym):
                    return self.err(400, "invalid symbol")
                if not re.fullmatch(r"\d{4}-\d\d-\d\d", str(b.get("date", ""))):
                    return self.err(400, "date must be YYYY-MM-DD")
                if JOB["state"] == "pending" and time.time() < JOB["end"]:
                    return self.err(409, "another lookup is pending")
                new = not any(e["symbol"] == sym and e["date"] == b["date"] for e in POSITIONS)
                if new and len(POSITIONS) >= 24:
                    return self.err(400, "max 12 positions")
                if "thb" in b:
                    if not isinstance(b["thb"], (int, float)) or b["thb"] <= 0:
                        return self.err(400, "thb must be > 0")
                    buy = close_on(b["date"])
                    JOB.update(state="pending", error="", symbol=sym, updated=False, end=time.time() + 2.5,
                               p={"symbol": sym, "date": b["date"], "qty": round(b["thb"] / buy, 6), "buy": buy})
                    return self.send(202, {"ok": True, "state": "pending"})
                q, buy = b.get("qty"), b.get("buy")
                if not all(isinstance(x, (int, float)) and x > 0 for x in (q, buy)):
                    return self.err(400, "qty and buy must be > 0")
                upsert({"symbol": sym, "date": b["date"], "qty": q, "buy": buy})
                return self.send(200, {"ok": True, "state": "ok"})
            if p == "/api/positions/delete":
                i = b.get("index")
                if not isinstance(i, int) or not 0 <= i < len(POSITIONS):
                    return self.err(400, "bad index")
                POSITIONS.pop(i)
                return self.send(200, {"ok": True})
            if p == "/api/sales/preview":
                if not b.get("price") and not (SALEJOB["state"] == "ok" and SALEJOB["symbol"] == str(b.get("symbol", "")).upper()
                                               and SALEJOB["date"] == b.get("date")):
                    if SALEJOB["state"] == "pending" and time.time() < SALEJOB["end"]:
                        return self.err(409, "a price lookup is already running")
                    SALEJOB.update(state="pending", error="", symbol=str(b.get("symbol", "")).upper(),
                                   date=str(b.get("date", "")), price=None, end=time.time() + 2.5)
                    return self.send(202, {"ok": True, "state": "pending"})
                try:
                    r = sale_preview(b)
                except ValueError as e:
                    return self.err(400, str(e))
                if r == "pending":
                    return self.err(400, "execution price must be > 0")
                return self.send(200, dict(r, ok=True))
            if p == "/api/sales/confirm":
                try:
                    r = sale_commit(b)
                except ValueError as e:
                    return self.err(409 if "preview" in str(e) else 400, str(e))
                return self.send(200, r)
            if p == "/api/sales/delete":
                try:
                    r = sale_delete(b)
                except ValueError as e:
                    return self.err(404 if "not found" in str(e) else 400, str(e))
                return self.send(200, r)
            if p == "/api/sales/reverse":
                try:
                    r = sale_reverse(b)
                except ValueError as e:
                    return self.err(400, str(e))
                return self.send(200, r)
            if p == "/api/wifi/connect":
                if not b.get("ssid"):
                    return self.err(400, "ssid required")
                WIFI["connect"] = {"state": "connecting", "ssid": b["ssid"], "error": ""}
                WIFI["conn_end"] = time.time() + 3
                WIFI["conn_pw"] = b.get("password", "")
                return self.send(202, {"ok": True, "state": "connecting"})
            if p == "/api/wifi/reconnect":
                return self.send(200, {"ok": True})
            if p == "/api/wifi/forget":
                if b.get("confirm") != "FORGET":
                    return self.err(400, "confirm must be FORGET")
                WIFI.update(saved=False, mode="ap-setup", ssid="", ip="192.168.4.1", rssi=None)
                return self.send(200, {"ok": True})
            if p in ("/api/system/restart", "/api/system/reset-settings", "/api/system/factory-reset"):
                need = {"/api/system/reset-settings": "RESET", "/api/system/factory-reset": "FACTORY"}.get(p)
                if need and b.get("confirm") != need:
                    return self.err(400, "confirm must be " + need)
                return self.send(200, {"ok": True})
        self.err(404, "not found")


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    print("mock SP500 server on http://127.0.0.1:%d/ scenario=%s latency=%.1fs" % (PORT, SCENARIO, LAT))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
