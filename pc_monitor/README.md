# Clawd Mochi PC Monitor

PC端系统监控服务，为Clawd Mochi设备提供CPU负载、内存使用、温度等数据。

## 快速开始

### 1. 安装依赖

```bash
pip install -r requirements.txt
```

### 2. 启动服务

**Windows:**
```cmd
双击 start.bat
```

**Linux/macOS:**
```bash
chmod +x start.sh
./start.sh
```

或直接运行：
```bash
python pc_monitor.py
```

启动后会显示系统托盘图标，服务运行在 `http://你的IP:8080`

### 3. 配网设置

在Clawd Mochi设备的配网页面或设置页面中，将PC IP设置为运行此脚本的电脑IP地址。

## PyInstaller打包

将脚本打包为独立可执行文件：

### Windows打包为.exe

```cmd
pip install pyinstaller
pyinstaller --onefile --windowed --icon=icon/favicon.ico --name "ClawdMochiMonitor" pc_monitor.py
```

打包完成后，可执行文件位于 `dist/ClawdMochiMonitor.exe`

**注意**: 打包后开机自启功能会自动使用exe路径，无需手动设置。

### Linux/macOS打包

```bash
pyinstaller --onefile --windowed --icon=icon/android-chrome-512x512.png --name "ClawdMochiMonitor" pc_monitor.py
```

## 开机自启

脚本支持跨平台开机自启功能（用户级别，无需管理员权限）：

### Windows
- 通过注册表 `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run` 实现
- 打包为exe后点击托盘菜单"开机自启"即可启用

### macOS
- 通过 `~/Library/LaunchAgents/com.clawd-mochi.monitor.plist` 实现
- 点击托盘菜单切换状态

### Linux
- 通过 `~/.config/autostart/clawd-mochi-monitor.desktop` 实现
- 点击托盘菜单切换状态

## Windows温度字段说明

**重要**: Windows系统下CPU温度获取较为困难，受以下限制：

1. 大多数Windows系统不提供标准的温度读取接口
2. WMI温度查询需要特定硬件支持且可能需要管理员权限
3. 温度字段在Windows下可能返回 `null`

**解决方案**:
- 如需准确温度，建议使用第三方工具如HWMonitor、CoreTemp等
- 这些工具提供API或共享内存接口，可自行扩展脚本

## 防火墙注意事项

### Windows防火墙

首次运行时，Windows防火墙可能弹出提示，请选择"允许访问"。

或手动添加规则：
```cmd
netsh advfirewall firewall add rule name="Clawd Mochi Monitor" dir=in action=allow protocol=tcp localport=8080
```

### Linux防火墙

如使用ufw：
```bash
sudo ufw allow 8080/tcp
```

如使用firewalld：
```bash
sudo firewall-cmd --add-port=8080/tcp --permanent
sudo firewall-cmd --reload
```

## 配置文件说明

`config.json` 配置项：

| 字段 | 说明 | 默认值 |
|------|------|--------|
| `port` | HTTP服务监听端口 | 8080 |
| `refresh_interval` | 数据刷新间隔（秒，暂未使用） | 2 |

修改端口后需重启服务生效。

## API接口

### `/stats.json`

返回系统监控数据：

```json
{
  "load": "25.5",       // CPU使用率（%）
  "mem": 45,            // 内存使用率（%）
  "temp": "42°C",       // CPU温度（Windows下可能为null）
  "uptime": "3d 5h",    // 运行时间
  "hour": 14,           // 当前小时
  "minute": 30,         // 当前分钟
  "day": 27             // 当前日期
}
```

### `/clawd/status`

把电脑或 Codex 状态推送到 Clawd Mochi：

```text
http://你的电脑IP:8080/clawd/status?status=working
```

支持状态：

| 状态 | 屏幕表情 |
| ---- | -------- |
| `idle` | 待机 |
| `working` / `thinking` | 思考中 |
| `done` / `success` | 完成 |
| `error` / `failed` | 报错 |
| `sleep` | 睡眠 |

也可以直接推送表情：

```text
http://你的电脑IP:8080/clawd/expression?name=happy
```

### 状态文件自动推送

脚本会监听 `config.json` 里的 `status_file`，默认是：

```text
pc_monitor/clawd_status.txt
```

把文件内容改成 `working`、`done`、`error`、`thinking` 等状态，脚本会自动请求 ESP32 的 `/api/status` 接口切换表情。

`config.json` 里需要把 `clawd_base_url` 改成 Clawd Mochi 在局域网里的地址，例如：

```json
{
  "clawd_base_url": "http://192.168.1.23"
}
```

## 系统托盘菜单

- 状态：运行中 ✅
- IP：显示当前监听地址
- 开机自启：点击切换启用/禁用
- 退出：关闭服务

## 目录结构

```
pc_monitor/
├── pc_monitor.py        # 主脚本
├── requirements.txt     # Python依赖
├── config.json          # 配置文件
├── start.bat            # Windows启动脚本
├── start.sh             # Linux/macOS启动脚本
├── icon/
│   ├── favicon.ico              # Windows托盘图标
│   └── android-chrome-512x512.png # Linux/macOS托盘图标
└── README.md            # 本说明文档
```

## 常见问题

**Q: 启动后无法访问API？**
A: 检查防火墙是否放行端口，确认IP地址正确。

**Q: 托盘图标不显示？**
A: 确保已安装 `pystray` 和 `pillow` 依赖。

**Q: 温度显示null？**
A: Windows系统温度获取受限，属于正常现象。

## License

MIT License
