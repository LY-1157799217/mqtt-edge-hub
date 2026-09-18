#!/usr/bin/env bash
# §9 整机断电重启验收 + 换段复验（Pi Hub）
# 用法：
#   1) 断电重启前，先记录基线：  ./acceptance_s9.sh baseline
#   2) 断电 -> 重启树莓派 -> 等约 60~90s（等 lightdm 自动登录 + 用户服务拉起 + 音频就绪）
#   3) 重启后自动核对：           ./acceptance_s9.sh check
# 说明：本脚本只"读"状态，不做任何写操作；安全可重复跑。
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
BASE_FILE="$DIR/logs/s9_baseline.env"
LOG="$DIR/logs/s9_acceptance.log"

log() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }

# ---------- 服务清单（5 个） ----------
SERVICES=(mosquitto pihub-api pihub-monitor pihub-devctl pihub-wecom-bot)

check_services() {
  local fail=0
  for s in "${SERVICES[@]}"; do
    st=$(systemctl is-active "$s" 2>/dev/null || true)
    en=$(systemctl is-enabled "$s" 2>/dev/null || true)
    if [ "$st" = "active" ] && [ "$en" = "enabled" ]; then
      log "  [OK]   $s  active/enabled"
    else
      log "  [FAIL] $s  active=$st enabled=$en"
      fail=1
    fi
  done
  return $fail
}

# ---------- Flask 绑定 & 端点 ----------
check_flask() {
  local fail=0
  local bind
  bind=$(ss -ltn 2>/dev/null | awk '$4 ~ /:5000$/ {print $4}' | head -1)
  if [ "$bind" = "0.0.0.0:5000" ]; then
    log "  [OK]   Flask LISTEN 0.0.0.0:5000 (免疫换 IP)"
  else
    log "  [FAIL] Flask 绑定异常: '$bind' (期望 0.0.0.0:5000)"
    fail=1
  fi

  # 回环可达（devctl 走这个）
  code=$(curl -s -m5 -o /dev/null -w '%{http_code}' http://127.0.0.1:5000/api/stock/sh000001 || echo 000)
  [ "$code" = "200" ] && log "  [OK]   curl 127.0.0.1:5000/api/stock/sh000001 -> 200" || { log "  [FAIL] 回环端点 -> $code"; fail=1; }

  # WiFi IP 可达
  ip=$(ip -4 addr show wlan0 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1 | head -1)
  if [ -n "$ip" ]; then
    code=$(curl -s -m5 -o /dev/null -w '%{http_code}' "http://$ip:5000/api/stock/sh000001" || echo 000)
    [ "$code" = "200" ] && log "  [OK]   curl $ip:5000/... -> 200" || { log "  [FAIL] WiFi IP 端点 -> $code"; fail=1; }
    log "  [INFO] 当前 WiFi IP = $ip"
  else
    log "  [FAIL] wlan0 无 IPv4 地址"; fail=1
  fi
  return $fail
}

# ---------- Mosquitto 不对外 ----------
check_mqtt() {
  local fail=0
  local bind
  bind=$(ss -ltn 2>/dev/null | awk '$4 ~ /:1883$/ {print $4}')
  if echo "$bind" | grep -q "127.0.0.1:1883"; then
    log "  [OK]   Mosquitto 绑 127.0.0.1:1883 ($bind)"
  else
    log "  [FAIL] Mosquitto 绑定异常: '$bind'"; fail=1
  fi
  # 回路自测
  if timeout 5 mosquitto_pub -h 127.0.0.1 -t 's9/selftest' -m 'ping' 2>/dev/null; then
    log "  [OK]   mosquitto_pub 回路可用"
  else
    log "  [FAIL] mosquitto_pub 失败"; fail=1
  fi
  return $fail
}

# ---------- ESP32 是否在走 Pi ----------
check_esp32() {
  local fail=0
  local smalltv
  smalltv=$(python3 - "$DIR" <<'PY'
import sqlite3, sys, os
db=os.path.join(sys.argv[1],"data","pihub.db")
try:
    c=sqlite3.connect(db)
    r=c.execute("select value from config where key='smalltv_ip'").fetchone()
    print(r[0] if r else "")
except Exception:
    print("")
PY
)
  log "  [INFO] config.smalltv_ip = ${smalltv:-<空>}"

  # ESP32 可达性
  if [ -n "$smalltv" ] && ping -c1 -W2 "$smalltv" >/dev/null 2>&1; then
    log "  [OK]   ESP32 $smalltv 可 ping 通"
    # 近 120s 内 ESP32 是否访问过 Pi
    n=$(tail -300 "$DIR/logs/api.log" 2>/dev/null | grep -c "$smalltv")
    if [ "$n" -gt 0 ]; then
      log "  [OK]   ESP32 近期有访问 Pi ($n 条)"
    else
      log "  [WARN] 近期未见 ESP32 访问 Pi（可能在时钟模式/未取数）"
    fi
  else
    log "  [WARN] ESP32 ${smalltv:-?} 不可达（离线/未上电）"
  fi

  # 上报端点是否出现过（非回环）
  if [ -n "$smalltv" ] && grep -q "$smalltv.*POST /api/esp32_ip" "$DIR/logs/api.log" 2>/dev/null; then
    log "  [OK]   见过 ESP32 上报 /api/esp32_ip"
  else
    log "  [WARN] 未见 ESP32 上报 /api/esp32_ip（重启后 ESP32 应会补报）"
  fi
  return $fail
}

# ---------- 换段复验提示 ----------
check_swap_hint() {
  local ip
  ip=$(ip -4 addr show wlan0 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1 | head -1)
  if [ -f "$BASE_FILE" ]; then
    old=$(grep '^WLAN_IP=' "$BASE_FILE" | cut -d= -f2)
    if [ -n "$old" ] && [ "$old" != "$ip" ]; then
      log "  [INFO] 检测到换段: $old -> $ip"
      log "  [INFO] Pi 侧应 0 改动自愈；ESP32 侧需改 piHubHost(网页) 或等其重连后自动上报"
    else
      log "  [INFO] 网段未变 ($ip)"
    fi
  fi
}

case "${1:-check}" in
  baseline)
    {
      echo "WLAN_IP=$(ip -4 addr show wlan0 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1 | head -1)"
      echo "TS=$(date +%s)"
      echo "API_LOG_LINES=$(wc -l < "$DIR/logs/api.log" 2>/dev/null || echo 0)"
    } > "$BASE_FILE"
    log "=== 基线已记录 -> $BASE_FILE ==="
    cat "$BASE_FILE" | tee -a "$LOG"
    ;;
  check|*)
    log "======== §9 验收检查 $(date '+%F %T') ========"
    rc=0
    log "-- 1) 5 服务自启 --";   check_services || rc=1
    log "-- 2) Flask 绑定/端点 --"; check_flask  || rc=1
    log "-- 3) Mosquitto 边界 --"; check_mqtt   || rc=1
    log "-- 4) ESP32 链路 --";     check_esp32  || rc=1
    log "-- 5) 换段提示 --";       check_swap_hint
    if [ $rc -eq 0 ]; then log "==== 结论: 通过 ===="; else log "==== 结论: 有 FAIL，见上 ===="; fi
    exit $rc
    ;;
esac
