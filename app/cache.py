#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pi Hub 缓存写入层：把采集到的数据落 SQLite（依据 §6 步骤 3）。

  update_quote_cache(symbol)   采集行情+分时 -> quote_cache
  update_kline(symbol, n)      采集日K -> kline_history
  update_weather(city_code)    采集天气 -> weather_cache
"""
import json
import os
import sqlite3
import time

from datasource import (fetch_quote, fetch_minute,
                        fetch_weather, build_spark)

BASE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(BASE)
DB_PATH = os.environ.get("PIHUB_DB", os.path.join(PROJECT, "data", "pihub.db"))


def _conn():
    c = sqlite3.connect(DB_PATH, timeout=10)
    c.row_factory = sqlite3.Row
    return c


def update_quote_cache(symbol):
    """返回 (ok, data)。ok 为 True 表示行情拿到并写入。"""
    q = fetch_quote(symbol)
    if not q or q.get("price") is None:
        return False, None
    prices = fetch_minute(symbol)
    spark = build_spark(prices) if prices else []
    if len(spark) < 48:  # 兜底：用现价补齐 48 点（契约 §2.1 固定长度）
        fill = spark[-1] if spark else q["price"]
        spark = (spark + [fill] * 48)[:48]
    now = int(time.time())
    with _conn() as c:
        c.execute("""
            INSERT INTO quote_cache(symbol, price, pct, prev, high, low, spark, updated_at)
            VALUES (?,?,?,?,?,?,?,?)
            ON CONFLICT(symbol) DO UPDATE SET
              price=excluded.price, pct=excluded.pct, prev=excluded.prev,
              high=excluded.high, low=excluded.low, spark=excluded.spark,
              updated_at=excluded.updated_at
        """, (symbol, q["price"], q["pct"], q["prev"], q["high"], q["low"],
              json.dumps(spark), now))
    return True, {"symbol": symbol, "name": q.get("name"), "price": q["price"],
                  "pct": q["pct"], "spark_len": len(spark)}


def update_kline(symbol, n=30):
    """采集日K -> kline_history。返回 (ok, data)。

    ok 只在【真的解析出并写入了行】时为 True。
    原先这里先调 fetch_kline()、再重取一次带日期的 —— 同一个 URL、同一套解析，白打
    两次上游；而且第二次请求失败时把 rows 置空却仍 return True，上层（api.py 按
    kline_ttl 判过期）会把"没取到"记成"刚刷新过"，反而让旧数据顶满一个 TTL。
    腾讯这个接口本来就同时给日期和价格，一次请求就够。
    """
    from datasource import _get
    try:
        url = (f"https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?"
               f"param={symbol},day,,,{n},qfq")
        r = _get(url, "http://gu.qq.com/")
        node = r.json().get("data", {}).get(symbol, {})
        rows = node.get("qfqday") or node.get("day") or []
    except Exception as e:
        print(f"[update_kline] {symbol} 上游失败: {e}")
        return False, None
    if not rows:
        print(f"[update_kline] {symbol} 上游无数据")
        return False, None
    wrote = 0
    with _conn() as c:
        for row in rows:
            # row = [日期, 开, 收, 高, 低, 量, ...]
            if len(row) >= 5:
                c.execute("""
                    INSERT INTO kline_history(symbol, date, open, close, high, low)
                    VALUES (?,?,?,?,?,?)
                    ON CONFLICT(symbol, date) DO UPDATE SET
                      open=excluded.open, close=excluded.close,
                      high=excluded.high, low=excluded.low
                """, (symbol, row[0], float(row[1]), float(row[2]),
                      float(row[3]), float(row[4])))
                wrote += 1
    if not wrote:
        return False, None
    return True, {"symbol": symbol, "rows": wrote}


def update_weather(city_code):
    w = fetch_weather(city_code)
    if not w or w.get("temp") is None:
        return False, None
    now = int(time.time())
    with _conn() as c:
        c.execute("""
            INSERT INTO weather_cache(city_code, temp, humi, press, aqi, code, dn, raw, updated_at)
            VALUES (?,?,?,?,?,?,?,?,?)
            ON CONFLICT(city_code) DO UPDATE SET
              temp=excluded.temp, humi=excluded.humi, press=excluded.press,
              aqi=excluded.aqi, code=excluded.code, dn=excluded.dn,
              raw=excluded.raw, updated_at=excluded.updated_at
        """, (city_code, w["temp"], w["humi"], w["press"], w["aqi"],
              w["code"], w["dn"], json.dumps(w.get("raw", {}), ensure_ascii=False), now))
    return True, {"city_code": city_code, "temp": w["temp"], "code": w["code"], "dn": w["dn"]}


if __name__ == "__main__":
    print("== 行情 ==")
    print(update_quote_cache("sh600519"))
    print(update_quote_cache("sh000001"))
    print("== 日K ==")
    print(update_kline("sh600519", 30))
    print(update_kline("sh000001", 30))
    print("== 天气 ==")
    print(update_weather("101120301"))
