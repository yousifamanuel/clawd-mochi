#!/usr/bin/env python3
"""
Clawd Mochi PC Monitor
提供系统监控数据HTTP API，配合系统托盘运行
"""

import os
import sys
import json
import time
import socket
import threading
import platform
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime

import psutil
from flask import Flask, jsonify, request
from flask_cors import CORS

# 系统托盘相关
try:
    import pystray
    from PIL import Image
    TRAY_AVAILABLE = True
except ImportError:
    TRAY_AVAILABLE = False
    print("Warning: pystray or Pillow not installed, tray icon disabled")

# ── 配置 ──────────────────────────────────────────────────────

CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'config.json')
ICON_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'icon')

DEFAULT_CONFIG = {
    "port": 8080,
    "refresh_interval": 2
}

def load_config():
    """加载配置文件"""
    if os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, 'r', encoding='utf-8') as f:
                return json.load(f)
        except Exception as e:
            print(f"Config load error: {e}")
    return DEFAULT_CONFIG

config = load_config()
PORT = config.get('port', 8080)
CLAWD_BASE_URL = config.get('clawd_base_url', 'http://192.168.4.1').rstrip('/')
STATUS_FILE = config.get('status_file', 'clawd_status.txt')
if not os.path.isabs(STATUS_FILE):
    STATUS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), STATUS_FILE)
PUSH_INTERVAL = float(config.get('push_interval', 1))
last_pushed_status = None

# ── Flask应用 ──────────────────────────────────────────────────

app = Flask(__name__)
CORS(app)  # 允许跨域请求

def push_clawd_status(status):
    """Push a semantic status to the ESP32 controller."""
    status = (status or '').strip().lower()
    if not status:
        return False, 'empty status'
    url = f"{CLAWD_BASE_URL}/api/status?status={urllib.parse.quote(status)}"
    try:
      with urllib.request.urlopen(url, timeout=2) as resp:
          ok = 200 <= resp.status < 300
          return ok, f"HTTP {resp.status}"
    except urllib.error.URLError as e:
        return False, str(e)

def status_file_watcher():
    """Watch a small text file and push changes to Clawd Mochi."""
    global last_pushed_status
    while True:
        try:
            if os.path.exists(STATUS_FILE):
                with open(STATUS_FILE, 'r', encoding='utf-8') as f:
                    lines = f.read().strip().splitlines()
                    status = lines[0].strip() if lines else ''
                if status and status != last_pushed_status:
                    ok, msg = push_clawd_status(status)
                    if ok:
                        last_pushed_status = status
                        print(f"Clawd status pushed: {status}")
                    else:
                        print(f"Clawd push failed: {msg}")
        except Exception as e:
            print(f"Status watcher error: {e}")
        time.sleep(PUSH_INTERVAL)

def get_cpu_temp():
    """获取CPU温度（跨平台）"""
    system = platform.system()
    
    if system == 'Linux':
        # Linux: 读取thermal_zone文件
        try:
            thermal_paths = [
                '/sys/class/thermal/thermal_zone0/temp',
                '/sys/class/thermal/thermal_zone1/temp',
            ]
            for path in thermal_paths:
                if os.path.exists(path):
                    with open(path, 'r') as f:
                        temp = int(f.read().strip()) / 1000.0
                        return f"{int(temp)}°C"
        except Exception:
            pass
        return None
    
    elif system == 'Darwin':  # macOS
        # macOS: 尝试使用系统命令获取温度
        try:
            import subprocess
            result = subprocess.run(['sysctl', '-n', 'machdep.xcpm.cpu_thermal_level'], 
                                   capture_output=True, text=True, timeout=2)
            if result.returncode == 0:
                # macOS没有直接的温度值，返回状态描述
                return None
        except Exception:
            pass
        return None
    
    elif system == 'Windows':
        # Windows: 尝试使用WMI获取温度
        try:
            import subprocess
            # 使用wmic获取温度（需要管理员权限，大多数系统不支持）
            result = subprocess.run(
                ['wmic', '/namespace:\\\\root\\wmi', 'PATH', 'MSAcpi_ThermalZoneTemperature', 'get', 'CurrentTemperature'],
                capture_output=True, text=True, timeout=5, creationflags=subprocess.CREATE_NO_WINDOW
            )
            if result.returncode == 0 and 'CurrentTemperature' in result.stdout:
                lines = result.stdout.strip().split('\n')
                for line in lines:
                    if line.strip() and line.strip() != 'CurrentTemperature':
                        try:
                            # WMI温度单位是0.1K，需要转换
                            temp_k = float(line.strip())
                            temp_c = (temp_k - 2732) / 10.0
                            return f"{int(temp_c)}°C"
                        except ValueError:
                            continue
        except Exception:
            pass
        return None  # Windows下获取失败返回null
    
    return None

