"""
MCP Server for Wireless Serial Debugger

AI 自动询问用户设备 IP，连接后即可远程调试。

Claude Desktop 配置 (claude_desktop_config.json):
{
  "mcpServers": {
    "wireless-debugger": {
      "command": "python",
      "args": ["D:/esp/wireless-serial-debugger/tools/mcp_server.py"]
    }
  }
}
"""

import os
import json
import httpx
from mcp.server.fastmcp import FastMCP

# ---------------------------------------------------------------------------
# 配置持久化
# ---------------------------------------------------------------------------

CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mcp_config.json")

def load_config() -> dict:
    if os.path.exists(CONFIG_FILE):
        with open(CONFIG_FILE, "r") as f:
            return json.load(f)
    return {}

def save_config(cfg: dict):
    with open(CONFIG_FILE, "w") as f:
        json.dump(cfg, f, indent=2)

# ---------------------------------------------------------------------------
# 全局状态
# ---------------------------------------------------------------------------

mcp = FastMCP("wireless-debugger")
client: httpx.Client | None = None
device_ip: str = ""

def _connect(ip: str):
    global client, device_ip
    if client:
        client.close()
    device_ip = ip
    client = httpx.Client(base_url=f"http://{ip}", timeout=10.0)
    # 验证连接
    r = client.get("/api/status")
    r.raise_for_status()
    save_config({"ip": ip})
    return r.json()

# 启动时自动加载已保存的 IP
_cfg = load_config()
if _cfg.get("ip"):
    try:
        _connect(_cfg["ip"])
    except Exception:
        pass  # 设备不在线，等用户手动连接

# ---------------------------------------------------------------------------
# 工具 — 连接管理
# ---------------------------------------------------------------------------

@mcp.tool()
def connect_device(ip: str = "") -> dict:
    """
    连接无线串口调试器设备。首次使用时需要用户提供设备 IP 地址。
    连接成功后 IP 会自动保存，下次启动无需重复配置。

    Args:
        ip: 设备 IP 地址，如 "192.168.1.129"。留空则尝试使用上次保存的 IP。
    """
    global client, device_ip
    if not ip:
        cfg = load_config()
        ip = cfg.get("ip", "")
    if not ip:
        return {"error": "请提供设备 IP 地址。设备开机后会在屏幕上显示 IP，或连接 SerialDbg-xxxx 热点后使用 192.168.4.1"}
    try:
        status = _connect(ip)
        return {"ok": True, "ip": ip, "status": status}
    except Exception as e:
        return {"error": f"连接失败 ({ip}): {e}"}


@mcp.tool()
def disconnect_device() -> str:
    """断开当前设备连接。"""
    global client, device_ip
    if client:
        client.close()
        client = None
    old = device_ip
    device_ip = ""
    return f"已断开 {old}"


# ---------------------------------------------------------------------------
# 工具 — 设备操作
# ---------------------------------------------------------------------------

def _require_client() -> httpx.Client:
    if not client:
        raise RuntimeError("未连接设备。请先调用 connect_device 并提供设备 IP 地址。")
    return client

@mcp.tool()
def get_status() -> dict:
    """获取设备状态：WiFi连接、IP地址、串口波特率、收发字节数、运行时间、当前菜单页面。"""
    c = _require_client()
    r = c.get("/api/status")
    r.raise_for_status()
    return r.json()


@mcp.tool()
def read_serial(max_bytes: int = 4096) -> str:
    """
    从目标设备读取串口接收到的数据（文本形式）。
    每次调用会清空缓冲区，适合轮询式读取。单次最多4KB。
    """
    c = _require_client()
    r = c.get("/api/mcp/data")
    r.raise_for_status()
    return r.text


@mcp.tool()
def send_serial(data: str, newline: str = "lf") -> str:
    """
    向目标设备发送串口数据。

    Args:
        data: 要发送的文本内容
        newline: 换行符 — "none"=无, "lf"=\\n, "cr"=\\r, "crlf"=\\r\\n
    """
    c = _require_client()
    suffix = {"none": "", "lf": "\n", "cr": "\r", "crlf": "\r\n"}.get(newline, "\n")
    r = c.post("/api/send", content=data + suffix)
    r.raise_for_status()
    return "OK"


@mcp.tool()
def set_baud(baud: int) -> str:
    """
    设置目标设备串口波特率。

    Args:
        baud: 如 9600, 115200, 460800, 921600
    """
    c = _require_client()
    r = c.get(f"/api/baud?b={baud}")
    r.raise_for_status()
    return "OK"


