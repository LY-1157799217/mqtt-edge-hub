-- Pi Hub SQLite schema
-- 依据 PI_HUB_SPEC.md §6 步骤 2
-- 表：symbols(自选股) / quote_cache / kline_history / weather_cache / alert_cooldown / config

PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;

-- 自选股列表
CREATE TABLE IF NOT EXISTS symbols (
    symbol      TEXT PRIMARY KEY,          -- 如 sh600519 / sz000725 / sh000001
    label       TEXT NOT NULL DEFAULT '',  -- 显示名，如 "贵州茅台"
    kind        TEXT NOT NULL DEFAULT 'stock', -- stock | index
    sort        INTEGER NOT NULL DEFAULT 0,
    enabled     INTEGER NOT NULL DEFAULT 1,
    created_at  INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

-- 实时行情缓存（每 symbol 一行最新）
CREATE TABLE IF NOT EXISTS quote_cache (
    symbol      TEXT PRIMARY KEY,
    price       REAL,
    pct         REAL,
    prev        REAL,
    high        REAL,
    low         REAL,
    spark       TEXT,                      -- JSON 数组，固定 48 个 float
    updated_at  INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

-- 日 K 历史
CREATE TABLE IF NOT EXISTS kline_history (
    symbol      TEXT NOT NULL,
    date        TEXT NOT NULL,             -- YYYY-MM-DD
    open        REAL,
    close       REAL,
    high        REAL,
    low         REAL,
    PRIMARY KEY (symbol, date)
);

-- 天气缓存
CREATE TABLE IF NOT EXISTS weather_cache (
    city_code   TEXT PRIMARY KEY,          -- 9 位中国天气网编码
    temp        REAL,
    humi        INTEGER,
    press       INTEGER,
    aqi         INTEGER,
    code        INTEGER,                   -- weathercode 去掉 d/n 后的数字
    dn          TEXT,                       -- 'd'/'n' 昼夜标识（不进契约，仅缓存，§2.3）
    raw         TEXT,                       -- 原始 JSON/HTML 备份，排障用
    updated_at  INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

-- 告警去重冷却（持久化，重启不重推，§4）
CREATE TABLE IF NOT EXISTS alert_cooldown (
    symbol      TEXT NOT NULL,
    alert_type  TEXT NOT NULL,             -- limit_up | limit_down | surge | weather_warning
    last_fired  INTEGER NOT NULL,          -- unix 时间戳
    payload     TEXT,                      -- 上次推送内容，排障用
    PRIMARY KEY (symbol, alert_type)
);

-- 配置表（阈值等可调参数）
CREATE TABLE IF NOT EXISTS config (
    key         TEXT PRIMARY KEY,
    value       TEXT NOT NULL,
    updated_at  INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

-- 企微会话（长连接模式主动推送目标）
-- 依据 PI_HUB_SPEC.md §7：机器人只能向"用户曾发过消息"的会话主动推送，
-- 首次收到 aibot_msg_callback 时记录 chatid/chat_type 至此，供告警推送复用。
CREATE TABLE IF NOT EXISTS wecom_chat (
    chatid      TEXT PRIMARY KEY,      -- 群聊 chatid / 单聊 userid
    chat_type   INTEGER NOT NULL DEFAULT 2, -- 1 单聊 / 2 群聊
    label       TEXT,                  -- 备注（如"告警群"）
    active      INTEGER NOT NULL DEFAULT 1,
    last_seen   INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);
