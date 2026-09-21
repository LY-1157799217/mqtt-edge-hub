#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pi Hub 设备控制器（§7.2 方案 A：企微指令必须走 MQTT）。

—— 通用 MQTT 中控架构的"执行器"角色 ——
订阅 hub/command/smalltv，把 {action,value} 翻译成本机 Flask §2.4 接口调用，
Flask 再转发 ESP32 /set。执行结果 publish 到 hub/command/smalltv/ack，
由企微 Bot 订阅后回群。

链路：
  群@机器人 → Bot 解析 → publish hub/command/smalltv
    → [本进程] 订阅 → 调 /api/control/mode|brightness → ESP32 /set
    → publish hub/command/smalltv/ack → Bot 回群

运行：cd /home/pi/cooperate/pihub && .venv/bin/python app/devctl.py
说明：为什么不让 Bot 直调 API？—— 保持"控制指令即总线消息"的中控语义，
      未来接入更多设备（机械臂/传感器）时，各执行器独立订阅各自 topic 即可。
"""
import json
import os
import sqlite3
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import requests
import paho.mqtt.client as mqtt
from config import get as cfg
from command import describe

BASE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(BASE)
DB_PATH = os.environ.get("PIHUB_DB", os.path.join(PROJECT, "data", "pihub.db"))

CMD_TOPIC = "hub/command/smalltv"
ACK_TOPIC = "hub/command/smalltv/ack"

# in-flight 忙则丢弃：执行期间来的新指令直接丢弃并回提示，
# 防止瞬时堆叠指令反复打断 ESP32 整屏重绘（用户偏好：后堆叠指令丢弃）。
_EXEC_LOCK = threading.Lock()
_COOLDOWN_SEC = 1.5  # 指令执行完后的静默期，期内再来的指令视为堆叠 → 丢弃
_last_done = [0.0]  # 上次执行完成时间戳（单元素即可，用锁保护）

# ---- C8（契约 PI_HUB_SPEC_AGENT_LINK §5）：带业务 ID 的指令必须幂等 ----
# 为什么需要：`调亮/调暗` 是【相对】指令（±20），同一条重试会把亮度调两次。
#   模式切换 / 绝对亮度本身幂等，所以真正会出事的是相对指令。
# 只对【带了业务 ID】的指令生效；没带 ID 的手工指令行为完全不变。
_IDEMPOTENT_TTL = 60   # 秒：同 ID 在该窗口内再次到达 = 重试，不重复执行
_TID_MAX = 200         # 记录上限，防无界增长
_recent_tids = {}      # tid -> (完成时刻, ok, msg)
_TID_LOCK = threading.Lock()


def _tag(tid, text):
    """把业务 ID 标在回执正文最前面，让发起方能原样对回它那一步。"""
    return f"[{tid}] {text}" if tid else text


def _prune_tids(now):
    """清掉过期/超量的记录。调用方须持 _TID_LOCK。"""
    for k in [k for k, v in _recent_tids.items() if now - v[0] >= _IDEMPOTENT_TTL]:
        _recent_tids.pop(k, None)
    if len(_recent_tids) > _TID_MAX:
        for k, _v in sorted(_recent_tids.items(), key=lambda kv: kv[1][0])[:-_TID_MAX]:
            _recent_tids.pop(k, None)


def log(msg):
    print(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}", flush=True)


def api_base():
    # devctl 与 Flask 同机，默认走回环 127.0.0.1（Flask 已绑 0.0.0.0，回环必然可用），
    # 从而彻底免疫 WiFi 热点换段/换 IP —— 不再像旧版那样硬指向具体 WiFi IP。
    # 仅当显式需要打到别的主机时，才用 PIHUB_API_HOST env / config 表 pihub_host_remote 覆盖。
    host = os.environ.get("PIHUB_API_HOST") or cfg("pihub_host_remote") or "127.0.0.1"
    port = cfg("pihub_port") or "5000"
    return f"http://{host}:{port}"


def _get_last_brightness():
    try:
        with sqlite3.connect(DB_PATH) as c:
            row = c.execute("SELECT value FROM config WHERE key='last_brightness'").fetchone()
            return int(float(row[0])) if row else 60
    except Exception:
        return 60


def _set_last_brightness(v):
    try:
        with sqlite3.connect(DB_PATH) as c:
            c.execute("INSERT INTO config(key,value) VALUES('last_brightness',?) "
                      "ON CONFLICT(key) DO UPDATE SET value=excluded.value", (str(v),))
    except Exception:
        pass


def execute(action, value, extra=None):
    """执行指令，返回 (ok, msg)。extra 为可选附加参数 dict（如壁纸 idx）。"""
    extra = extra or {}
    idx = None  # 仅 set_wallpaper 静态时赋值；其余分支保持 None（describe 也会用到）
    base = api_base()
    try:
        if action == "set_mode":
            r = requests.post(f"{base}/api/control/mode", json={"mode": int(value)}, timeout=12)
            ok = r.status_code == 200
        elif action == "set_brightness":
            v = max(0, min(100, int(value)))
            r = requests.post(f"{base}/api/control/brightness", json={"value": v}, timeout=12)
            ok = r.status_code == 200
            if ok:
                _set_last_brightness(v)
            value = v
        elif action == "set_brightness_rel":
            v = max(0, min(100, _get_last_brightness() + int(value)))
            r = requests.post(f"{base}/api/control/brightness", json={"value": v}, timeout=12)
            ok = r.status_code == 200
            if ok:
                _set_last_brightness(v)
            action, value = "set_brightness", v
        elif action == "set_stockview":
            v = 1 if int(value) == 1 else 0
            r = requests.post(f"{base}/api/control/set", json={"params": {"stockview": v}}, timeout=12)
            ok = r.status_code == 200
            value = v
        elif action == "set_auto_brightness":
            r = requests.post(f"{base}/api/control/set",
                              json={"params": {"auto_brightness": 1}}, timeout=12)
            ok = r.status_code == 200
            value = 1
        elif action == "set_wallpaper":
            # 壁纸模式 0无/1静态/2动态；静态时可指定图片索引 idx(0..2)，未指定默认第 1 张。
            # 动态不需要 idx；关闭不需要 idx。
            mode = int(value)
            params = {"wp": mode}
            if mode == 1:
                idx = int(extra.get("idx", 0))
                idx = max(0, min(2, idx))
                params["idx"] = idx
            r = requests.post(f"{base}/api/control/set", json={"params": params}, timeout=12)
            ok = r.status_code == 200
            value = mode
        elif action == "help":
            return True, describe("help", None)
        else:
            return False, f"未知指令: {action}"
        if ok:
            return True, describe(action, value, idx=idx)
        return False, f"执行失败（ESP32 返回 {r.status_code}）"
    except Exception as e:
        log(f"[exec] {action} 异常: {e}")
        return False, f"执行异常：{e}"


def on_connect(client, userdata, flags, rc, props=None):
    client.subscribe(CMD_TOPIC, qos=1)
    log(f"[mqtt] connected, subscribed {CMD_TOPIC}")


def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except Exception:
        log(f"[mqtt] 非法 payload: {msg.payload!r}")
        return
    action = payload.get("action")
    value = payload.get("value")
    extra = payload.get("extra") or {}
    req_id = payload.get("req_id")
    tid = extra.get("tid")   # §5 C5：发起方带过来的业务 ID；没带就是 None

    # ---- C8：同 ID 重复到达 ⇒ 按上次结果回执，【不重复执行】----
    # 必须在冷却判断【之前】做：重试撞上冷却会被报成"设备忙"，
    # 而它其实已经执行过了 —— 那会让发起方一直重试，永远对不上。
    if tid:
        now = time.time()
        with _TID_LOCK:
            _prune_tids(now)
            hit = _recent_tids.get(tid)
        if hit and now - hit[0] < _IDEMPOTENT_TTL:
            log(f"[dup] 业务ID {tid} 重复到达（上次 ok={hit[1]}），不重复执行")
            ack = {"ok": hit[1], "msg": _tag(tid, f"{hit[2]}（重复下发，未再次执行）"),
                   "action": action, "value": value, "tid": tid,
                   "req_id": req_id, "ts": int(now)}
            client.publish(ACK_TOPIC, json.dumps(ack, ensure_ascii=False), qos=1, retain=False)
            return

    # 忙则丢弃：① 正有一条在执行（ESP32 重绘中）；② 距上次完成不足冷却期。
    # 两者都直接拒绝本条，避免堆叠指令把 ESP32 冲坏。用非阻塞锁，不排队。
    now = time.time()
    if now - _last_done[0] < _COOLDOWN_SEC:
        log(f"[busy] 冷却期内丢弃 {action}={value}" + (f" [{tid}]" if tid else ""))
        ack = {"ok": 0, "msg": _tag(tid, "设备忙（上一条指令刚执行完），已忽略"),
               "action": action, "value": value, "tid": tid,
               "req_id": req_id, "ts": int(time.time())}
        client.publish(ACK_TOPIC, json.dumps(ack, ensure_ascii=False), qos=1, retain=False)
        return
    if not _EXEC_LOCK.acquire(blocking=False):
        log(f"[busy] 丢弃堆叠指令 {action}={value}" + (f" [{tid}]" if tid else ""))
        ack = {"ok": 0, "msg": _tag(tid, "设备忙（上一条指令执行中），已忽略"),
               "action": action, "value": value, "tid": tid,
               "req_id": req_id, "ts": int(time.time())}
        client.publish(ACK_TOPIC, json.dumps(ack, ensure_ascii=False), qos=1, retain=False)
        return

    try:
        log(f"[cmd] {action}={value}" + (f" [{tid}]" if tid else ""))
        ok, text = execute(action, value, extra)
    finally:
        _last_done[0] = time.time()
        _EXEC_LOCK.release()

    # 只有【真执行过】才登记业务 ID —— 被丢弃的那次绝不能登记，
    # 否则发起方重试时会被判成"重复"，那条指令就永远执行不了。
    if tid:
        with _TID_LOCK:
            _recent_tids[tid] = (time.time(), ok, text)

    ack = {"ok": 1 if ok else 0, "msg": _tag(tid, text), "action": action, "value": value,
           "tid": tid, "req_id": req_id, "ts": int(time.time())}
    client.publish(ACK_TOPIC, json.dumps(ack, ensure_ascii=False), qos=1, retain=False)
    log(f"[ack] {ack}")


def main():
    host = cfg("mqtt_host") or "127.0.0.1"
    port = int(float(cfg("mqtt_port") or 1883))
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="pihub-devctl")
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(host, port, keepalive=30)
    log(f"[start] devctl 启动, api={api_base()}")
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        log("[stop] devctl 退出")


if __name__ == "__main__":
    main()
