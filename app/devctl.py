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
    extra = payload.get("extra")
    req_id = payload.get("req_id")

    # 忙则丢弃：① 正有一条在执行（ESP32 重绘中）；② 距上次完成不足冷却期。
    # 两者都直接拒绝本条，避免堆叠指令把 ESP32 冲坏。用非阻塞锁，不排队。
    now = time.time()
    if now - _last_done[0] < _COOLDOWN_SEC:
        log(f"[busy] 冷却期内丢弃 {action}={value}")
        ack = {"ok": 0, "msg": "设备忙（上一条指令刚执行完），已忽略",
               "action": action, "value": value, "req_id": req_id,
               "ts": int(time.time())}
        client.publish(ACK_TOPIC, json.dumps(ack, ensure_ascii=False), qos=1, retain=False)
        return
    if not _EXEC_LOCK.acquire(blocking=False):
        log(f"[busy] 丢弃堆叠指令 {action}={value}")
        ack = {"ok": 0, "msg": "设备忙（上一条指令执行中），已忽略",
               "action": action, "value": value, "req_id": req_id,
               "ts": int(time.time())}
        client.publish(ACK_TOPIC, json.dumps(ack, ensure_ascii=False), qos=1, retain=False)
        return

    try:
        log(f"[cmd] {action}={value}")
        ok, text = execute(action, value, extra)
    finally:
        _last_done[0] = time.time()
        _EXEC_LOCK.release()

    ack = {"ok": 1 if ok else 0, "msg": text, "action": action, "value": value,
           "req_id": req_id, "ts": int(time.time())}
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
