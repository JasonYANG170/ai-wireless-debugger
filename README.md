# 🔧 无线串口调试器 (Wireless Serial Debugger)

基于 ESP32-S3 的多功能无线硬件调试器，支持串口、SPI、I2C、PWM、SWD 等多种协议，并集成 AI 调试能力。

## 项目展示

![AI 远程调试器项目展示](docs/images/project-hardware.webp)

[硬件项目与图片来源](https://oshwhub.com/course-examples/project_ddnrazxm)

## ✨ 功能特点

### 📡 多协议调试
- **串口调试**：支持 UART 串口数据收发，可配置波特率 (9600/115200/460800/921600)
- **SPI 监控**：实时捕获和分析 SPI 总线数据
- **I2C 监控**：I2C 总线事务捕获和分析
- **PWM 测量**：PWM 频率和占空比测量
- **SWD 调试**：支持 CMSIS-DAP 协议的 ARM 调试接口

### 📶 无线连接
- **WiFi 连接**：支持 STA/AP 模式，可通过 Web 界面配置
- **TCP 服务器**：端口 3333，支持多客户端同时连接
- **HTTP 状态页**：端口 80，实时显示设备状态和配置

### 🖥️ 用户界面
- **LCD 显示屏**：横屏显示，实时状态监控
- **菜单系统**：通过物理按键导航，支持多级菜单
- **按键操作**：
  - SW1：下一个选项 / 长按进入 AP 配置模式
  - SW2：确认 / 长按返回上级 / 按住执行特殊功能
  - SW3：上一个选项 / 长按切换波特率

### 🤖 AI 集成 (MCP Server)
- **Model Context Protocol**：让 AI 工具直接调试硬件
- **支持的 AI 工具**：
  - Claude Code / Claude Desktop
  - Cursor
  - OpenCode
  - 其他支持 MCP 的工具

### 引脚分配
引脚功能可通过软件动态配置，支持以下协议：
- UART (TX/RX)
- SPI (SCK/MOSI/MISO/CS)
- I2C (SDA/SCL)
- PWM 输出
- SWD (SWCLK/SWDIO/NRST)

## 📦 安装与编译

### 环境准备
1. 安装 [ESP-IDF](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/) 开发环境
2. 配置 ESP-IDF 环境变量

### 编译步骤
```bash
# 克隆项目
git clone https://github.com/yourusername/ai-wireless-debugger.git
cd ai-wireless-debugger

# 配置项目 (可选)
idf.py menuconfig

# 编译
idf.py build

# 烧录到设备
idf.py -p COMx flash monitor
```

### MCP Server 安装 (AI 调试功能)
```bash
# 进入 MCP 目录
cd mcp

# 安装 Python 依赖
pip install -r requirements.txt

# 注册到 AI 工具 (以 Claude Code 为例)
claude mcp add wireless-debugger --scope user -- python mcp/mcp_server.py
```

## 🚀 使用指南

### 首次使用
1. **烧录固件**：将编译好的固件烧录到 ESP32-S3
2. **连接 WiFi**：
   - 长按 SW1 进入 AP 配置模式
   - 连接设备创建的 WiFi 热点
   - 访问配置页面设置 WiFi 信息
3. **查看 IP**：LCD 屏幕会显示分配的 IP 地址

### 日常使用
- **串口调试**：通过 TCP 客户端连接 `tcp://<IP>:3333`
- **Web 监控**：浏览器访问 `http://<IP>:80`
- **AI 调试**：配置 MCP Server 后，AI 工具可自动连接设备

### AI 调试示例
配置 MCP Server 后，可以对 AI 说：
- "连接我的调试器，IP 是 192.168.1.100"
- "读取串口数据"
- "发送 AT 指令到设备"
- "检查 SPI 总线上的数据"
- "测量 PWM 信号的频率"

## 📁 项目结构
```
ai-wireless-debugger/
├── main/                    # 主程序代码
│   ├── main.c              # 程序入口
│   ├── pin_config.c/h      # 引脚配置
│   ├── pwm_mon.c/h         # PWM 监控
│   ├── spi_mon.c/h         # SPI 监控
│   ├── i2c_mon.c/h         # I2C 监控
│   ├── buttons/            # 按键驱动
│   ├── lcd/                # LCD 驱动和 UI
│   ├── net/                # 网络服务 (TCP/HTTP)
│   ├── serial/             # 串口桥接
│   ├── swd/                # SWD 调试接口
│   └── wifi/               # WiFi 管理
├── components/              # 自定义组件
├── mcp/                     # MCP Server (AI 集成)
│   ├── mcp_server.py       # MCP 服务器主程序
│   ├── requirements.txt    # Python 依赖
│   └── README.md           # MCP 使用说明
├── tools/                   # 工具脚本
├── CMakeLists.txt          # 项目配置
└── README.md               # 项目说明
```

## 🔧 配置说明

### WiFi 配置
- **STA 模式**：连接现有 WiFi 网络
- **AP 模式**：创建 WiFi 热点，用于初始配置

### 串口配置
- 默认波特率：115200
- 支持动态切换：9600 / 115200 / 460800 / 921600

### MCP Server 配置
配置文件 `mcp/mcp_config.json` 会自动保存设备 IP：
```json
{
  "device_ip": "192.168.1.100"
}
```

## 🐛 故障排除

### 常见问题
1. **无法连接 WiFi**
   - 检查 WiFi 密码是否正确
   - 确保设备在 WiFi 信号范围内
   - 尝试长按 SW1 重新配置

2. **串口无数据**
   - 检查波特率设置是否匹配
   - 确认接线正确 (TX/RX 交叉连接)
   - 检查目标设备是否正常工作

3. **MCP Server 连接失败**
   - 确认设备 IP 地址正确
   - 检查设备是否在同一网络
   - 查看 MCP Server 日志输出

### 调试模式
- 通过串口监控查看系统日志：`idf.py monitor`
- Web 界面实时查看设备状态
- LCD 屏幕显示当前工作状态


