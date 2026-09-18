#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pi Hub 上游数据源采集层（依据 PI_HUB_SPEC.md §7）。

提供：
  fetch_quote(symbol)    -> dict(price, prev, pct, high, low, ...) 或 None
  fetch_minute(symbol)   -> list[float] 分时价格序列 或 None
  fetch_kline(symbol, n) -> list[[open,close,high,low]] 正序 或 None
  fetch_weather(code)    -> dict(temp,humi,press,aqi,code,dn) 或 None
  build_spark(prices)    -> list[float] 固定 48 点

反爬要点：腾讯系 Referer=http://gu.qq.com/，天气网 Referer=http://www.weather.com.cn/；
都需要常规 UA，天气网 HTML 需带时间戳破缓存。
"""
import json
import re
import time
import requests

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120.0 Safari/537.36")
UA_IPHONE = ("Mozilla/5.0 (iPhone; CPU iPhone OS 15_0 like Mac OS X) "
             "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.0 Mobile/15E148 Safari/604.1")
TIMEOUT = 8

SPARK_LEN = 48


def _get(url, referer, ua=None, **kw):
    headers = {"User-Agent": ua or UA, "Referer": referer, "Connection": "close"}
    headers.update(kw.pop("headers", {}))
    return requests.get(url, headers=headers, timeout=TIMEOUT, **kw)


# ---------- 实时行情 ----------
def fetch_quote(symbol):
    """腾讯实时行情。返回 dict 或 None。
    字段索引：3=现价 4=昨收 31=涨跌额 32=涨跌幅 33=最高 34=最低
    """
    try:
        r = _get(f"http://qt.gtimg.cn/q={symbol}", "http://gu.qq.com/")
        r.encoding = "gbk"
        txt = r.text
        m = re.search(r'="([^"]*)"', txt)
        if not m:
            return None
        parts = m.group(1).split("~")
        if len(parts) < 35:
            return None

        def f(i):
            try:
                return float(parts[i])
            except (ValueError, IndexError):
                return None

        return {
            "symbol": symbol,
            "name": parts[1],
            "price": f(3),
            "prev": f(4),
            "change": f(31),
            "pct": f(32),
            "high": f(33),
            "low": f(34),
        }
    except Exception as e:
        print(f"[fetch_quote] {symbol} 失败: {e}")
        return None


# ---------- 分时 ----------
def fetch_minute(symbol):
    """腾讯分时。返回价格序列 list[float]（按时间正序）或 None。
    路径：data.{symbol}.data.data，每行 "0930 1285.15 320 xxx"，取第 2 段。
    """
    try:
        url = f"https://web.ifzq.gtimg.cn/appstock/app/minute/query?code={symbol}"
        r = _get(url, "http://gu.qq.com/")
        js = r.json()
        node = js.get("data", {}).get(symbol, {})
        rows = node.get("data", {}).get("data", [])
        prices = []
        for row in rows:
            seg = str(row).split()
            if len(seg) >= 2:
                try:
                    prices.append(float(seg[1]))
                except ValueError:
                    pass
        return prices or None
    except Exception as e:
        print(f"[fetch_minute] {symbol} 失败: {e}")
        return None


# ---------- 日K ----------
def fetch_kline(symbol, n=30):
    """腾讯日K。股票取 qfqday（前复权），指数取 day（不复权）。
    返回 [[open, close, high, low], ...] 按时间正序，最多 n 组 或 None。
    注意输出顺序为 [开,收,高,低]（契约 §2.2），而上游原始是 [日期,开,收,高,低,量]。
    """
    try:
        url = (f"https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?"
               f"param={symbol},day,,,{n},qfq")
        r = _get(url, "http://gu.qq.com/")
        js = r.json()
        node = js.get("data", {}).get(symbol, {})
        rows = node.get("qfqday") or node.get("day")
        if not rows:
            return None
        out = []
        for row in rows:
            # row = [日期, 开, 收, 高, 低, 量, ...]
            if len(row) >= 5:
                out.append([float(row[1]), float(row[2]), float(row[3]), float(row[4])])
        return out[-n:] or None
    except Exception as e:
        print(f"[fetch_kline] {symbol} 失败: {e}")
        return None


# ---------- 天气 ----------
def fetch_weather(city_code):
    """中国天气网。返回 dict(temp,humi,press,aqi,code,dn,weather) 或 None。
    实际页面形如：var dataSK ={...};var dataZS ={...}
    """
    try:
        ts = int(time.time() * 1000)
        url = f"http://d1.weather.com.cn/weather_index/{city_code}.html?_={ts}"
        r = _get(url, "http://www.weather.com.cn/", ua=UA_IPHONE)
        # 天气网页面为 UTF-8，但 requests 常猜成 ISO-8859-1，需显式指定
        r.encoding = "utf-8"
        html = r.text
        i = html.find("dataSK =")
        j = html.find(";var dataZS", i) if i >= 0 else -1
        if i < 0 or j < 0:
            return None
        seg = html[i + len("dataSK ="):j].strip().rstrip(";").strip()
        sk = json.loads(seg)

        def to_int(x):
            try:
                return int(float(str(x).rstrip("%")))
            except (ValueError, TypeError):
                return None

        def to_float(x):
            try:
                return float(x)
            except (ValueError, TypeError):
                return None

        # dataSK 里的 weathercode 形如 d02/n1；名字等中文字段用 utf-8 解码后正常
        wc = str(sk.get("weathercode", ""))
        dn = wc[0] if wc and wc[0] in ("d", "n") else None
        num = re.sub(r"^[dn]", "", wc)  # 去掉 d/n 前缀，保留完整数字部分
        code = int(num) if num.isdigit() else None

        return {
            "temp": to_float(sk.get("temp")),
            "humi": to_int(sk.get("SD")),
            "press": to_int(sk.get("qy")),
            "aqi": to_int(sk.get("aqi")),
            "code": code,
            "dn": dn,
            "weather": sk.get("weather"),
            "raw": sk,
        }
    except Exception as e:
        print(f"[fetch_weather] {city_code} 失败: {e}")
        return None


# ---------- spark 采样（契约 §2.1：按点索引等距抽样）----------
def build_spark(prices, n=SPARK_LEN):
    """把分时价格序列按点索引等距抽样到固定 n 点。
    index = round(i * N / n)，i∈[0,n)；N<n 时用末值向后填充。
    """
    if not prices:
        return []
    N = len(prices)
    out = []
    for i in range(n):
        idx = round(i * N / n)
        if idx >= N:
            idx = N - 1
        out.append(float(prices[idx]))
    return out


if __name__ == "__main__":
    # 自测
    print("=== quote sh600519 ===")
    print(fetch_quote("sh600519"))
    print("=== quote sh000001 (指数) ===")
    print(fetch_quote("sh000001"))
    print("=== minute sh600519 (前5) ===")
    mi = fetch_minute("sh600519")
    print(mi[:5] if mi else None, "共", len(mi) if mi else 0)
    print("=== spark ===")
    sp = build_spark(mi)
    print(f"长度={len(sp)} 前5={sp[:5]} 后5={sp[-5:]}")
    print("=== kline sh600519 (最后3) ===")
    kl = fetch_kline("sh600519", 30)
    print(kl[-3:] if kl else None, "共", len(kl) if kl else 0)
    print("=== kline 指数 sh000001 (fallback day) ===")
    kl2 = fetch_kline("sh000001", 5)
    print(kl2 if kl2 else None)
    print("=== weather 101120301 ===")
    print(fetch_weather("101120301"))
