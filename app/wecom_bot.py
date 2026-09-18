#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pi Hub 企微 Bot（依据 PI_HUB_SPEC.md §7，长连接 API 模式）。

架构（§0 角色表：企微 Bot = 订阅 MQTT → 推送到手机；接收指令 → 控制设备）：
  ┌ 企业微信 智能机器人（长连接 WebSocket）  ┐
  │  wss://openws.work.weixin.qq.com        │
  │  aibot_subscribe(bot_id, secret)        │
  │  ← aibot_msg_callback / event_callback  │  收到用户消息 → 记录 chatid
  │  → aibot_send_msg (主动推送)             │  MQTT 告警 → 推到群聊
  └─────────────────────────────────────────┘
  ↑ MQTT subscribe hub/alert/#           ↓ MQTT 可选 hub/command/smalltv

关键约束（官方文档 §7）：
  - 智能机器人「主动推送」要求：用户先在会话里给机器人发过消息，才可向该会话推送。
    → 首次收到 aibot_msg_callback 时把 chatid 落 SQLite(wecom_chat)，后续告警复用。
  - 心跳：每 30s 发 ping 保活；断线需自动重连。
  - 频率限制：单会话 30 条/分钟，1000 条/小时（告警侧已有 1h 冷却，另加分钟级防刷）。

