#!/usr/bin/env python3
"""
wifi_serial_bridge.py — 全自动虚拟串口桥接

一键运行：自动检测 com0com → 自动创建虚拟串口对 → 自动连接 ESP32 → 透明转发。

    python wifi_serial_bridge.py                      # 全自动
    python wifi_serial_bridge.py --host 192.168.1.129  # 指定 IP
    python wifi_serial_bridge.py --status              # 查看状态

你的串口工具（PuTTY/Arduino/SSCOM 等）打开 APP 端 COM 口即可通信。
"""

import logging
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    import subprocess
    print("⏳ 正在安装 pyserial...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "pyserial"])
    import serial
    from serial.tools import list_ports

log = logging.getLogger("bridge")

# ── 配置 ──
DEFAULT_HOST = "192.168.1.129"
TCP_PORT = 3333
BAUD = 115200


# ════════════════════════════════════════
# 1. 自动检测 com0com 虚拟串口
# ════════════════════════════════════════

def detect_com0com_ports():
    """通过 pyserial hwid 自动检测 com0com 虚拟串口对，返回 (app_port, bridge_port) 或 None。
       CNCA* 是 A 端(给终端用)，CNCB* 是 B 端(脚本桥接用)。"""
    a_port, b_port = None, None
    for p in list_ports.comports():
        if "COM0COM" in p.hwid.upper() or "COM0COM" in p.description.upper():
            if "CNCA" in p.hwid.upper():
                a_port = p.device
            elif "CNCB" in p.hwid.upper():
                b_port = p.device
    if a_port and b_port:
        return a_port, b_port
    # 只找到一个或没找到
    com0com_ports = [p.device for p in list_ports.comports()
                     if "COM0COM" in p.hwid.upper() or "COM0COM" in p.description.upper()]
    if len(com0com_ports) >= 2:
        return com0com_ports[0], com0com_ports[1]
    return None


def find_setupc():
    """查找 com0com 的 setupc.exe。"""
    for p in (r"C:\Program Files (x86)\com0com\setupc.exe",
              r"C:\Program Files\com0com\setupc.exe"):
        if os.path.isfile(p):
            return p
    w = shutil.which("setupc.exe")
    return w if w and os.path.isfile(w) else None


def list_pairs(setupc):
    """检测已有的 com0com 虚拟串口对。优先用 pyserial，回退到 setupc list。"""
    # 方法1: pyserial (不需要管理员权限)
    pairs = []
    ports_by_cnc = {}
    for p in list_ports.comports():
        hwid_upper = p.hwid.upper()
        if "COM0COM" in hwid_upper:
            if "CNCA" in hwid_upper:
                ports_by_cnc["A"] = p.device
            elif "CNCB" in hwid_upper:
                ports_by_cnc["B"] = p.device
    if "A" in ports_by_cnc and "B" in ports_by_cnc:
        pairs.append(("CNCA", ports_by_cnc["A"], "CNCB", ports_by_cnc["B"]))
        return pairs

    # 方法2: setupc list (需要管理员权限)
    try:
        r = subprocess.run([setupc, "list"], capture_output=True, text=True,
                           timeout=5, cwd=os.path.dirname(setupc))
        entries = {}
        for line in r.stdout.splitlines():
            m = re.match(r"(\w+)\s+PortName=(\S+)", line.strip())
            if m:
                entries[m.group(1)] = m.group(2)
        for cn in sorted(entries):
            if cn.startswith("CNCA"):
                cn_b = cn.replace("A", "B")
                if cn_b in entries:
                    pairs.append((cn, entries[cn], cn_b, entries[cn_b]))
    except Exception:
        pass
    return pairs


def check_com0com():
    """检测 com0com 安装状态和已有端口对。返回 setupc 路径或 None。"""
    setupc = find_setupc()
    if not setupc:
        print("❌ com0com 未安装")
        print("   下载: https://com0com.sourceforge.net/")
        print("   安装后重新运行此脚本")
        return None

    pairs = list_pairs(setupc)
    if pairs:
        print(f"✅ com0com 已安装，已有 {len(pairs)} 对虚拟串口:")
        for cn_a, name_a, cn_b, name_b in pairs:
            print(f"   {name_a} <-> {name_b}")
    else:
        print("⚠️  com0com 已安装，但还没有虚拟串口对")

    return setupc


# ════════════════════════════════════════
# 2. 自动绑定（创建虚拟串口对）
# ════════════════════════════════════════

def pair_exists(setupc, app_port, bridge_port):
    """检查指定端口对是否已存在。"""
    # 优先用 pyserial 检测（不需要管理员权限）
    found = set()
    for p in list_ports.comports():
        hwid_upper = p.hwid.upper()
        if "COM0COM" in hwid_upper or "COM0COM" in p.description.upper():
            if p.device in (app_port, bridge_port):
                found.add(p.device)
    if app_port in found and bridge_port in found:
        return True
    # 回退到 setupc list
    pairs = list_pairs(setupc)
    for _, a, _, b in pairs:
        if (a == app_port and b == bridge_port) or (a == bridge_port and b == app_port):
            return True
    return False


def create_pair(setupc, app_port, bridge_port):
    """用 UAC 提权运行 setupc 创建端口对。"""
    params = f"install PortName={app_port} PortName={bridge_port}"
    try:
        import ctypes
        rc = ctypes.windll.shell32.ShellExecuteW(None, "runas", setupc, params, None, 0)
        if rc > 32:
            print(f"⏳ 正在创建 {app_port} <-> {bridge_port}（请允许 UAC 弹窗）...")
            # 等待端口出现
            for _ in range(10):
                time.sleep(1)
                if pair_exists(setupc, app_port, bridge_port):
                    print(f"✅ 端口对创建成功: {app_port} <-> {bridge_port}")
                    return True
            print("⚠️  请确认 UAC 弹窗已通过，端口对可能在下次运行时出现")
            return True
    except Exception as e:
        print(f"❌ UAC 提权失败: {e}")
        print(f'   手动运行（管理员 CMD）: "{setupc}" {params}')
    return False


def auto_bind(setupc, app_port, bridge_port):
    """自动绑定：已有则跳过，没有则创建。"""
    if pair_exists(setupc, app_port, bridge_port):
        print(f"✅ 端口对已存在: {app_port} <-> {bridge_port}")
        return True
    return create_pair(setupc, app_port, bridge_port)


# ════════════════════════════════════════
# 3. 自动连接（桥接 COM <-> TCP）
# ════════════════════════════════════════

def connect_tcp(host, port):
    """连接 ESP32 TCP，自动重试。"""
    while True:
        try:
            s = socket.create_connection((host, port), timeout=5)
            s.settimeout(None)
            print(f"✅ 已连接 {host}:{port}")
            return s
        except OSError as e:
            print(f"⏳ 连接 {host}:{port} 失败: {e}，2s 后重试...")
            time.sleep(2)


def open_com(port, baud):
    """打开 COM 端口，自动重试。"""
    while True:
        try:
            ser = serial.Serial(port, baudrate=baud, timeout=0.2, write_timeout=0.2)
            print(f"✅ 已打开 {port}")
            return ser
        except serial.SerialException as e:
            print(f"⏳ 打开 {port} 失败: {e}，2s 后重试...")
            time.sleep(2)


def pump_tcp_to_com(sock, ser, stop):
    """TCP → COM"""
    try:
        while not stop.is_set():
            data = sock.recv(4096)
            if not data:
                log.debug("TCP 连接关闭 (recv 返回空)")
                break
            ser.write(data)
            ser.flush()
    except (OSError, serial.SerialException) as e:
        log.debug(f"TCP→COM 异常退出: {e}")


def pump_com_to_tcp(ser, sock, stop):
    """COM → TCP"""
    try:
        while not stop.is_set():
            data = ser.read(4096)
            if data:
                sock.sendall(data)
    except (OSError, serial.SerialException) as e:
        log.debug(f"COM→TCP 异常退出: {e}")


def run_bridge(com_port, host, tcp_port, baud):
    """运行桥接，断线自动重连。"""
    ser = open_com(com_port, baud)
    try:
        while True:
            sock = connect_tcp(host, tcp_port)
            stop = threading.Event()
            t1 = threading.Thread(target=pump_tcp_to_com, args=(sock, ser, stop), daemon=True)
            t2 = threading.Thread(target=pump_com_to_tcp, args=(ser, sock, stop), daemon=True)
            t1.start(); t2.start()
            while t1.is_alive() and t2.is_alive():
                time.sleep(0.2)
            stop.set()
            print(f"🔄 桥接线程退出 (t1={t1.is_alive()}, t2={t2.is_alive()})，2s 后重连...")
            try: sock.shutdown(socket.SHUT_RDWR)
            except OSError: pass
            try: sock.close()
            except OSError: pass
            t1.join(timeout=1); t2.join(timeout=1)
            print("🔄 连接断开，2s 后自动重连...")
            time.sleep(2)
    except KeyboardInterrupt:
        print("\n已停止")
    finally:
        try: ser.close()
        except Exception: pass


# ════════════════════════════════════════
# 主流程
# ════════════════════════════════════════

def show_status(setupc):
    """显示当前状态。"""
    print("═══ com0com 状态 ═══")
    pairs = list_pairs(setupc)
    if pairs:
        for cn_a, a, cn_b, b in pairs:
            print(f"  {a} <-> {b}")
    else:
        print("  无端口对")
    print(f"\n═══ 系统 COM 端口 ═══")
    for p in sorted(x.device for x in list_ports.comports()):
        print(f"  {p}")


def main():
    import argparse
    p = argparse.ArgumentParser(description="全自动虚拟串口桥接")
    p.add_argument("--host", default=DEFAULT_HOST, help=f"ESP IP (默认 {DEFAULT_HOST})")
    p.add_argument("--app-com", default=None, help="终端打开的端口 (默认自动检测 com0com)")
    p.add_argument("--bridge-com", default=None, help="桥接内部端口 (默认自动检测 com0com)")
    p.add_argument("--baud", type=int, default=BAUD, help=f"波特率 (默认 {BAUD})")
    p.add_argument("--status", action="store_true", help="只查看状态不运行")
    p.add_argument("-v", "--verbose", action="store_true", help="详细日志")
    args = p.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(message)s", datefmt="%H:%M:%S")

    # Step 1: 检测 com0com
    print("══════════════════════════════════")
    print("  无线串口调试器 — 虚拟串口桥接")
    print("══════════════════════════════════\n")
    print("【1/3】检测 com0com...")
    setupc = check_com0com()
    if not setupc:
        return

    if args.status:
        show_status(setupc)
        return

    # Step 2: 自动绑定 — 确定端口
    if args.app_com and args.bridge_com:
        app_port, bridge_port = args.app_com, args.bridge_com
    else:
        detected = detect_com0com_ports()
        if detected:
            app_port = args.app_com or detected[0]
            bridge_port = args.bridge_com or detected[1]
            print(f"✅ 自动检测到 com0com 端口: {app_port} <-> {bridge_port}")
        else:
            print("❌ 未检测到 com0com 虚拟串口，请用 --app-com / --bridge-com 指定")
            return

    print(f"\n【2/3】自动绑定虚拟串口对...")
    if not auto_bind(setupc, app_port, bridge_port):
        print("❌ 无法创建端口对，请手动运行 --status 查看")
        return

    # Step 3: 自动连接
    print(f"\n【3/3】连接 ESP32 {args.host}:{TCP_PORT}...")
    print(f"\n{'='*50}")
    print(f"  ✅ 桥接就绪")
    print(f"  打开串口工具 → 选择 {app_port} → {args.baud} 8N1")
    print(f"  桥接运行中，等待数据...")
    print(f"  Ctrl+C 退出")
    print(f"{'='*50}\n")

    run_bridge(bridge_port, args.host, TCP_PORT, args.baud)


if __name__ == "__main__":
    main()