def get_uptime_str():
    """获取系统运行时间"""
    uptime_seconds = int(time.time() - psutil.boot_time())
    days = uptime_seconds // 86400
    hours = (uptime_seconds % 86400) // 3600
    minutes = (uptime_seconds % 3600) // 60
    
    if days > 0:
        return f"{days}d {hours}h"
    elif hours > 0:
        return f"{hours}h {minutes}m"
    else:
        return f"{minutes}m"

@app.route('/stats.json')
def get_stats():
    """返回系统监控数据"""
    now = datetime.now()
    
    stats = {
        "load": f"{psutil.cpu_percent(interval=0.1)}",
        "mem": psutil.virtual_memory().percent,
        "temp": get_cpu_temp(),  # Windows下可能为null
        "uptime": get_uptime_str(),
        "hour": now.hour,
        "minute": now.minute,
        "day": now.day
    }
    
    return jsonify(stats)

@app.route('/clawd/status', methods=['GET', 'POST'])
def clawd_status():
    """Push status to Clawd Mochi. Example: /clawd/status?status=working"""
    status = request.values.get('status', '').strip()
    if not status and request.is_json:
        body = request.get_json(silent=True) or {}
        status = str(body.get('status', '')).strip()
    ok, msg = push_clawd_status(status)
    return jsonify({"ok": ok, "status": status, "message": msg, "target": CLAWD_BASE_URL}), (200 if ok else 502)

@app.route('/clawd/expression', methods=['GET', 'POST'])
def clawd_expression():
    """Push expression to Clawd Mochi. Example: /clawd/expression?name=happy"""
    name = request.values.get('name') or request.values.get('expr') or ''
    name = name.strip()
    if not name and request.is_json:
        body = request.get_json(silent=True) or {}
        name = str(body.get('name') or body.get('expr') or '').strip()
    if not name:
        return jsonify({"ok": False, "error": "missing expression"}), 400
    url = f"{CLAWD_BASE_URL}/api/expression?name={urllib.parse.quote(name)}"
    try:
        with urllib.request.urlopen(url, timeout=2) as resp:
            return jsonify({"ok": 200 <= resp.status < 300, "expression": name, "target": CLAWD_BASE_URL})
    except urllib.error.URLError as e:
        return jsonify({"ok": False, "expression": name, "message": str(e), "target": CLAWD_BASE_URL}), 502

@app.route('/')
def index():
    """简单的状态页面"""
    return '''
    <html>
    <head><title>Clawd Mochi Monitor</title></head>
    <body style="background:#1c1c20;color:#e8e4dc;font-family:monospace;padding:20px">
    <h1 style="color:#c96a3e">Clawd Mochi PC Monitor</h1>
    <p>Status: Running</p>
    <p>API: <a href="/stats.json" style="color:#28b878">/stats.json</a></p>
    </body>
    </html>
    '''

# ── 开机自启功能 ────────────────────────────────────────────────

APP_NAME = "ClawdMochiMonitor"
SCRIPT_PATH = os.path.abspath(__file__)

