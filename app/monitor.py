#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pi Hub 监控线程（依据 PI_HUB_SPEC.md §4）。

职责（唯一"主动"环节）：
  1. 轮询自选股行情（交易时段每 10s，非交易时段每 300s / 直接休眠）
  2. 触发条件：涨停 / 跌停 / 异动（|pct| 超自定义阈值）
  3. 冷却去重：同一 (symbol, alert_type) 命中后 1 小时内不重复推送（持久化 SQLite，重启不重推）
  4. 失败不静默：上游连续失败 N 次记日志
  5. MQTT publish 到 hub/alert/stock/*（QoS 1，不 retained）

主题（§3）：
  hub/alert/stock/limit_up    {"symbol","label","price","pct"}
  hub/alert/stock/limit_down  {"symbol","label","price","pct"}
  hub/alert/stock/surge       {"symbol","label","price","pct","threshold"}

运行：cd /home/pi/cooperate/pihub && .venv/bin/python app/monitor.py
自测：加 --once 跑一轮就退出；加 --force 忽略交易时段（便于验收）。
"""
import argparse
import json
import os
import sqlite3
import sys
import time
from datetime import datetime, time as dtime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paho.mqtt.client as mqtt
from config import get as cfg
import datasource as ds

BASE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(BASE)
DB_PATH = os.environ.get("PIHUB_DB", os.path.join(PROJECT, "data", "pihub.db"))

TOPIC = "hub/alert/stock/{t}"

# 法定节假日休市（§8）——仅靠"周一到周五"会在长假空转并推陈旧数据。
# 维护近 2 年 A 股休市日；上游交易日历接入前先用手工列表兜底。
HOLIDAYS = {
    # 2025
    "2025-01-01", "2025-01-28", "2025-01-29", "2025-01-30", "2025-01-31",
    "2025-02-03", "2025-02-04", "2025-04-04", "2025-04-07", "2025-05-01",
    "2025-05-02", "2025-05-05", "2025-06-02", "2025-10-01", "2025-10-02",
    "2025-10-03", "2025-10-06", "2025-10-07", "2025-10-08",
    # 2026
    "2026-01-01", "2026-01-02", "2026-02-16", "2026-02-17", "2026-02-18",
    "2026-02-19", "2026-02-20", "2026-02-23", "2026-02-24", "2026-04-06",
    "2026-05-01", "2026-05-04", "2026-05-05", "2026-06-19", "2026-10-01",
    "2026-10-02", "2026-10-05", "2026-10-06", "2026-10-07", "2026-10-08",
}


# ---------------- 交易时段 ----------------
# 交易时段 09:15-11:35 / 12:55-15:05（§4，含集合竞价前后缓冲）
MORNING = (dtime(9, 15), dtime(11, 35))
AFTERNOON = (dtime(12, 55), dtime(15, 5))


def is_trading_day(now=None):
    now = now or datetime.now()
    if now.weekday() >= 5:          # 周六周日
        return False
    if now.strftime("%Y-%m-%d") in HOLIDAYS:
        return False
    return True


def is_trading_time(now=None):
    now = now or datetime.now()
    if not is_trading_day(now):
        return False
    t = now.time()
    return (MORNING[0] <= t <= MORNING[1]) or (AFTERNOON[0] <= t <= AFTERNOON[1])


def is_gem_or_star(symbol):
    """创业板 sz30* / 科创板 sh68* —— 涨跌停 ±20%。"""
    return symbol.startswith("sz30") or symbol.startswith("sh68")


# ---------------- DB ----------------
def _conn():
    c = sqlite3.connect(DB_PATH, timeout=10)
    c.row_factory = sqlite3.Row
    return c


def load_symbols():
    with _conn() as c:
        rows = c.execute(
            "SELECT symbol, label, kind FROM symbols WHERE enabled=1 ORDER BY sort"
        ).fetchall()
    return [dict(r) for r in rows]


def cooldown_ok(symbol, alert_type, cooldown_sec):
    """冷却检查：距上次同 (symbol, alert_type) 推送 >= cooldown_sec 才允许再推。"""
    now = int(time.time())
    with _conn() as c:
        row = c.execute(
            "SELECT last_fired FROM alert_cooldown WHERE symbol=? AND alert_type=?",
            (symbol, alert_type),
        ).fetchone()
    if row is None:
        return True
    return (now - row["last_fired"]) >= cooldown_sec


def mark_fired(symbol, alert_type, payload):
    now = int(time.time())
    with _conn() as c:
        c.execute("""
            INSERT INTO alert_cooldown(symbol, alert_type, last_fired, payload)
            VALUES (?,?,?,?)
            ON CONFLICT(symbol, alert_type) DO UPDATE SET
              last_fired=excluded.last_fired, payload=excluded.payload
        """, (symbol, alert_type, now, json.dumps(payload, ensure_ascii=False)))


# ---------------- 阈值 ----------------
def _f(key, default):
    try:
        return float(cfg(key))
    except (TypeError, ValueError):
        return default


def _i(key, default):
    try:
        return int(float(cfg(key)))
    except (TypeError, ValueError):
        return default


def thresholds(symbol):
    """返回 (涨停线, 跌停线)。创业板/科创板用 ±19.8，其余 ±9.8。"""
    if is_gem_or_star(symbol):
        return _f("limit_up_pct_gem", 19.8), _f("limit_down_pct_gem", -19.8)
    return _f("limit_up_pct", 9.8), _f("limit_down_pct", -9.8)


# ---------------- MQTT ----------------
def make_client():
    host = cfg("mqtt_host") or "127.0.0.1"
    port = _i("mqtt_port", 1883)
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="pihub-monitor")
    client.connect(host, port, keepalive=30)
    client.loop_start()
    return client


def publish(client, alert_type, symbol, label, price, pct, threshold=None):
    payload = {"symbol": symbol, "label": label,
               "price": round(float(price), 4), "pct": round(float(pct), 4)}
    if threshold is not None:
        payload["threshold"] = threshold
    topic = TOPIC.format(t=alert_type)
    # QoS 1，不 retained（§3）
    info = client.publish(topic, json.dumps(payload, ensure_ascii=False), qos=1, retain=False)
    info.wait_for_publish(timeout=3)
    print(f"[alert] {topic} -> {payload}")
    return payload


# ---------------- 主循环 ----------------
def check_symbol(client, sym, cooldown_sec, surge_threshold, log):
    symbol, label = sym["symbol"], sym.get("label") or sym["symbol"]
    q = ds.fetch_quote(symbol)
    if not q or q.get("price") is None or q.get("pct") is None:
        return False
    price, pct = q["price"], q["pct"]
    up, down = thresholds(symbol)

    fired = []
    if pct >= up:
        if cooldown_ok(symbol, "limit_up", cooldown_sec):
            p = publish(client, "limit_up", symbol, label, price, pct)
            mark_fired(symbol, "limit_up", p)
            fired.append("limit_up")
    elif pct <= down:
        if cooldown_ok(symbol, "limit_down", cooldown_sec):
            p = publish(client, "limit_down", symbol, label, price, pct)
            mark_fired(symbol, "limit_down", p)
            fired.append("limit_down")
    elif abs(pct) >= surge_threshold:
        if cooldown_ok(symbol, "surge", cooldown_sec):
            p = publish(client, "surge", symbol, label, price, pct, threshold=surge_threshold)
            mark_fired(symbol, "surge", p)
            fired.append("surge")

    log(f"[poll] {symbol} {label} price={price} pct={pct}% "
        f"(up>={up} down<={down} surge>={surge_threshold}) fired={fired or '-'}")
    return True


def run(once=False, force=False):
    log_path = os.path.join(PROJECT, "logs", "monitor.log")
    os.makedirs(os.path.dirname(log_path), exist_ok=True)

    def log(msg):
        line = f"{datetime.now().strftime('%Y-%m-%d %H:%M:%S')} {msg}"
        print(line, flush=True)
        # systemd 下 stdout 已 append 到 monitor.log，避免重复写；手动跑(TTY)时自行落盘
        if sys.stdout.isatty():
            try:
                with open(log_path, "a", encoding="utf-8") as f:
                    f.write(line + "\n")
            except Exception:
                pass

    cooldown_sec = _i("alert_cooldown_sec", 3600)
    surge_threshold = _f("surge_threshold", 5.0)
    idle_sec = _i("poll_interval_idle", 300)
    trading_sec = _i("poll_interval_trading", 10)
    fail_threshold = _i("upstream_fail_threshold", 5)

    symbols = load_symbols()
    log(f"[start] monitor 启动: symbols={[s['symbol'] for s in symbols]} "
        f"cooldown={cooldown_sec}s surge={surge_threshold}% "
        f"trading={trading_sec}s idle={idle_sec}s")

    client = None
    try:
        client = make_client()
        log(f"[start] MQTT 已连接 {cfg('mqtt_host')}:{cfg('mqtt_port')}")
    except Exception as e:
        log(f"[start] MQTT 连接失败: {e}（告警将只记日志，等 MQTT 恢复需重启）")

    consec_fail = 0
    while True:
        try:
            trading = force or is_trading_time()
            if trading:
                ok_any = False
                if client is not None:
                    for s in symbols:
                        try:
                            if check_symbol(client, s, cooldown_sec, surge_threshold, log):
                                ok_any = True
                        except Exception as e:
                            log(f"[poll] {s['symbol']} 异常: {e}")
                else:
                    log("[poll] MQTT 未连接，跳过本轮")
                    ok_any = True  # 不把 MQTT 断连算作上游失败

                if ok_any:
                    consec_fail = 0
                else:
                    consec_fail += 1
                    if consec_fail >= fail_threshold:
                        log(f"[warn] 上游连续失败 {consec_fail} 次（阈值 {fail_threshold}），"
                            f"疑似采集挂了")
                        consec_fail = 0  # 记一次告警后重置，避免刷屏
            else:
                # 非交易时段：休眠一轮（不轮询，避免空转与陈旧数据）
                pass
        except Exception as e:
            log(f"[loop] 未捕获异常: {e}")

        if once:
            break

        sleep_sec = trading_sec if (force or is_trading_time()) else idle_sec
        time.sleep(sleep_sec)

    if client is not None:
        try:
            client.loop_stop()
            client.disconnect()
        except Exception:
            pass
    log("[stop] monitor 退出")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--once", action="store_true", help="只跑一轮就退出")
    ap.add_argument("--force", action="store_true", help="忽略交易时段（验收用）")
    args = ap.parse_args()
    run(once=args.once, force=args.force)
