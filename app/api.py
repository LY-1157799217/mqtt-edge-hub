#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pi Hub Flask API（严格按 PI_HUB_SPEC.md §2 契约输出）。

接口：
  GET  /api/stock/{symbol}              单只行情 + 分时(48点 spark)
  GET  /api/stock/{symbol}/kline        日K ([[开,收,高,低], ...], 最多30)
  GET  /api/weather/{city_code}         天气
  POST /api/control/mode                控制 ESP32 (转发到 /set?mode=N)
  POST /api/control/brightness          控制 ESP32 亮度
  POST /api/control/set                 通用透传（白名单参数）→ /set?<params>

契约要点（不可改）：
  - 一律 HTTP，Connection: close，不用 chunked
  - JSON 字段名全小写、扁平、无嵌套
  - 响应体 < 4KB
  - 失败返回 HTTP 非 200（ESP32 只看状态码）
  - spark 固定 48 个 float；kl 每组 4 数且顺序 [开,收,高,低]、时间正序
  - 结果实时取自上游（短缓存），确保数据新鲜
"""
import json
import os
import sqlite3
import sys
import time

from flask import Flask, Response, jsonify, request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cache as cache_mod
from config import get as cfg

BASE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(BASE)
DB_PATH = os.environ.get("PIHUB_DB", os.path.join(PROJECT, "data", "pihub.db"))

app = Flask(__name__)
app.config["JSON_AS_ASCII"] = False

# 各接口数据新鲜度上限（秒），过期则实时刷新
QUOTE_TTL = 5
KLINE_TTL = 300
WEATHER_TTL = 600


def _db():
    c = sqlite3.connect(DB_PATH, timeout=10)
    c.row_factory = sqlite3.Row
    return c


def _resp(payload, status=200):
    """统一响应：JSON + Connection: close + Content-Length（绝不 chunked）。"""
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    r = Response(body, status=status, mimetype="application/json")
    r.headers["Connection"] = "close"
    r.headers["Content-Type"] = "application/json; charset=utf-8"
    r.headers["Content-Length"] = str(len(body.encode("utf-8")))
    return r


def _fail(status=502):
    """失败：非 200，空 body（ESP32 只看状态码）。"""
    r = Response(b"", status=status, mimetype="application/json")
    r.headers["Connection"] = "close"
    r.headers["Content-Length"] = "0"
    return r


# ---------------- §2.1 单只行情 + 分时 ----------------
def _build_quote_payload(symbol):
    row = None
    with _db() as c:
        row = c.execute("SELECT * FROM quote_cache WHERE symbol=?", (symbol,)).fetchone()
    fresh = row and (time.time() - row["updated_at"] <= QUOTE_TTL)
    if not fresh:
        ok, _ = cache_mod.update_quote_cache(symbol)
        if not ok:
            return None
        with _db() as c:
            row = c.execute("SELECT * FROM quote_cache WHERE symbol=?", (symbol,)).fetchone()
        if not row:
            return None
    spark = json.loads(row["spark"]) if row["spark"] else []
    # 契约兜底：spark 固定 48 个 float
    if len(spark) != 48:
        fill = spark[-1] if spark else (row["price"] or 0.0)
        spark = (list(spark) + [fill] * 48)[:48]
    return {
        "ok": 1,
        "price": round(float(row["price"]), 2) if row["price"] is not None else 0.0,
        "pct": round(float(row["pct"]), 2) if row["pct"] is not None else 0.0,
        "prev": round(float(row["prev"]), 2) if row["prev"] is not None else 0.0,
        "high": round(float(row["high"]), 2) if row["high"] is not None else 0.0,
        "low": round(float(row["low"]), 2) if row["low"] is not None else 0.0,
        "spark": [round(float(x), 2) for x in spark],
    }


@app.route("/api/stock/<symbol>")
def api_stock(symbol):
    try:
        payload = _build_quote_payload(symbol)
        if not payload:
            return _fail(502)
        return _resp(payload)
    except Exception as e:
        print(f"[api_stock] {symbol} 异常: {e}")
        return _fail(500)


# ---------------- §2.2 日K ----------------
def _kline_fetch_key(symbol):
    return f"kline_fetch_at:{symbol}"


def _kline_mark_fetched(symbol, ts):
    """记录日K上次成功刷新的 unix 秒（存 config 表）。

    为什么不用 kline_history.updated_at：该表没有这一列，而 init_db.py 只是
    executescript(schema.sql)，`CREATE TABLE IF NOT EXISTS` 不会给已存在的表加列
    —— 加列对老库静默无效。存 config 则无需迁移，老库新库都立刻生效。
    """
    try:
        with sqlite3.connect(DB_PATH, timeout=10) as c:
            c.execute("INSERT INTO config(key,value,updated_at) VALUES(?,?,?) "
                      "ON CONFLICT(key) DO UPDATE SET value=excluded.value, "
                      "updated_at=excluded.updated_at",
                      (_kline_fetch_key(symbol), str(ts), ts))
            c.commit()
    except Exception as e:
        print(f"[kline] 记录刷新时间失败: {e}")


def _build_kline_payload(symbol):
    with _db() as c:
        row = c.execute(
            "SELECT value FROM config WHERE key='kline_ttl'").fetchone()
        mark = c.execute("SELECT value FROM config WHERE key=?",
                         (_kline_fetch_key(symbol),)).fetchone()
        rows = c.execute(
            "SELECT date,open,close,high,low FROM kline_history WHERE symbol=? "
            "ORDER BY date ASC", (symbol,)).fetchall()
    try:
        ttl = int(row["value"]) if row else KLINE_TTL
    except (TypeError, ValueError):
        ttl = KLINE_TTL
    if ttl <= 0:
        ttl = KLINE_TTL
    try:
        age = time.time() - int(mark["value"]) if mark else None
    except (TypeError, ValueError):
        age = None
    # 过期判据只看「距上次成功刷新的秒数」，共两条：
    #   ① age is None —— 从未成功刷新过（新装/清库），必须取
    #   ② age >= ttl —— 超时重取。**当天这根K线随盘中一直在变**，只问"日期是不是今天"
    #      会让当天落第一行之后冻结到收盘（旧版就是这个缺陷）。
    # 不再单独判「最后一根日期 != 今天」：那会让停牌股（最新一根是几个月前）每次请求
    # 都去打一次上游，等于重试风暴；而重取也变不出今天这根。跨日/长假后开市由 ② 覆盖
    # ——上次刷新在昨天，age 必然远超 ttl。
    stale = (not rows) or (age is None) or (age >= ttl)
    if stale:
        ok, _ = cache_mod.update_kline(symbol, 30)
        if not ok and not rows:
            return None
        if ok:
            _kline_mark_fetched(symbol, int(time.time()))
        with _db() as c:
            rows = c.execute(
                "SELECT date,open,close,high,low FROM kline_history WHERE symbol=? "
                "ORDER BY date ASC", (symbol,)).fetchall()
    rows = rows[-30:]
    kl = [[round(float(r["open"]), 2), round(float(r["close"]), 2),
           round(float(r["high"]), 2), round(float(r["low"]), 2)] for r in rows]
    return {"ok": 1, "n": len(kl), "kl": kl}


@app.route("/api/stock/<symbol>/kline")
def api_kline(symbol):
    try:
        payload = _build_kline_payload(symbol)
        if not payload:
            return _fail(502)
        return _resp(payload)
    except Exception as e:
        print(f"[api_kline] {symbol} 异常: {e}")
        return _fail(500)


# ---------------- §2.3 天气 ----------------
def _build_weather_payload(city_code):
    row = None
    with _db() as c:
        row = c.execute("SELECT * FROM weather_cache WHERE city_code=?",
                        (city_code,)).fetchone()
    ttl = int(cfg("weather_cache_sec", WEATHER_TTL))
    fresh = row and (time.time() - row["updated_at"] <= ttl)
    if not fresh:
        ok, _ = cache_mod.update_weather(city_code)
        with _db() as c:
            row = c.execute("SELECT * FROM weather_cache WHERE city_code=?",
                            (city_code,)).fetchone()
        if not row:
            return None
    return {
        "ok": 1,
        "temp": round(float(row["temp"]), 1) if row["temp"] is not None else 0.0,
        "humi": int(row["humi"]) if row["humi"] is not None else 0,
        "press": int(row["press"]) if row["press"] is not None else 0,
        "aqi": int(row["aqi"]) if row["aqi"] is not None else 0,
        "code": int(row["code"]) if row["code"] is not None else 0,
    }


@app.route("/api/weather/<city_code>")
def api_weather(city_code):
    try:
        payload = _build_weather_payload(city_code)
        if not payload:
            return _fail(502)
        return _resp(payload)
    except Exception as e:
        print(f"[api_weather] {city_code} 异常: {e}")
        return _fail(500)


# ---------------- §2.4 控制 ESP32 ----------------
ESP32_TIMEOUT = 6.0  # ESP32 整屏重绘需几百毫秒，叠加热点抖动，超时须留足余量（CC 建议 ≥5s）


def _forward_set(params, attempts=2, delay=0.6):
    """转发到 ESP32 现有 /set 接口。返回 (ok, status)。
    网络抖动（手机热点下多设备争用）会导致偶发不响应，故延迟重试 attempts 次。
    注意：ESP32 web server 单次卡死时重试无效，故 attempts 不宜过大（避免阻塞调用方）。
    超时故意放宽到 6s：切模式要整屏重绘，超时太短会把 "ESP32 没问题但 Pi 等不及" 误报成 502。
    """
    ip = cfg("smalltv_ip", "")
    if not ip:
        return False, 503
    import requests as _rq
    last_err = None
    for i in range(attempts):
        try:
            r = _rq.get(f"http://{ip}/set", params=params, timeout=ESP32_TIMEOUT)
            if r.status_code == 200:
                return True, 200
            last_err = r.status_code
        except Exception as e:
            last_err = str(e)
        if i < attempts - 1:
            time.sleep(delay)
    print(f"[forward_set] {ip} 失败({attempts}次尝试): {last_err}")
    return False, 502


@app.route("/api/control/mode", methods=["POST"])
def api_control_mode():
    try:
        data = request.get_json(silent=True) or {}
        mode = int(data.get("mode"))
        if mode not in (0, 1, 2, 3):
            return _fail(400)
        ok, st = _forward_set({"mode": mode})
        return _resp({"ok": 1 if ok else 0, "mode": mode}, 200 if ok else 502)
    except Exception as e:
        print(f"[api_control_mode] 异常: {e}")
        return _fail(400)


@app.route("/api/control/brightness", methods=["POST"])
def api_control_brightness():
    try:
        data = request.get_json(silent=True) or {}
        val = int(data.get("value"))
        if not (0 <= val <= 100):
            return _fail(400)
        ok, st = _forward_set({"brightness": val})
        return _resp({"ok": 1 if ok else 0, "value": val}, 200 if ok else 502)
    except Exception as e:
        print(f"[api_control_brightness] 异常: {e}")
        return _fail(400)


# 通用透传白名单：只允许这些 ESP32 /set 参数键，防止任意参数注入。
# 新增指令只需在此登记参数名 + 在 command.py 加中文映射，无需再改转发层。
#   值域说明：
#     stockview  0分时图 / 1日K
#     auto_brightness  1开
#     wp   0无 / 1静态 / 2动态（壁纸模式）
#     idx  静态壁纸索引 0..2（配合 wp=1）
_SET_WHITELIST = {
    "stockview": (0, 1),
    "auto_brightness": (0, 1),
    "wp": (0, 2),
    "idx": (0, 2),
}


@app.route("/api/control/set", methods=["POST"])
def api_control_set():
    """通用透传：{"params": {"stockview": 1, "wp": 1, "idx": 0}} → ESP32 /set。
    仅允许白名单键；值一律转 int 并做区间校验。既有 mode/brightness 端点保持原样。"""
    try:
        data = request.get_json(silent=True) or {}
        params = data.get("params") or {}
        if not isinstance(params, dict) or not params:
            return _fail(400)
        clean = {}
        for k, v in params.items():
            if k not in _SET_WHITELIST:
                return _fail(400)
            iv = int(v)
            lo, hi = _SET_WHITELIST[k]
            if not (lo <= iv <= hi):
                return _fail(400)
            clean[k] = iv
        ok, st = _forward_set(clean)
        return _resp({"ok": 1 if ok else 0, "params": clean}, 200 if ok else 502)
    except Exception as e:
        print(f"[api_control_set] 异常: {e}")
        return _fail(400)


# ---------------- ESP32 自上报 IP（消除 smalltv_ip 手工维护） ----------------
# 场景：手机热点换段后，ESP32 的 WiFi IP 会变，Pi 的 config.smalltv_ip 立即作废。
# 设计（CC 提议、用户拍板）：ESP32 在【启动后首次成功取数】或【熔断器恢复在线】时，
# 主动 POST 一次自己的 WiFi.localIP() 到这里；不做轮询，只在状态迁移时发一次。
# 因果自洽：ESP32 能连上 Pi，本身就说明它的 piHubHost 已经对了；故用户只需改 ESP32 一处，
# 另一半（Pi→ESP32 控制方向）自动恢复。
def _update_smalltv_ip(ip):
    old = cfg("smalltv_ip", "")
    try:
        with sqlite3.connect(DB_PATH, timeout=10) as c:
            c.execute("INSERT INTO config(key,value) VALUES('smalltv_ip',?) "
                      "ON CONFLICT(key) DO UPDATE SET value=excluded.value", (ip,))
            c.commit()
    except Exception as e:
        print(f"[esp32_ip] 写库失败: {e}")
        return False, old
    if old != ip:
        print(f"[esp32_ip] smalltv_ip 更新: {old!r} -> {ip!r}")
    else:
        print(f"[esp32_ip] ESP32 上报 ip={ip}（与现有一致，无需变更）")
    return True, old


def _valid_ipv4(s, allow_loopback=True):
    if not s or not isinstance(s, str):
        return False
    parts = s.split(".")
    if len(parts) != 4:
        return False
    for p in parts:
        if not p.isdigit() or not (0 <= int(p) <= 255):
            return False
        if len(p) > 1 and p[0] == "0":
            return False
    if not allow_loopback and parts[0] == "127":
        return False
    return True


@app.route("/api/esp32_ip", methods=["POST"])
def api_esp32_ip():
    """ESP32 主动上报自己的 WiFi IP。body 可为 {"ip":"x.x.x.x"}（推荐），
    缺省时回退到请求来源 remote_addr（兼容固件不传 body 的实现）。
    注意：本端点不参与 §2 契约（ESP32 只调用、不解析响应），可自由扩展。
    """
    try:
        data = request.get_json(silent=True) or {}
        ip = (data.get("ip") or "").strip()
        if not _valid_ipv4(ip):
            # body 缺失/非法 → 回退请求源地址（兼容空 body 调用）
            if ip:
                print(f"[api_esp32_ip] body ip 非法({ip!r})，回退 remote_addr")
            ip = request.remote_addr or ""
        # ★ 安全：真 ESP32 上报的是其局域网地址，绝不可能是回环。
        # 若最终解析成 127.x（通常是本地/代理测试误发空 body），拒绝，避免把 smalltv_ip 污染成回环。
        if not _valid_ipv4(ip, allow_loopback=False):
            print(f"[api_esp32_ip] 拒绝：解析得非法或回环 IP ({ip!r})，返回 400")
            return _fail(400)
        ok, old = _update_smalltv_ip(ip)
        return _resp({"ok": 1 if ok else 0, "smalltv_ip": old}, 200 if ok else 500)
    except Exception as e:
        print(f"[api_esp32_ip] 异常: {e}")
        return _fail(400)


@app.route("/")
def index():
    return _resp({"ok": 1, "service": "pihub", "endpoints": [
        "/api/stock/<symbol>", "/api/stock/<symbol>/kline", "/api/weather/<city_code>",
        "/api/control/mode", "/api/control/brightness", "/api/control/set", "/api/esp32_ip"]})


if __name__ == "__main__":
    host = "0.0.0.0"  # IPv4 通配：含回环+当前 WiFi IP，换 IP 无需重启
    port = int(cfg("pihub_port", "5000"))
    app.run(host=host, port=port, threaded=True, debug=False)
