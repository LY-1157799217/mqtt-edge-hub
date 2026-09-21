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

# 重连退避：起点 3s，每次失败翻倍，封顶 30s。
# 封顶是有意的（服务端打不通时别疯狂重连）；但**连上之后必须回到起点** ——
# 原先只增不减，导致一次抖动之后每次断线都按封顶等，详见 _on_open 的说明。
BACKOFF_START = 3
BACKOFF_MAX = 30

# 「等待回执」记录的超时与上限（详见 _sweep_pending 的说明）。
# 45s 的依据：正常回执路径 = devctl 冷却(≤1.5s) + 转发 ESP32(HTTP 超时 12s) + 回程，
# 实测在 15s 内完成；等满 45s 基本可以断定这条回执不会来了。
_PENDING_TTL = 45          # 秒：超过仍未收到 ack ⇒ 判「结果未知」
_PENDING_MAX = 200         # 硬上限兜底，防无界增长
_PENDING_SWEEP_SEC = 15    # 扫描间隔

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
        self.pending = {}  # req_id -> (response_url, 发起时刻, tid)；等 devctl 回执后回群，超时见 _sweep_pending
        self._pending_lock = threading.Lock()
        self._backoff = BACKOFF_START  # 重连退避当前值；连上即重置，见 _on_open

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
                # 记「发起时刻」供超时判定，记「业务 ID」让超时回报也能对上发起方的步骤
                self.pending[req_id] = (response_url, time.time(),
                                        (extra or {}).get("tid"))
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
            ent = self.pending.pop(req_id, None)
        rurl = ent[0] if ent else None
        if rurl:
            self.reply(rurl, text)
        else:
            # 无待回复 url 时，主动推到默认会话
            self.send_markdown(text)
        _log(f"[ack] {text}")

    # ---------------- 等待回执的超时清理 ----------------
    def _sweep_pending(self):
        """清掉超时未回执的记录，并**明确回一句「结果未知」**。

        为什么需要：send_command 把 response_url 记进 self.pending，等 devctl 的 ack 回来再取用。
        但 ack 可能**永远不来**（devctl 没起 / MQTT 断 / 指令根本没落到执行侧）。旧版只有
        「收到 ack 才删」，于是：① 执行器离线期间每条指令都留一条记录，反复重试则内存持续增长；
        ② 发起方**永远等不到任何回音**，只能一直干等或盲目重试。

        措辞用**「结果未知」而不是「失败」** —— 超时既可能是「没执行」，也可能是
        「执行了但回执丢了」，这两者从本侧分不出来，说成失败会误导发起方去重试。
        """
        now = time.time()
        with self._pending_lock:
            stale = [(k, v) for k, v in self.pending.items() if now - v[1] >= _PENDING_TTL]
            for k, _v in stale:
                self.pending.pop(k, None)
            dropped = 0
            if len(self.pending) > _PENDING_MAX:
                # 兜底：即便 TTL 未到也不能让它无界增长。正常不该走到这条路径；
                # 被丢掉的记录若随后有 ack 回来，会走 on_command_ack 的「主动推默认会话」分支。
                for k, _v in sorted(self.pending.items(), key=lambda kv: kv[1][1])[:-_PENDING_MAX]:
                    self.pending.pop(k, None)
                    dropped += 1
        if dropped:
            _log(f"[pending] 超过上限 {_PENDING_MAX}，丢弃最旧的 {dropped} 条")

        for _k, (rurl, ts, tid) in stale:
            waited = int(now - ts)
            tag = f"[{tid}] " if tid else ""
            msg = (f"{tag}未收到执行回执（已等待 {waited} 秒），**本次结果未知** —— "
                   f"既可能已执行、也可能没有。请先确认设备状态，不要盲目重发。")
            if self.reply(rurl, msg):
                _log(f"[pending] 已回报「结果未知」（等待 {waited}s, tid={tid}）")
            else:
                # response_url 通常有有效期，过期后只能改推默认会话，否则这条信号就丢了
                _log(f"[pending] response_url 回报失败，改推默认会话（tid={tid}）")
                self.send_markdown("⚠️ " + msg)

    def _pending_sweeper(self):
        """常驻扫描线程。与心跳同理：整个进程只起这一个，且必须在重连循环【外】起。

        单独开线程而不挂在心跳循环上：回报要发 HTTP（reply 超时 6s，失败还会再推一次），
        而心跳的职责是每 30s 发 ping 保活 —— 被拖慢有掉线的风险。
        """
        while not self.stop:
            time.sleep(_PENDING_SWEEP_SEC)
            if self.stop:
                return
            try:
                self._sweep_pending()
            except Exception as e:
                _log(f"[pending] 清理异常: {e}")

    def _on_open(self, ws):
        _log(f"[ws] connected, subscribe bot_id={self.bot_id}")
        # 连上了 ⇒ 网络与服务端都是通的，退避归零。
        #
        # 修的是这个缺陷：退避原先在 run_forever 里【只增不减】，从不重置。
        # 于是只要经历过几次抖动，此后【每次】断线都要按封顶等 30s ——
        # 哪怕这次断线与上次毫无关系、网络也早就好了。
        # 而断线窗口内企微**不下发也不补发** @ 本机器人的消息 ⇒ 窗口越长丢得越多。
        #
        # 为什么放这里、而不是"run_forever 返回之后"：run_forever 在
        # 【连上后被关闭】与【压根没连上】两种情况下都会正常返回，看返回值分不清。
        # _on_open 是"确实连上过"的唯一可靠信号。
        #
        # 权衡：若服务端出现"接受连接后立刻断开"的反复，这里会以 3s 的节奏重试，
        # 而不是逐步退避到 30s。3s ≈ 每分钟 20 次，不构成重试风暴；
        # 而"要求连接稳定 N 秒才重置"要多引入一个可调参数，只换来这个未观察到的场景，故不采用。
        if self._backoff != BACKOFF_START:
            _log(f"[ws] 连接成功，退避重置 {self._backoff}s -> {BACKOFF_START}s")
        self._backoff = BACKOFF_START
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
                # help 不走 MQTT，所以业务 ID 要在这里自己带上（与其他回执保持一致）
                tid = (extra or {}).get("tid")
                self.reply(rurl, f"[{tid}]\n{HELP_TEXT}" if tid else HELP_TEXT)
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
        # 心跳线程【整个进程只起这一个】。
        # 旧版把它放在重连循环里，每次重连都 new 一个；而旧线程的退出条件
        # `not self.stop and self.ws is not None` 依赖 self.ws 变 None 才结束，
        # 可 self.ws 只会被【重新赋值】、从不置 None ⇒ 旧线程永不退出，
        # 且它读的是 self.ws 属性 ⇒ 重连后 N 个线程挤在新连接上发 N 倍 ping，无上限累积。
        threading.Thread(target=self._heartbeat_loop, daemon=True,
                         name="wecom-heartbeat").start()
        # 等待回执的超时扫描，同样【只起这一个】（理由见 _pending_sweeper）。
        threading.Thread(target=self._pending_sweeper, daemon=True,
                         name="wecom-pending-sweeper").start()
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
                # blocked until close
                self.ws.run_forever(ping_interval=0)
            except Exception as e:
                _log(f"[ws] run_forever 异常: {e}")
            if self.stop:
                break
            _log(f"[ws] {self._backoff}s 后重连")
            time.sleep(self._backoff)
            self._backoff = min(self._backoff * 2, BACKOFF_MAX)

    def _heartbeat_loop(self):
        # 常驻单线程：只在「已订阅」期间发 ping。断开时 _on_close 会 clear subscribed，
        # 它自然静默等待重连，无需也不该再起第二个线程。
        while not self.stop:
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