@mcp.tool()
def read_pwm() -> dict:
    """读取 PWM 频率(Hz)和占空比(%)。需先在设备上分配 PWM 引脚。"""
    c = _require_client()
    r = c.get("/api/pwm")
    r.raise_for_status()
    return r.json()


@mcp.tool()
def read_spi(count: int = 5) -> dict:
    """
    读取 SPI 从机捕获的事务历史（MOSI/MISO 字节）。
    需先在设备上分配 SCK/MOSI/MISO/CS 引脚。

    Args:
        count: 返回最近几笔事务 (默认5)
    """
    c = _require_client()
    r = c.get("/api/spi")
    r.raise_for_status()
    data = r.json()
    if "history" in data:
        data["history"] = data["history"][-count:]
    return data


@mcp.tool()
def read_i2c(count: int = 5) -> dict:
    """
    读取 I2C 总线捕获的事务历史。
    返回: running状态, 总事务数, ISR计数, 最近事务列表(含地址/方向/数据)。
    需要先在设备上分配 SDA/SCL 引脚。

    Args:
        count: 返回最近几笔事务 (默认5)
    """
    c = _require_client()
    r = c.get("/api/i2c")
    r.raise_for_status()
    data = r.json()
    if "history" in data:
        data["history"] = data["history"][-count:]
    return data


@mcp.tool()
def send_pwm(freq: float, duty: float = 50.0, enable: bool = True) -> dict:
    """
    控制 PWM 输出（LEDC 生成）。

    Args:
        freq: 输出频率 (Hz)
        duty: 占空比 0~100 (默认 50%)
        enable: true=开始输出, false=停止输出
    """
    c = _require_client()
    body = f"freq={freq}&duty={duty}&enable={'1' if enable else '0'}"
    r = c.post("/api/pwm/out", content=body,
               headers={"Content-Type": "application/x-www-form-urlencoded"})
    r.raise_for_status()
    return r.json()


@mcp.tool()
def send_spi(hex_data: str, mode: int = 0, clock: int = 1000000) -> dict:
    """
    通过 SPI 主机发送数据。自动停止 SPI 从机监听并切换为主机模式。

    Args:
        hex_data: 要发送的十六进制字节串，如 "AA55DEADBEEF"
        mode: SPI 模式 0/1/2/3 (默认 0)
        clock: 时钟频率 Hz (默认 1MHz)
    """
    c = _require_client()
    body = f"hex={hex_data}&mode={mode}&clk={clock}"
    r = c.post("/api/spi/send", content=body,
               headers={"Content-Type": "application/x-www-form-urlencoded"})
    r.raise_for_status()
    return r.json()


@mcp.tool()
def send_i2c(addr: int, hex_data: str = "", mode: str = "w", read_len: int = 1) -> dict:
    """
    通过 I2C 主机发送/接收数据。自动停止 I2C 从机监听并切换为主机模式。

    Args:
        addr: 7-bit 从机地址 (如 0x50 表示 EEPROM)
        hex_data: 十六进制数据 (写操作时使用，如 "00A5")
        mode: "w"=写, "r"=读, "wr"=写后读 (寄存器读取)
        read_len: 读取字节数 (mode=r 或 wr 时使用，默认1)
    """
    c = _require_client()
    body = f"addr={addr}&hex={hex_data}&mode={mode}&len={read_len}"
    r = c.post("/api/i2c/send", content=body,
               headers={"Content-Type": "application/x-www-form-urlencoded"})
    r.raise_for_status()
    return r.json()


@mcp.tool()
def get_capacity() -> dict:
    """
    获取设备存储容量和各缓冲区使用情况。
    返回: free_heap(剩余堆栈KB), 以及 serial/spi/i2c/rx 各自的 {n:当前条数, max:容量}。
    用于判断是否需要清理缓冲区。
    """
    c = _require_client()
    r = c.get("/api/capacity")
    r.raise_for_status()
    return r.json()


@mcp.tool()
def clear_buffer(target: str = "all") -> str:
    """
    清空缓冲区，避免空间不足。

    Args:
        target: "serial"=串口, "spi"=SPI历史, "i2c"=I2C历史, "rx"=RX历史, "all"=全部(默认)
    """
    c = _require_client()
    r = c.post(f"/api/clear?what={target}")
    r.raise_for_status()
    return f"cleared {target}"


