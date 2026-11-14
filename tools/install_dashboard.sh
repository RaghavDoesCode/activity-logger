#!/bin/bash
# Installs the admin dashboard as a systemd service that starts at boot and
# restarts if it crashes. Run as your normal user (it uses sudo where needed):
#   ./tools/install_dashboard.sh
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$(pwd)
RUN_AS=$(id -un)

[ -f "$HOME/dashboard.conf" ] || { echo "missing ~/dashboard.conf - run tools/setup_admin_db.sh first" >&2; exit 1; }
[ -f "$REPO/dashboard/app.py" ] || { echo "dashboard/app.py not found" >&2; exit 1; }

sudo tee /etc/systemd/system/activity-dashboard.service >/dev/null <<UNIT
[Unit]
Description=Activity Logger admin dashboard
After=network-online.target mysql.service
Wants=network-online.target mysql.service

[Service]
User=$RUN_AS
WorkingDirectory=$REPO
Environment=PYTHONUNBUFFERED=1
ExecStart=/usr/bin/python3 $REPO/dashboard/app.py
Restart=on-failure
RestartSec=5
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full

[Install]
WantedBy=multi-user.target
UNIT

sudo systemctl daemon-reload
sudo systemctl enable --now activity-dashboard
sleep 2
systemctl is-active activity-dashboard
