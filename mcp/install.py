#!/usr/bin/env python
"""
MCP Server 安装脚本 —— 自动生成 JSON 配置并写入对应工具的配置文件。

用法:
  python install.py              # 交互式选择工具
  python install.py claude       # Claude Desktop
  python install.py cursor       # Cursor
  python install.py opencode     # OpenCode
  python install.py all          # 全部安装
  python install.py --print      # 只打印 JSON 不写文件
"""

import json
import os
import sys
import platform

# ── 路径 ──
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SERVER_PY = os.path.join(SCRIPT_DIR, "mcp_server.py")
PYTHON = sys.executable or "python"

# 统一用正斜杠（JSON 中 Windows 路径兼容）
SERVER_PY_FMT = SERVER_PY.replace("\\", "/")
PYTHON_FMT = PYTHON.replace("\\", "/")

# ── 各工具的配置格式 ──
TOOLS = {
    "claude": {
        "name": "Claude Desktop",
        "format": "claude_desktop",
        "config_path": {
            "Windows": os.path.expandvars(r"%APPDATA%\Claude\claude_desktop_config.json"),
            "Darwin":  os.path.expanduser("~/Library/Application Support/Claude/claude_desktop_config.json"),
        },
    },
    "cursor": {
        "name": "Cursor",
        "format": "cursor",
        "config_path": {
            "Windows": os.path.expanduser("~/.cursor/mcp.json"),
            "Darwin":  os.path.expanduser("~/.cursor/mcp.json"),
        },
    },
    "opencode": {
        "name": "OpenCode",
        "format": "opencode",
        "config_path": {
            "Windows": os.path.expanduser("~/.opencode/config.json"),
            "Darwin":  os.path.expanduser("~/.opencode/config.json"),
        },
    },
    "cline": {
        "name": "Cline (VSCode)",
        "format": "cline",
        "config_path": {
            "Windows": os.path.expanduser("~/AppData/Roaming/Code/User/globalStorage/saoudrizwan.claude-dev/settings/cline_mcp_settings.json"),
            "Darwin":  os.path.expanduser("~/Library/Application Support/Code/User/globalStorage/saoudrizwan.claude-dev/settings/cline_mcp_settings.json"),
        },
    },
}


def build_config(fmt):
    """根据工具格式生成 JSON 配置字典。"""
    server_entry = {
        "command": PYTHON_FMT,
        "args": [SERVER_PY_FMT],
    }
    if fmt == "opencode":
        # OpenCode 用 command 数组格式
        return {
            "wireless-debugger": {
                "type": "local",
                "command": [PYTHON_FMT, SERVER_PY_FMT],
            }
        }
    else:
        # Claude / Cursor / Cline 都用 mcpServers 包裹
        return {
            "mcpServers": {
                "wireless-debugger": server_entry,
            }
        }


def get_config_path(tool_key):
    """获取当前平台的配置文件路径。"""
    tool = TOOLS[tool_key]
    plat = platform.system()
    path = tool["config_path"].get(plat) or tool["config_path"].get("Windows")
    return path


def merge_config(path, new_config):
    """读取已有配置，合并 wireless-debugger，写回。"""
    existing = {}
    if os.path.exists(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                existing = json.load(f)
        except (json.JSONDecodeError, IOError):
            existing = {}

    if "mcpServers" in new_config:
        # Claude/Cursor/Cline 格式：合并 mcpServers
        if "mcpServers" not in existing:
            existing["mcpServers"] = {}
        existing["mcpServers"]["wireless-debugger"] = new_config["mcpServers"]["wireless-debugger"]
    else:
        # OpenCode 格式：直接合并顶层 key
        existing["wireless-debugger"] = new_config["wireless-debugger"]

    return existing


def install_tool(tool_key, dry_run=False):
    """安装到指定工具。"""
    tool = TOOLS[tool_key]
    config = build_config(tool["format"])
    path = get_config_path(tool_key)

    print(f"\n{'─' * 50}")
    print(f"  {tool['name']}")
    print(f"{'─' * 50}")
    print(f"  配置路径: {path}")
    print(f"  Python:   {PYTHON_FMT}")
    print(f"  Server:   {SERVER_PY_FMT}")

    if dry_run:
        print(f"\n  JSON 配置:")
        print(json.dumps(config, indent=2, ensure_ascii=False))
        return True

    # 写入
    merged = merge_config(path, config)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(merged, f, indent=2, ensure_ascii=False)

    print(f"  ✅ 已写入 {path}")
    return True


def main():
    args = sys.argv[1:]
    dry_run = "--print" in args
    args = [a for a in args if a != "--print"]

    print("=" * 50)
    print("  无线串口调试器 — MCP Server 安装")
    print("=" * 50)
    print(f"  Server: {SERVER_PY_FMT}")

    if not args:
        # 交互式
        print("\n选择要安装到的 AI 工具:")
        keys = list(TOOLS.keys())
        for i, k in enumerate(keys, 1):
            print(f"  {i}. {TOOLS[k]['name']}")
        print(f"  {len(keys)+1}. 全部安装")
        print(f"  0. 只打印 JSON (--print)")

        try:
            choice = int(input("\n输入数字: "))
        except (ValueError, EOFError):
            print("无效输入")
            return

        if choice == 0:
            for k in keys:
                install_tool(k, dry_run=True)
            return
        if choice == len(keys) + 1:
            args = ["all"]
        elif 1 <= choice <= len(keys):
            args = [keys[choice - 1]]
        else:
            print("无效选择")
            return

    for arg in args:
        if arg == "all":
            for k in TOOLS:
                install_tool(k, dry_run)
        elif arg in TOOLS:
            install_tool(arg, dry_run)
        else:
            print(f"未知工具: {arg}")
            print(f"可选: {', '.join(TOOLS.keys())}, all")

    if not dry_run:
        print("\n✅ 安装完成。重启对应的 AI 工具即可使用。")
        print("   首次使用 AI 会询问设备 IP，输入后自动保存。")


if __name__ == "__main__":
    main()