运行：cd /home/pi/cooperate/pihub && .venv/bin/python app/wecom_bot.py
自测：--send-test "内容" 直接向已记录 chatid 推一条测试（不起 MQTT）。
"""
import argparse
import json
import os
import sqlite3
import sys
import threading
import time
import uuid
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import websocket  # websocket-client
import paho.mqtt.client as mqtt
from config import get as cfg
from command import parse_command, describe, HELP_TEXT

BASE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(BASE)
DB_PATH = os.environ.get("PIHUB_DB", os.path.join(PROJECT, "data", "pihub.db"))


def _load_env():
    """读 .env（不进 git 的密钥）。"""
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

WS_URL = "wss://openws.work.weixin.qq.com"
HEARTBEAT_SEC = 30

# 告警类型 → 中文标题
ALERT_TITLES = {
    "limit_up": "涨停警报",
    "limit_down": "跌停警报",
    "surge": "异动提醒",
    "weather_warning": "天气预警",
}

# §7.2 指令总线（方案 A：企微指令必走 MQTT）
CMD_TOPIC = "hub/command/smalltv"
CMD_ACK_TOPIC = "hub/command/smalltv/ack"


def _log(msg):
    line = f"{datetime.now().strftime('%Y-%m-%d %H:%M:%S')} {msg}"
    print(line, flush=True)


def _conn():
    c = sqlite3.connect(DB_PATH, timeout=10)
    c.row_factory = sqlite3.Row
    return c


# ---------------- chatid 持久化 ----------------
def save_chat(chatid, chat_type, label=None):
    """记录可主动推送的会话（首次收到用户消息时）。"""
    now = int(time.time())
    with _conn() as c:
        c.execute("""
            INSERT INTO wecom_chat(chatid, chat_type, label, active, last_seen)
            VALUES (?,?,?,1,?)
            ON CONFLICT(chatid) DO UPDATE SET
              chat_type=excluded.chat_type, last_seen=excluded.last_seen
        """, (chatid, chat_type, label, now))
        # 同时写 config，便于其它组件读取默认目标
        c.execute("""INSERT INTO config(key,value,updated_at) VALUES('wecom_chat_id',?,?)
                     ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at""",
                  (chatid, now))
        c.execute("""INSERT INTO config(key,value,updated_at) VALUES('wecom_chat_type',?,?)
                     ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at""",
                  (str(chat_type), now))


def get_target_chat():
    """返回 (chatid, chat_type)；优先 config.wecom_chat_id，其次最新 active 会话。"""
    cid = cfg("wecom_chat_id")
    ctype = int(float(cfg("wecom_chat_type") or 2))
    if cid:
        return cid, ctype
    with _conn() as c:
        row = c.execute("SELECT chatid,chat_type FROM wecom_chat WHERE active=1 "
                        "ORDER BY last_seen DESC LIMIT 1").fetchone()
    if row:
        return row["chatid"], row["chat_type"]
    return None, ctype


# ---------------- 消息格式化 ----------------
def format_alert(topic, payload):
    """MQTT 主题+payload → 企微 markdown 文本（§7.2 示例风格）。"""
    atype = topic.rsplit("/", 1)[-1] if "/" in topic else topic
    title = ALERT_TITLES.get(atype, "提醒")
    sym = payload.get("symbol", "")
    label = payload.get("label", sym)
    # 代码：sh600519 -> 600519
    code = sym[2:] if len(sym) > 2 and sym[:2] in ("sh", "sz") else sym
    if atype == "weather_warning":
        return (f"**⚠️ {title}**\n"
                f"> 城市：{payload.get('city','')}\n"
                f"> 等级：{payload.get('level','')}\n"
                f"> {payload.get('text','')}")
    price = payload.get("price")
    pct = payload.get("pct")
    sign = "+" if (pct or 0) >= 0 else ""
    extra = ""
    if atype == "surge" and payload.get("threshold") is not None:
        extra = f"\n> 触发阈值：±{payload['threshold']}%"
    return (f"**{title}**\n"
            f"> {label}({code})\n"
            f"> 现价 ¥{price}　涨幅 {sign}{pct}%{extra}\n"
            f"> {datetime.now().strftime('%H:%M:%S')}")


# ---------------- WebSocket 长连接 ----------------
class WeComBot:
    def __init__(self, on_message=None):
        # 凭证统一从 .env / 环境变量读取（§1：密钥不进 git，不入 config 表）
        env = _load_env()
        self.bot_id = os.environ.get("WECOM_BOT_ID") or env.get("WECOM_BOT_ID", "")
        self.secret = os.environ.get("WECOM_BOT_SECRET") or env.get("WECOM_BOT_SECRET", "")
        self.ws = None
        self.subscribed = threading.Event()
        self.stop = False
        self.on_message = on_message  # 收到消息时的可选回调(body)
        self._last_hb = 0
        self._send_lock = threading.Lock()
        self.mqtt = None  # 由 start_mqtt 注入（发指令用）
        self.pending = {}  # req_id -> response_url（等待 devctl 回执后回群）
        self._pending_lock = threading.Lock()

    def _now(self):
        return int(time.time())

    def _new_req_id(self):
        return uuid.uuid4().hex

    def _send(self, obj):
        with self._send_lock:
            if self.ws is None:
                return False
            try:
                self.ws.send(json.dumps(obj, ensure_ascii=False))
                return True
            except Exception as e:
                _log(f"[ws] send 失败: {e}")
                return False

    def subscribe(self):
        self._send({
            "cmd": "aibot_subscribe",
            "headers": {"req_id": self._new_req_id()},
            "body": {"bot_id": self.bot_id, "secret": self.secret},
        })

    def send_markdown(self, content, chatid=None, chat_type=None):
        """主动推送 markdown 到指定会话（aibot_send_msg）。"""
        if chatid is None:
            chatid, chat_type = get_target_chat()
        if not chatid:
            _log("[send] 无可用 chatid（需用户先给机器人发消息），跳过推送")
            return False
        if chat_type is None:
            chat_type = 2
        ok = self._send({
            "cmd": "aibot_send_msg",
            "headers": {"req_id": self._new_req_id()},
            "body": {"chatid": chatid, "chat_type": int(chat_type),
                     "msgtype": "markdown", "markdown": {"content": content}},
        })
        _log(f"[send] markdown -> {chatid}(type={chat_type}) ok={ok}")
        return ok

    def reply(self, response_url, content):
        """用 response_url 被动回复（§101138）。无需 chatid，直接 HTTP POST。"""
        if not response_url:
            return False
        try:
            import requests as _rq
            r = _rq.post(response_url, json={
                "msgtype": "markdown", "markdown": {"content": content}},
                timeout=6)
            _log(f"[reply] -> {r.status_code} {r.text[:120]}")
            return r.status_code == 200
        except Exception as e:
            _log(f"[reply] 失败: {e}")
            return False

    def send_command(self, action, value, response_url=None, extra=None):
        """发指令到 MQTT 总线 hub/command/smalltv（§7.2 方案 A：不自调 API）。
        等待 devctl 执行后回 ack，再用 response_url 回群。
        """
        req_id = self._new_req_id()
        if response_url:
            with self._pending_lock:
                self.pending[req_id] = response_url
        payload = {"action": action, "value": value, "req_id": req_id}
        if extra:
            payload["extra"] = extra
        if self.mqtt is None:
            _log("[cmd] MQTT 未连接，无法发指令")
            if response_url:
                self.reply(response_url, "指令总线未连接，请稍后重试")
            return False
        self.mqtt.publish(CMD_TOPIC, json.dumps(payload, ensure_ascii=False), qos=1)
        _log(f"[cmd] publish {CMD_TOPIC} {payload}")
        return True

    def on_command_ack(self, payload):
        """收到 hub/command/smalltv/ack，找回 response_url 回群。"""
        req_id = payload.get("req_id")
        ok = payload.get("ok")
        msg = payload.get("msg", "")
        text = ("✅ " if ok else "⚠️ ") + msg
        with self._pending_lock:
            rurl = self.pending.pop(req_id, None)
        if rurl:
            self.reply(rurl, text)
        else:
            # 无待回复 url 时，主动推到默认会话
            self.send_markdown(text)
        _log(f"[ack] {text}")

    def _on_open(self, ws):
        _log(f"[ws] connected, subscribe bot_id={self.bot_id}")
        self.subscribe()

    def _on_message(self, ws, raw):
        try:
            msg = json.loads(raw)
        except Exception:
            _log(f"[ws] 非JSON: {raw[:200]}")
            return
        cmd = msg.get("cmd", "")
        if cmd == "aibot_subscribe" or (not cmd and "errcode" in msg and not self.subscribed.is_set()):
            # 订阅响应：无 cmd，靠 req_id 回执；errcode=0 即成功
            rc = msg.get("errcode", msg.get("result", {}).get("errcode"))
            if rc == 0:
                self.subscribed.set()
                _log("[ws] subscribe ok")
            else:
                _log(f"[ws] subscribe 失败: {msg}")
            return
        if cmd == "aibot_msg_callback":
            body = msg.get("body", {})
            ctype = 1 if body.get("chattype") == "single" else 2
            cid = body.get("chatid") or (body.get("from", {}) or {}).get("userid")
            if cid:
                save_chat(cid, ctype, label="告警群" if ctype == 2 else None)
                _log(f"[ws] 记录会话 chatid={cid} type={ctype}")
            text = (body.get("text") or {}).get("content", "")
            _log(f"[ws] 收到消息: {text[:80]}")
            # §7.2：解析指令 → 发 MQTT 总线 → devctl 执行 → ack 回群
            rurl = body.get("response_url")
            action, value, extra = parse_command(text)
            if action and action != "help":
                self.send_command(action, value, response_url=rurl, extra=extra)
            elif action == "help":
                self.reply(rurl, HELP_TEXT)
            else:
                # 未识别为指令：回一句引导（同时便于排障确认消息已到达）
                self.reply(rurl, "收到～\n" + HELP_TEXT)
                if self.on_message:
                    try:
                        self.on_message(body)
                    except Exception as e:
                        _log(f"[ws] on_message 异常: {e}")
            return
        if cmd == "aibot_event_callback":
            body = msg.get("body", {})
            ev = (body.get("event") or {}).get("eventtype", "")
            if ev == "disconnected_event":
                _log("[ws] 服务端断开旧连接事件")
                self.subscribed.clear()
                try:
                    ws.close()
                except Exception:
                    pass
            return
        if cmd in ("aibot_respond_msg", "aibot_send_msg", "aibot_respond_welcome_msg",
                   "aibot_respond_update_msg", "ping"):
            rc = msg.get("errcode")
            if rc not in (0, None):
                _log(f"[ws] {cmd} resp: {msg}")
            return
        # 无 cmd 的响应（心跳/推送回执）静默处理，仅在错误时记录
        if not cmd:
            rc = msg.get("errcode")
            if rc not in (0, None):
                _log(f"[ws] 响应错误: {msg}")
            return
        _log(f"[ws] 未处理 cmd={cmd!r} raw={raw[:300]}")

    def _on_error(self, ws, err):
        _log(f"[ws] error: {err}")

    def _on_close(self, ws, code, msg):
        self.subscribed.clear()
        _log(f"[ws] closed code={code} msg={msg}")

    def run_forever(self):
        backoff = 3
        while not self.stop:
            try:
                _log(f"[ws] 连接 {WS_URL} ...")
                self.ws = websocket.WebSocketApp(
                    WS_URL,
                    on_open=self._on_open,
                    on_message=self._on_message,
                    on_error=self._on_error,
                    on_close=self._on_close,
                )
                # 心跳线程
                t = threading.Thread(target=self._heartbeat_loop, daemon=True)
                t.start()
                # blocked until close
                self.ws.run_forever(ping_interval=0)
            except Exception as e:
                _log(f"[ws] run_forever 异常: {e}")
            if self.stop:
                break
            _log(f"[ws] {backoff}s 后重连")
            time.sleep(backoff)
            backoff = min(backoff * 2, 30)

    def _heartbeat_loop(self):
        while not self.stop and self.ws is not None:
            if self.subscribed.is_set():
                self._send({"cmd": "ping", "headers": {"req_id": self._new_req_id()}})
                self._last_hb = self._now()
            time.sleep(HEARTBEAT_SEC)

    def stop_bot(self):
        self.stop = True
        try:
            if self.ws:
                self.ws.close()
        except Exception:
            pass


# ---------------- MQTT 订阅 ----------------
def start_mqtt(bot, log_path):
    host = cfg("mqtt_host") or "127.0.0.1"
    port = int(float(cfg("mqtt_port") or 1883))

    def on_connect(client, userdata, flags, rc, props=None):
        client.subscribe("hub/alert/#", qos=1)
        client.subscribe(CMD_ACK_TOPIC, qos=1)
        _log(f"[mqtt] connected {host}:{port}, subscribed hub/alert/# + {CMD_ACK_TOPIC}")

    def on_message(client, userdata, msg):
        topic = msg.topic
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except Exception:
            payload = {"raw": msg.payload.decode("utf-8", "ignore")}
        if topic == CMD_ACK_TOPIC:
            bot.on_command_ack(payload)
            return
        content = format_alert(topic, payload)
        _log(f"[mqtt] {topic} -> 推送")
        bot.send_markdown(content)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="pihub-wecom-bot")
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(host, port, keepalive=30)
    client.loop_start()
    bot.mqtt = client
    return client


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--send-test", metavar="TEXT", help="直接推送一条测试消息到已记录会话")
    ap.add_argument("--chatid", help="指定测试推送的 chatid")
    args = ap.parse_args()

    if args.send_test:
        bot = WeComBot()
        # 测试模式：临时建立连接→订阅→推送→退出
        bot.ws = websocket.WebSocketApp(
            WS_URL, on_open=bot._on_open, on_message=bot._on_message,
            on_error=bot._on_error, on_close=bot._on_close)
        t = threading.Thread(target=bot.ws.run_forever, kwargs={"ping_interval": 0}, daemon=True)
        t.start()
        if not bot.subscribed.wait(timeout=10):
            _log("[test] 订阅超时")
        time.sleep(1)
        bot.send_markdown(args.send_test, chatid=args.chatid)
        time.sleep(2)
        bot.stop_bot()
        return

    bot = WeComBot()
    mqtt_client = start_mqtt(bot, None)
    try:
        bot.run_forever()
    except KeyboardInterrupt:
        _log("[main] 退出")
    finally:
        mqtt_client.loop_stop()


if __name__ == "__main__":
    main()