def is_autostart_enabled():
    """检查开机自启是否启用"""
    system = platform.system()
    
    if system == 'Windows':
        try:
            import winreg
            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, 
                r"Software\Microsoft\Windows\CurrentVersion\Run", 0, winreg.KEY_READ)
            try:
                winreg.QueryValueEx(key, APP_NAME)
                winreg.CloseKey(key)
                return True
            except FileNotFoundError:
                winreg.CloseKey(key)
                return False
        except Exception:
            return False
    
    elif system == 'Darwin':  # macOS
        plist_path = os.path.expanduser("~/Library/LaunchAgents/com.clawd-mochi.monitor.plist")
        return os.path.exists(plist_path)
    
    elif system == 'Linux':
        desktop_path = os.path.expanduser("~/.config/autostart/clawd-mochi-monitor.desktop")
        return os.path.exists(desktop_path)
    
    return False

def get_executable_path():
    """获取可执行文件路径（支持打包exe和脚本运行）"""
    if getattr(sys, 'frozen', False):
        # PyInstaller打包后的exe
        return sys.executable
    else:
        # 脚本运行模式，返回pythonw + 脚本路径
        return f'pythonw "{SCRIPT_PATH}"'

def enable_autostart():
    """启用开机自启"""
    system = platform.system()
    
    if system == 'Windows':
        try:
            import winreg
            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                r"Software\Microsoft\Windows\CurrentVersion\Run", 0, winreg.KEY_WRITE)
            exe_path = get_executable_path()
            winreg.SetValueEx(key, APP_NAME, 0, winreg.REG_SZ, exe_path)
            winreg.CloseKey(key)
            print(f"Autostart enabled: {exe_path}")
            return True
        except Exception as e:
            print(f"Enable autostart error: {e}")
            return False
    
    elif system == 'Darwin':  # macOS
        try:
            import plistlib
            plist_path = os.path.expanduser("~/Library/LaunchAgents/com.clawd-mochi.monitor.plist")
            plist_data = {
                "Label": "com.clawd-mochi.monitor",
                "ProgramArguments": [sys.executable, SCRIPT_PATH],
                "RunAtLoad": True,
                "KeepAlive": False
            }
            os.makedirs(os.path.dirname(plist_path), exist_ok=True)
            with open(plist_path, 'wb') as f:
                plistlib.dump(plist_data, f)
            return True
        except Exception as e:
            print(f"Enable autostart error: {e}")
            return False
    
    elif system == 'Linux':
        try:
            desktop_path = os.path.expanduser("~/.config/autostart/clawd-mochi-monitor.desktop")
            desktop_content = f'''[Desktop Entry]
Type=Application
Name=Clawd Mochi Monitor
Exec={sys.executable} "{SCRIPT_PATH}"
Icon={os.path.join(ICON_DIR, "android-chrome-512x512.png")}
Terminal=false
Hidden=false
'''
            os.makedirs(os.path.dirname(desktop_path), exist_ok=True)
            with open(desktop_path, 'w') as f:
                f.write(desktop_content)
            return True
        except Exception as e:
            print(f"Enable autostart error: {e}")
            return False
    
    return False

def disable_autostart():
    """禁用开机自启"""
    system = platform.system()
    
    if system == 'Windows':
        try:
            import winreg
            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                r"Software\Microsoft\Windows\CurrentVersion\Run", 0, winreg.KEY_WRITE)
            try:
                winreg.DeleteValue(key, APP_NAME)
            except FileNotFoundError:
                pass
            winreg.CloseKey(key)
            return True
        except Exception as e:
            print(f"Disable autostart error: {e}")
            return False
    
    elif system == 'Darwin':  # macOS
        plist_path = os.path.expanduser("~/Library/LaunchAgents/com.clawd-mochi.monitor.plist")
        try:
            if os.path.exists(plist_path):
                os.remove(plist_path)
            return True
        except Exception as e:
            print(f"Disable autostart error: {e}")
            return False
    
    elif system == 'Linux':
        desktop_path = os.path.expanduser("~/.config/autostart/clawd-mochi-monitor.desktop")
        try:
            if os.path.exists(desktop_path):
                os.remove(desktop_path)
            return True
        except Exception as e:
            print(f"Disable autostart error: {e}")
            return False
    
    return False