@mcp.tool()
def set_buffer_config(serial_buf: int = 0, spi_hist: int = 0,
                      i2c_hist: int = 0, rx_hist: int = 0) -> dict:
    """
    调整缓冲区/历史容量，让 AI 能获取更多数据。已设安全界限防止溢出分区。
    只传需要改的参数，0 表示不修改。

    Args:
        serial_buf: 串口缓冲字节 (512-16384，默认0不改)
        spi_hist: SPI历史笔数 (2-100)
        i2c_hist: I2C历史笔数 (2-100)
        rx_hist: RX历史行数 (2-100)

    Returns: 实际生效的值（已被设备钳位到安全范围）
    """
    c = _require_client()
    result = {}
    if serial_buf:
        r = c.post(f"/api/cfg?key=buf&val={serial_buf}"); r.raise_for_status()
        result["serial_buf"] = r.json().get("val")
    if spi_hist:
        r = c.post(f"/api/cfg?key=spihist&val={spi_hist}"); r.raise_for_status()
        result["spi_hist"] = r.json().get("val")
    if i2c_hist:
        r = c.post(f"/api/cfg?key=i2chist&val={i2c_hist}"); r.raise_for_status()
        result["i2c_hist"] = r.json().get("val")
    if rx_hist:
        r = c.post(f"/api/cfg?key=rxhist&val={rx_hist}"); r.raise_for_status()
        result["rx_hist"] = r.json().get("val")
    return result


@mcp.tool()
def get_pins() -> dict:
    """
    获取当前引脚协议分配。
    可用协议: SWCLK, SWDIO, NRST, TX, RX, PWM, SCK, MOSI, MISO, CS, SDA, SCL
    """
    c = _require_client()
    r = c.get("/api/pins")
    r.raise_for_status()
    return r.json()


@mcp.tool()
def set_pins(order: str) -> str:
    """
    设置引脚协议分配（逗号分隔5个协议名）。

    Args:
        order: 如 "TX,RX,NRST,SWDIO,SWCLK" 或 "SDA,SCL,MOSI,SCK,PWM"
    """
    c = _require_client()
    r = c.post("/api/pins", content=f"order={order}",
               headers={"Content-Type": "application/x-www-form-urlencoded"})
    r.raise_for_status()
    return r.text


@mcp.tool()
def press_button(btn: int, action: str = "press") -> str:
    """
    模拟设备按钮操作。

    Args:
        btn: 1=SW1, 2=SW2, 3=SW3
        action: "press"=短按, "long"=长按(返回主页)
    """
    c = _require_client()
    r = c.get(f"/api/btn?b={btn}&a={action}")
    r.raise_for_status()
    return "OK"


# ---------------------------------------------------------------------------
# 资源 — 设备状态摘要
# ---------------------------------------------------------------------------

@mcp.resource("debugger://status")
def device_status() -> str:
    """设备当前状态摘要"""
    if not client:
        return "未连接设备。请先告知设备 IP 地址。"
    try:
        s = client.get("/api/status").json()
        pwm = client.get("/api/pwm").json()
        spi = client.get("/api/spi").json()
        i2c = client.get("/api/i2c").json()
        pins = client.get("/api/pins").json()
    except Exception as e:
        return f"设备连接失��: {e}"

    lines = [
        f"=== 无线串口调试器 ({device_ip}) ===",
        f"WiFi: {s['wifi']}  IP: {s['ip']}",
        f"串口: {s['baud']} baud  RX: {s['rx']}B  TX: {s['tx']}B",
        f"运行: {s['uptime']}s  TCP客户端: {s['tcp']}",
        "",
        f"PWM: {pwm['freq']:.2f} Hz, {pwm['duty']:.1f}%  (pin={pwm['pin']})",
        f"SPI: {'运行中' if spi['running'] else '未启动'}, {spi['count']}笔事务",
        f"I2C: {'运行中' if i2c['running'] else '未启动'}, {i2c['count']}笔事务",
        f"DAP TCP: :5555 (CMSIS-DAP for OpenOCD)",
        "",
        "引脚分配:",
    ]
    p = pins.get("order", [])
    io = pins.get("pos_io", [])
    for i in range(len(p)):
        lines.append(f"  IO{io[i]}: {p[i]}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------

if __name__ == "__main__":
    mcp.run()
