# 无线串口调试器 MCP Server

让 AI（Claude / Cursor / OpenCode 等）远程调试硬件设备。

## 安装

### 1. 安装依赖

```bash
cd D:\esp\wireless-wireless-debugger\mcp
pip install -r requirements.txt
```

### 2. 注册到你的 AI 工具

#### Claude Code

```bash
claude mcp add wireless-debugger --scope user -- python D:/esp/wireless-wireless-debugger/mcp/mcp_server.py
```

#### Claude Desktop

编辑 `%APPDATA%\Claude\claude_desktop_config.json`：

```json
{
  "mcpServers": {
    "wireless-debugger": {
      "command": "python",
      "args": ["D:/esp/wireless-wireless-debugger/mcp/mcp_server.py"]
    }
  }
}
```

#### Cursor

编辑 `~/.cursor/mcp.json`（全局）或项目 `.cursor/mcp.json`：

```json
{
  "mcpServers": {
    "wireless-debugger": {
      "command": "python",
      "args": ["D:/esp/wireless-wireless-debugger/mcp/mcp_server.py"]
    }
  }
}
```

#### OpenCode

```bash
opencode mcp add wireless-debugger -- python D:/esp/wireless-wireless-debugger/mcp/mcp_server.py
```

或手动编辑 OpenCode 配置文件，添加：

```json
{
  "wireless-debugger": {
    "type": "local",
    "command": ["python", "D:/esp/wireless-wireless-debugger/mcp/mcp_server.py"]
  }
}
```

#### 其他支持 MCP 的工具

引用本目录下的 `config.json`，或按各工具文档填入：

| 字段 | 值 |
|------|-----|
| command | `python` |
| args | `D:/esp/wireless-wireless-debugger/mcp/mcp_server.py` |

### 3. 使用

首次连接时 AI 会询问设备 IP（设备屏幕上显示），输入后自动保存到 `mcp_config.json`，下次自动连接。

## 可用工具

| 工具 | 功能 |
|------|------|
| `connect_device` | 连接设备（AI 询问 IP） |
| `disconnect_device` | 断开连接 |
| `get_status` | WiFi/IP/波特率/收发计数 |
| `read_serial` | 读串口数据（独立缓冲区，不抢 Web） |
| `send_serial` | 发串口数据 |
| `set_baud` | 设波特率 |
| `read_pwm` / `send_pwm` | PWM 读/发 |
| `read_spi` / `send_spi` | SPI 读/发 |
| `read_i2c` / `send_i2c` | I2C 读/发 |
| `get_pins` / `set_pins` | 引脚协议分配 |
| `press_button` | 模拟按钮 |
| `get_capacity` | 缓冲区占用/剩余内存 |
| `clear_buffer` | 清空缓冲区 |
| `set_buffer_config` | 调整缓冲区大小 |

## 配置文件

- `mcp_config.json` — 设备 IP（首次输入后自动生成）
