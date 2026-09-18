#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pi Hub 数据库初始化 / schema 应用（幂等，可重复运行）。
用法: python3 init_db.py
"""
import os
import sqlite3
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.environ.get("PIHUB_DB", os.path.join(BASE, "..", "data", "pihub.db"))
DB_PATH = os.path.abspath(DB_PATH)
SCHEMA = os.path.join(BASE, "schema.sql")

# 默认配置（阈值可调，写进 config 表）
DEFAULT_CONFIG = {
    "poll_interval_trading": "10",      # 交易时段轮询秒数
    "poll_interval_idle": "300",        # 非交易时段轮询秒数
    "limit_up_pct": "9.8",              # 主板涨停阈值
    "limit_up_pct_gem": "19.8",         # 创业板/科创板涨停阈值
    "limit_down_pct": "-9.8",
    "limit_down_pct_gem": "-19.8",
    "surge_threshold": "5.0",           # 异动自定义幅度
    "alert_cooldown_sec": "3600",       # 告警冷却 1 小时
    "weather_cache_sec": "600",         # 天气缓存 10 分钟
    "upstream_fail_threshold": "5",     # 上游连续失败 N 次记日志/告警
    "smalltv_ip": "",                   # ESP32 小电视 IP（§2.4 控制用，待填）
    # 企微智能机器人（长连接 API 模式，§7）
    "wecom_bot_id": "",                 # 智能机器人 BotID（凭证在 .env，勿入 git）
    "wecom_chat_id": "",                # 告警推送目标群 chatid（首次收到回调后落库）
    "wecom_chat_type": "2",             # 1 单聊 / 2 群聊
}


def main():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    if not os.path.exists(SCHEMA):
        print(f"!! schema 文件不存在: {SCHEMA}")
        sys.exit(1)

    conn = sqlite3.connect(DB_PATH)
    try:
        with open(SCHEMA, "r", encoding="utf-8") as f:
            conn.executescript(f.read())

        # 写入默认配置（不覆盖已存在的用户值）
        for k, v in DEFAULT_CONFIG.items():
            conn.execute(
                "INSERT OR IGNORE INTO config(key, value) VALUES (?, ?)", (k, v)
            )
        conn.commit()

        # 汇报
        tables = [r[0] for r in conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")]
        print(f"数据库: {DB_PATH}")
        print(f"已建表 ({len(tables)}): {', '.join(tables)}")
        cfg_n = conn.execute("SELECT COUNT(*) FROM config").fetchone()[0]
        print(f"config 项: {cfg_n}")
        print("列结构:")
        for t in tables:
            cols = [r[1] for r in conn.execute(f"PRAGMA table_info({t})")]
            print(f"  {t}: {', '.join(cols)}")
    finally:
        conn.close()


if __name__ == "__main__":
    main()
