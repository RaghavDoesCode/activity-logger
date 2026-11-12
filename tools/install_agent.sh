#!/usr/bin/env bash
# Install the agent on THIS machine: kernel module, logger binary, systemd unit.
# Needs /etc/activity-logger/agent.conf and ca.pem to exist already.
#   ./tools/install_agent.sh
set -euo pipefail
cd "$(dirname "$0")/.."

[ -f kernel/proc_watch.ko ] || { echo "build the module first: (cd kernel && make)"; exit 1; }
[ -x logger/logger ]        || { echo "build the logger first (see README)"; exit 1; }
[ -f /etc/activity-logger/agent.conf ] || { echo "missing /etc/activity-logger/agent.conf"; exit 1; }
[ -f /etc/activity-logger/ca.pem ]     || { echo "missing /etc/activity-logger/ca.pem"; exit 1; }

# Module goes where modprobe looks, so it can load by name (and at boot)
sudo install -D -m 644 kernel/proc_watch.ko "/lib/modules/$(uname -r)/extra/proc_watch.ko"
sudo depmod -a

sudo install -m 755 logger/logger /usr/local/sbin/activity-logger
sudo install -m 644 deploy/activity-logger.service /etc/systemd/system/activity-logger.service

sudo systemctl daemon-reload
sudo systemctl enable --now activity-logger
sleep 2
systemctl --no-pager --lines=8 status activity-logger || true
