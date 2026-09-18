#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pi Hub 配置读取：env 优先，其次 SQLite config 表，最后默认值。"""
import os
import sqlite3

BASE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(BASE)
DB_PATH = os.environ.get("PIHUB_DB", os.path.join(PROJECT, "data", "pihub.db"))

DEFAULTS = {
    "wecom_webhook_key": "",
    "smalltv_ip": "",
    "mqtt_host": "127.0.0.1",
    "mqtt_port": "1883",
    "pihub_host": "127.0.0.1",  # 仅作占位；服务端绑定已改为 0.0.0.0（免疫换 IP），客户端一律走回环
    "pihub_port": "5000",
}


def _load_env_file():
    """轻量 .env 解析（不引入 python-dotenv 依赖）。"""
    out = {}
    path = os.path.join(PROJECT, ".env")
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    return out


def get(key, default=None):
    """按优先级取配置：环境变量 > .env > SQLite config 表 > DEFAULTS。"""
    if key in os.environ and os.environ[key] != "":
        return os.environ[key]
    env = _load_env_file()
    if key in env and env[key] != "":
        return env[key]
    try:
        conn = sqlite3.connect(DB_PATH)
        row = conn.execute("SELECT value FROM config WHERE key=?", (key,)).fetchone()
        conn.close()
        if row and row[0] != "":
            return row[0]
    except Exception:
        pass
    return DEFAULTS.get(key, default)


if __name__ == "__main__":
    for k in list(DEFAULTS.keys()) + ["poll_interval_trading", "surge_threshold"]:
        print(f"{k} = {get(k)!r}")
