# 脱敏配置示例（片段）

> ⚠️ 这是**示例**，不是可直接使用的配置。请**按你的 harness 适配**，并把 `<...>` 换成你自己的值。
> 真实凭证只放本地（`.env` / 环境变量），**永不进 git**。

## 示例 A：企微机器人凭证（.env 片段）

```dotenv
# 企业微信「智能机器人」凭证 —— 只放本地，.env 不进 git
WECOM_BOT_ID=<YOUR_BOT_ID>
WECOM_BOT_SECRET=<YOUR_BOT_SECRET>
# 若用 Agent（自建应用）模式才需要：
# WECOM_CORP_ID=<YOUR_CORP_ID>
```

## 示例 B：渠道注册（伪配置结构，非某框架真实语法）

不同 harness 的配置键名不同。**核心字段**通常就是这些：

```jsonc
{
  "channels": {
    "wecom": {
      "enabled": true,
      "botId": "<YOUR_BOT_ID>",
      "secret": "<YOUR_BOT_SECRET>",
      "connectionMode": "websocket",   // 长连接，免公网回调
      "dmPolicy": "allowlist",         // 私聊收紧
      "allowFrom": ["<YOUR_USERID>"]   // 私聊白名单（群聊靠 @ 触发）
    }
  }
}
```

> 把 `dmPolicy`/`allowFrom` 之类的**意图**理解清楚即可：**私聊只允许你本人，群聊靠 @ 触发**。
> 不要照抄具体键名和值——你的 harness 可能完全不同。

## 示例 C：ESP32 端侧地址（Pi Hub 的 `config` 表 / `.env`）

```dotenv
# Pi 侧记录的 ESP32 局域网地址（只用于控制链路：Pi → 设备）
smalltv_ip=<ESP32_IP>
```

> ⚠️ **两处地址都要人工维护，而且方向相反 —— 别搞混：**
>
> | 配在哪 | 变量 / 位置 | 服务于哪个方向 |
> |---|---|---|
> | **Pi 侧** | `smalltv_ip`（**就是这个大小写**）| **控制**：Pi → 设备 |
> | **ESP32 固件** | 网页里改的 Hub 地址（存 NVS） | **取数**：设备 → Pi |
>
> **换网络后两处都要改。** Pi 侧另有一个 `/api/esp32_ip` 登记接口，
> 但**当前固件不会自动调用**，所以仍需手工填一次 —— 别指望它自动同步
> （见主仓库 README 的「已知限制」与「路线图」）。

---

## 反面清单（别这么做）

- ❌ 不要把完整的、真实的 agent 配置文件整个发出去。
- ❌ 不要在示例里放真实 botId / secret / corpId / chatid / userid。
- ❌ 不要为了「看起来能跑」而给出与你自己环境强绑定的路径、IP、群 id。
