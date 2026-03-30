#!/bin/bash
# ClaWD Status Installer
# Sets up the daemon, hook script, and Claude Code hook configuration.
#
# Usage: ./install.sh [--uninstall]

set -e

CLAWD_DIR="$HOME/.clawd"
CLAUDE_SETTINGS="$HOME/.claude/settings.json"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

GREEN='\033[0;32m'
ORANGE='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m'

info()  { echo -e "${GREEN}[clawd]${NC} $1"; }
warn()  { echo -e "${ORANGE}[clawd]${NC} $1"; }
error() { echo -e "${RED}[clawd]${NC} $1"; }

# ── Uninstall ──────────────────────────────────────────────────
if [ "$1" = "--uninstall" ]; then
  info "Uninstalling ClaWD Status..."

  # Stop daemon
  if [ -f "$CLAWD_DIR/clawd.pid" ]; then
    PID=$(cat "$CLAWD_DIR/clawd.pid" 2>/dev/null)
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
      kill "$PID" 2>/dev/null
      info "Stopped daemon (PID $PID)"
    fi
  fi

  # Unload launchd
  PLIST="$HOME/Library/LaunchAgents/com.clawd.status-daemon.plist"
  if [ -f "$PLIST" ]; then
    launchctl unload "$PLIST" 2>/dev/null || true
    rm -f "$PLIST"
    info "Removed launchd service"
  fi

  # Remove hooks from Claude settings
  if [ -f "$CLAUDE_SETTINGS" ] && command -v python3 &>/dev/null; then
    python3 -c "
import json, sys

path = '$CLAUDE_SETTINGS'
try:
    with open(path) as f:
        cfg = json.load(f)
except:
    sys.exit(0)

hooks = cfg.get('hooks', {})
changed = False
for event in ['PreToolUse', 'UserPromptSubmit', 'Stop', 'Notification']:
    if event in hooks:
        hooks[event] = [h for h in hooks[event]
                        if not any('clawd_hook.sh' in hh.get('command', '')
                                   for hh in h.get('hooks', []))]
        if not hooks[event]:
            del hooks[event]
        changed = True

if changed:
    cfg['hooks'] = hooks
    with open(path, 'w') as f:
        json.dump(cfg, f, indent=2)
    print('Removed hooks from Claude settings')
"
  fi

  info "Uninstall complete. Run 'rm -rf ~/.clawd' to remove all data."
  exit 0
fi

# ── Install ────────────────────────────────────────────────────
info "Installing ClaWD Status..."

# 1. Create directory
mkdir -p "$CLAWD_DIR"
info "Created $CLAWD_DIR"

# 2. Copy files
cp "$REPO_DIR/daemon/clawd_daemon.py" "$CLAWD_DIR/clawd_daemon.py"
cp "$SCRIPT_DIR/clawd_hook.sh" "$CLAWD_DIR/clawd_hook.sh"
chmod +x "$CLAWD_DIR/clawd_hook.sh"
info "Copied daemon and hook script"

# 3. Install Python dependency
if ! python3 -c "import serial" 2>/dev/null; then
  info "Installing pyserial..."
  pip3 install pyserial --quiet 2>/dev/null || {
    warn "Could not install pyserial automatically."
    warn "Please run: pip3 install pyserial"
  }
fi

# 4. Create default config if not exists
if [ ! -f "$CLAWD_DIR/config.json" ]; then
  cat > "$CLAWD_DIR/config.json" << 'EOF'
{
  "serial_port": "auto",
  "baud_rate": 115200,
  "idle_timeout": 300,
  "brightness": 255,
  "speed": 2
}
EOF
  info "Created default config at $CLAWD_DIR/config.json"
fi

# 5. Install Claude Code hooks
mkdir -p "$(dirname "$CLAUDE_SETTINGS")"

if command -v python3 &>/dev/null; then
  python3 -c "
import json, os, sys

path = '$CLAUDE_SETTINGS'
hook_cmd_working   = '~/.clawd/clawd_hook.sh working'
hook_cmd_attention = '~/.clawd/clawd_hook.sh attention'

# Load existing settings or start fresh
try:
    with open(path) as f:
        cfg = json.load(f)
except (FileNotFoundError, json.JSONDecodeError):
    cfg = {}

hooks = cfg.setdefault('hooks', {})

def has_clawd_hook(event_hooks):
    for entry in event_hooks:
        for h in entry.get('hooks', []):
            if 'clawd_hook.sh' in h.get('command', ''):
                return True
    return False

def add_hook(event, command, matcher=None):
    entries = hooks.setdefault(event, [])
    if has_clawd_hook(entries):
        return  # already installed
    entry = {'hooks': [{'type': 'command', 'command': command}]}
    if matcher:
        entry['matcher'] = matcher
    entries.append(entry)

add_hook('PreToolUse', hook_cmd_working)
add_hook('UserPromptSubmit', hook_cmd_working)
add_hook('Stop', hook_cmd_attention)
add_hook('Notification', hook_cmd_attention, matcher='idle_prompt')

cfg['hooks'] = hooks
with open(path, 'w') as f:
    json.dump(cfg, f, indent=2)
print('Hooks installed in ' + path)
"
  info "Claude Code hooks configured"
else
  error "python3 not found — cannot install hooks automatically."
  warn "Add the following to $CLAUDE_SETTINGS manually:"
  echo '  See hooks/README for the required configuration.'
fi

# 6. Install launchd service (macOS only)
if [ "$(uname)" = "Darwin" ]; then
  PLIST_SRC="$REPO_DIR/daemon/com.clawd.status-daemon.plist"
  PLIST_DST="$HOME/Library/LaunchAgents/com.clawd.status-daemon.plist"
  mkdir -p "$HOME/Library/LaunchAgents"

  # Expand ~ in plist to actual home directory
  sed "s|~|$HOME|g" "$PLIST_SRC" > "$PLIST_DST"

  # Load the service
  launchctl unload "$PLIST_DST" 2>/dev/null || true
  launchctl load "$PLIST_DST"
  info "Daemon installed and started via launchd"
else
  info "Not macOS — skipping launchd setup."
  info "Start the daemon manually: python3 $CLAWD_DIR/clawd_daemon.py"
fi

echo ""
info "Installation complete!"
info ""
info "  Config:  $CLAWD_DIR/config.json"
info "  Daemon:  $CLAWD_DIR/clawd_daemon.py"
info "  Hooks:   $CLAUDE_SETTINGS"
info "  Logs:    $CLAWD_DIR/clawd_daemon.log"
info ""
info "Flash the ESP32 firmware from firmware/clawd_status/ using Arduino IDE."
info "The daemon will auto-detect the ESP32 when plugged in via USB."