def toggle_autostart(icon, item):
    """切换开机自启状态"""
    if is_autostart_enabled():
        disable_autostart()
    else:
        enable_autostart()
    # 更新菜单
    update_menu()

# ── 系统托盘 ────────────────────────────────────────────────────

tray_icon = None
menu_items = []

def get_local_ip():
    """获取本地IP地址"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"

def get_autostart_text():
    """获取开机自启菜单文本"""
    enabled = is_autostart_enabled()
    return f"开机自启  {'[✓ 已启用]' if enabled else '[未启用]'}"

def update_menu():
    """更新托盘菜单"""
    global tray_icon, menu_items
    if tray_icon is None:
        return
    
    # 重建菜单
    menu = pystray.Menu(
        pystray.MenuItem("状态：运行中 ✅", None, enabled=False),
        pystray.MenuItem(f"IP：{get_local_ip()}:{PORT}", None, enabled=False),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem(get_autostart_text(), toggle_autostart),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("退出", exit_app)
    )
    tray_icon.menu = menu
    tray_icon.update_menu()

def exit_app(icon, item):
    """退出应用"""
    global tray_icon
    if tray_icon:
        tray_icon.stop()
    os._exit(0)

def load_icon():
    """加载托盘图标"""
    system = platform.system()
    
    if system == 'Windows':
        icon_path = os.path.join(ICON_DIR, 'favicon.ico')
        if os.path.exists(icon_path):
            return Image.open(icon_path)
    else:
        icon_path = os.path.join(ICON_DIR, 'android-chrome-512x512.png')
        if os.path.exists(icon_path):
            img = Image.open(icon_path)
            # 缩放到合适大小
            return img.resize((64, 64), Image.Resampling.LANCZOS)
    
    # 默认图标
    return Image.new('RGB', (64, 64), color='#c96a3e')

def run_tray():
    """运行系统托盘"""
    global tray_icon
    
    if not TRAY_AVAILABLE:
        return
    
    icon_image = load_icon()
    
    menu = pystray.Menu(
        pystray.MenuItem("状态：运行中 ✅", None, enabled=False),
        pystray.MenuItem(f"IP：{get_local_ip()}:{PORT}", None, enabled=False),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem(get_autostart_text(), toggle_autostart),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("退出", exit_app)
    )
    
    tray_icon = pystray.Icon("clawd_mochi_monitor", icon_image, "Clawd Mochi Monitor", menu)
    tray_icon.run()

def run_flask():
    """运行Flask服务器"""
    app.run(host='0.0.0.0', port=PORT, threaded=True, use_reloader=False)

# ── 主程序 ──────────────────────────────────────────────────────

def main():
    """主函数"""
    print(f"Clawd Mochi PC Monitor starting...")
    print(f"Port: {PORT}")
    print(f"Platform: {platform.system()}")
    print(f"Autostart: {'Enabled' if is_autostart_enabled() else 'Disabled'}")
    
    # Flask线程
    flask_thread = threading.Thread(target=run_flask, daemon=True)
    flask_thread.start()
    watcher_thread = threading.Thread(target=status_file_watcher, daemon=True)
    watcher_thread.start()
    
    print(f"HTTP API running at http://{get_local_ip()}:{PORT}/stats.json")
    print(f"Clawd target: {CLAWD_BASE_URL}")
    print(f"Status file: {STATUS_FILE}")
    
    # 系统托盘（主线程）
    if TRAY_AVAILABLE:
        run_tray()
    else:
        # 无托盘时保持运行
        print("Running without tray icon. Press Ctrl+C to exit.")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print("Exiting...")

if __name__ == '__main__':
    main()
