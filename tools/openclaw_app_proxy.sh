#!/bin/zsh

set -euo pipefail

REMOTE_HOST="${OPENCLAW_SSH_HOST:-mrkmkr_openclaw@192.168.68.79}"
REMOTE_APP_HOST="${OPENCLAW_APP_HOST:-127.0.0.1}"
DEFAULT_REMOTE_APP_PORT="${OPENCLAW_APP_PORT:-3000}"
DEFAULT_HTTPS_PORT="${OPENCLAW_HTTPS_PORT:-8443}"

usage() {
  cat <<'EOF'
Usage:
  openclaw_app_proxy.sh start [remote_app_port] [https_port]
  openclaw_app_proxy.sh stop [https_port]
  openclaw_app_proxy.sh status
  openclaw_app_proxy.sh check

Purpose:
  Expose a temporary app on the OpenClaw host through Tailscale Serve without
  changing the main OpenClaw gateway mapping on port 443.

Defaults:
  remote_app_port: 3000
  https_port: 8443

Environment overrides:
  OPENCLAW_SSH_HOST   SSH target, default mrkmkr_openclaw@192.168.68.79
  OPENCLAW_APP_HOST   Remote bind host for the app, default 127.0.0.1
  OPENCLAW_APP_PORT   Default app port, default 3000
  OPENCLAW_HTTPS_PORT Default exposed HTTPS port, default 8443

Examples:
  ./tools/openclaw_app_proxy.sh start
  ./tools/openclaw_app_proxy.sh start 4173 8443
  ./tools/openclaw_app_proxy.sh stop
  ./tools/openclaw_app_proxy.sh status
  ./tools/openclaw_app_proxy.sh check
EOF
}

remote_exec() {
  ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new "$REMOTE_HOST" "$@"
}

cmd_start() {
  local remote_app_port="${1:-$DEFAULT_REMOTE_APP_PORT}"
  local https_port="${2:-$DEFAULT_HTTPS_PORT}"
  local target="${REMOTE_APP_HOST}:${remote_app_port}"

  remote_exec "set -e; tailscale serve --yes --bg --https=${https_port} ${target}; tailscale serve status"
}

cmd_stop() {
  local https_port="${1:-$DEFAULT_HTTPS_PORT}"

  remote_exec "set -e; tailscale serve --https=${https_port} off; tailscale serve status"
}

cmd_status() {
  remote_exec "tailscale serve status"
}

cmd_check() {
  remote_exec "set -e; echo '--- openclaw ---'; ss -ltnp | grep 18789 || true; echo '--- tailscale ---'; tailscale serve status; echo '--- app ports ---'; ss -ltnp | grep -E ':3000|:4173|:8000|:8080|:8443' || true"
}

main() {
  local command="${1:-}"
  shift || true

  case "$command" in
    start)
      cmd_start "$@"
      ;;
    stop)
      cmd_stop "$@"
      ;;
    status)
      cmd_status
      ;;
    check)
      cmd_check
      ;;
    ""|-h|--help|help)
      usage
      ;;
    *)
      echo "Unknown command: $command" >&2
      usage >&2
      exit 1
      ;;
  esac
}

main "$@"
