#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pi Hub 指令解析（§7.2 企微指令 → hub/command/smalltv 的 {action,value}）。

被 wecom_bot（解析群消息）与 devctl（执行）共用，保证单一来源。

约定（§3）：
  {"action": "set_mode", "value": 3}          # 0时钟 1天气 2相册 3股票
  {"action": "set_brightness", "value": 60}   # 0-100
  {"action": "set_brightness_rel", "value": +20}
  {"action": "set_stockview", "value": 1}      # 0分时图 1日K
  {"action": "set_auto_brightness", "value": 1}  # 自动亮度
  {"action": "set_wallpaper", "value": 1, "extra": {"idx": 0}}  # 0无 1静态 2动态
                                                          # extra.idx 仅静态时有意义(0..2)

parse_command 返回 (action, value, extra)；extra 为可选 dict（无则 None），
用于携带壁纸索引等附加参数，保持对既有二元调用方向的向后兼容。
"""
import re

MODE_NAMES = {0: "时钟", 1: "天气", 2: "相册", 3: "股票"}
MODE_KEYWORDS = {
    0: ["时钟", "时间", "看钟", "clock"],
    1: ["天气", "weather"],
    2: ["相册", "照片", "图片", "photo", "album"],
    3: ["股票", "行情", "股价", "stock"],
}

WALLPAPER_NAMES = {0: "关闭", 1: "静态", 2: "动态"}

HELP_TEXT = ("可用指令：\n"
             "- 切换到时钟 / 天气 / 相册 / 股票\n"
             "- 亮度 60（0-100）/ 调亮 / 调暗 / 自动亮度\n"
             "- 分时图 / 日K图\n"
             "- 静态壁纸 / 动态壁纸 / 关闭壁纸\n"
             "- 壁纸1 / 壁纸2 / 壁纸3（选具体图片）")


def parse_command(text):
    """把群消息文本解析为 (action, value)；无法识别返回 (None, None)。

    兼容用户 @机器人 时的前缀（如 "@小龙虾-派 切换到股票"）。
    """
    if not text:
        return None, None, None
    # 去掉 @xxx 前缀
    t = re.sub(r"@\S+", " ", text).strip()

    # 亮度：数字优先
    m = re.search(r"(亮度|brightness)\D{0,4}(\d{1,3})", t, re.I)
    if m:
        val = int(m.group(2))
        if 0 <= val <= 100:
            return "set_brightness", val, None
    # 相对亮度
    if any(k in t for k in ("调亮", "亮一点", "变亮")):
        return "set_brightness_rel", +20, None
    if any(k in t for k in ("调暗", "暗一点", "变暗")):
        return "set_brightness_rel", -20, None

    # 自动亮度（放在亮度数值之后、模式切换之前；避免被“亮度”前缀误吞）
    if any(k in t for k in ("自动亮度", "自动调节亮度", "auto_brightness", "auto brightness")):
        return "set_auto_brightness", 1, None

    # 股票视图：日K / 分时图（必须含“图”字，避免和“股票模式”冲突）
    if any(k in t for k in ("日k", "日K", "日k图", "日K图", "k线图", "K线图")):
        return "set_stockview", 1, None
    if any(k in t for k in ("分时图", "分时线", "分时")):
        return "set_stockview", 0, None

    # 壁纸：先看“哪个/第几张/编号”是否指定了具体图片（图片1/图2/第3张...）
    wp_idx = _parse_wallpaper_index(t)
    # 动态壁纸
    if any(k in t for k in ("动态壁纸", "动态墙纸")):
        return "set_wallpaper", 2, None
    # 静态壁纸（含指定图片号的“壁纸2”等）
    if any(k in t for k in ("静态壁纸", "静态墙纸")) or wp_idx is not None:
        return "set_wallpaper", 1, {"idx": wp_idx if wp_idx is not None else 0}
    # 关闭壁纸
    if any(k in t for k in ("关闭壁纸", "关壁纸", "关掉壁纸", "取消壁纸")):
        return "set_wallpaper", 0, None

    # 模式切换
    for mode, kws in MODE_KEYWORDS.items():
        if any(k.lower() in t.lower() for k in kws):
            return "set_mode", mode, None

    if any(k in t for k in ("帮助", "help", "指令", "怎么用")):
        return "help", None, None
    return None, None, None


def _parse_wallpaper_index(t):
    """从文本中解析“指定第几张壁纸”，返回 0..2，否则 None。

    支持：壁纸1 / 壁纸 2 / 壁纸二 / 第3张壁纸 / 图片1 / 图2 / 第三张
    忽略“静态壁纸”“动态壁纸”“关闭壁纸”这类无编号表述（返回 None）。
    """
    cn = {"一": 1, "二": 2, "三": 3, "两": 2}
    # 先干掉“静态/动态/关闭”前缀，避免把“壁纸”当成编号载体误判
    s = t.replace("静态", "").replace("动态", "").replace("关闭", "").replace("取消", "")
    # 阿拉伯数字：壁纸 2 / 第2张 / 图片2 / 图2
    m = re.search(r"(?:壁纸|墙纸|图片|图|第)\s*(\d)", s)
    if m:
        n = int(m.group(1))
        if 1 <= n <= 3:
            return n - 1
    # 中文数字：壁纸二 / 第三张
    m = re.search(r"(?:壁纸|墙纸|图片|图|第)\s*([一二三两])", s)
    if m:
        n = cn.get(m.group(1))
        if n and 1 <= n <= 3:
            return n - 1
    return None


def describe(action, value, idx=None):
    """执行结果 → 人类可读描述（回执用）。idx 仅静态壁纸时有意义。"""
    if action == "set_mode":
        return f"已切换到{MODE_NAMES.get(value, value)}模式"
    if action == "set_brightness":
        return f"亮度已设为 {value}"
    if action == "set_brightness_rel":
        return "亮度已调整"
    if action == "set_stockview":
        return f"已切换到{'日K图' if value == 1 else '分时图'}"
    if action == "set_auto_brightness":
        return "已开启自动亮度"
    if action == "set_wallpaper":
        if value == 1 and idx is not None:
            return f"已切换到静态壁纸{idx + 1}"
        return f"壁纸已{WALLPAPER_NAMES.get(value, value)}"
    if action == "help":
        return HELP_TEXT
    return "指令已执行"


if __name__ == "__main__":
    for s in ["@小龙虾-派 切换到股票", "看天气", "亮度 60", "调亮", "帮助",
              "分时图", "日K图", "自动亮度", "静态壁纸", "动态壁纸", "关闭壁纸",
              "壁纸1", "壁纸 2", "壁纸二", "第3张壁纸", "图片2", "图3", "随便说点什么"]:
        print(repr(s), "->", parse_command(s))
