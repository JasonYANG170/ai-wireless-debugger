[简体中文](README.md) | [English](README_en.md)

# 🔧 Wireless Serial Debugger

A multi-functional wireless hardware debugger based on ESP32-S3, supports multiple protocols such as serial port, SPI, I2C, PWM, SWD, etc., and integrates AI debugging capabilities.

## Project showcase

![AI remote debugger project display](docs/images/project-hardware.webp)

[Hardware project and image source](https://oshwhub.com/course-examples/project_ddnrazxm)

## ✨ Features

### 📡 Multi-protocol debugging
- **Serial port debugging**: Supports UART serial port data sending and receiving, configurable baud rate (9600/115200/460800/921600)
- **SPI Monitor**: Capture and analyze SPI bus data in real time
- **I2C Monitor**: I2C bus transaction capture and analysis
- **PWM Measurement**: PWM frequency and duty cycle measurement
- **SWD debugging**: ARM debugging interface supporting CMSIS-DAP protocol

### 📶 Wireless connection
- **WiFi Connection**: Supports STA/AP mode, configurable via web interface
- **TCP Server**: Port 3333, supports multiple clients connecting at the same time
- **HTTP Status Page**: Port 80, real-time display of device status and configuration

### 🖥️ User Interface
- **LCD display**: horizontal screen display, real-time status monitoring
- **Menu System**: Navigation via physical buttons, supports multi-level menus
- **Key operation**:
  - SW1: Next option/long press to enter AP configuration mode
  - SW2: Confirm / long press to return to the previous level / press and hold to perform special functions
  - SW3: Previous option/long press to switch baud rate

### 🤖 AI Integration (MCP Server)
- **Model Context Protocol**: Let AI tools debug hardware directly
- **Supported AI Tools**:
  - Claude Code / Claude Desktop
  - Cursor
  - OpenCode
  - Other tools that support MCP

### Pin assignment
Pin functions are dynamically configurable through software and support the following protocols:
- UART (TX/RX)
- SPI (SCK/MOSI/MISO/CS)
- I2C (SDA/SCL)
- PWM output
- SWD (SWCLK/SWDIO/NRST)

## 📦 Installation and compilation

### Environment preparation
1. Install the [ESP-IDF](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/) development environment
2. Configure ESP-IDF environment variables

### Compilation steps
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

### MCP Server installation (AI debugging function)
```bash
# 进入 MCP 目录
cd mcp

# 安装 Python 依赖
pip install -r requirements.txt

# 注册到 AI 工具 (以 Claude Code 为例)
claude mcp add wireless-debugger --scope user -- python mcp/mcp_server.py
```

## 🚀 User Guide

### First time use
1. **Flash firmware**: Flash the compiled firmware to the ESP32-S3
2. **Connect to WiFi**:
   - Long press SW1 to enter AP configuration mode
   - Connect to WiFi hotspots created by your device
   - Visit the configuration page to set WiFi information
3. **View IP**: The LCD screen will display the assigned IP address

### Daily use
- **Serial Port Debugging**: Connect via TCP client `tcp://<IP>:3333`
- **Web Monitoring**: Browser access `http://<IP>:80`
- **AI debugging**: After configuring the MCP Server, the AI tool can automatically connect to the device

### AI Debugging Example
After configuring the MCP Server, you can say to the AI:
- "Connect to my debugger, IP is 192.168.1.100"
- "Read serial port data"
- "Send AT commands to device"
- "Check data on SPI bus"
- "Measuring the frequency of a PWM signal"

## 📁 Project structure
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

## 🔧 Configuration instructions

### WiFi configuration
- **STA Mode**: Connect to existing WiFi network
- **AP Mode**: Create WiFi hotspot for initial configuration

### Serial port configuration
- Default baud rate: 115200
- Support dynamic switching: 9600 / 115200 / 460800 / 921600

### MCP Server Configuration
The configuration file `mcp/mcp_config.json` will automatically save the device IP:
```json
{
  "device_ip": "192.168.1.100"
}
```

## 🐛 Troubleshooting

### FAQ
1. **Unable to connect to WiFi**
   - Check if WiFi password is correct
   - Make sure your device is within WiFi range
   - Try long pressing SW1 to reconfigure

2. **No data in serial port**
   - Check if the baud rate settings match
   - Verify wiring is correct (TX/RX cross-connect)
   - Check if the target device is working properly

3. **MCP Server connection failed**
   - Confirm that the device IP address is correct
   - Check if the device is on the same network
   - View MCP Server log output

### Debug mode
- Check the system log through serial port monitoring: `idf.py monitor`
- Web interface to view device status in real time
- LCD screen displays current working status


