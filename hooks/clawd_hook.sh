#!/bin/bash
# ClaWD Status Hook — sends state to the daemon via Unix socket.
# Usage: clawd_hook.sh working|attention
#
# Called by Claude Code hooks (PreToolUse, UserPromptSubmit, Stop, Notification).
# Fire-and-forget: exits silently if daemon is not running.

SOCKET="$HOME/.clawd/clawd.sock"

python3 -c "
import socket, sys, os
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect(os.path.expanduser('~/.clawd/clawd.sock'))
    s.sendall((sys.argv[1] + '\n').encode())
except Exception:
    pass
finally:
    s.close()
" "$1" 2>/dev/null
