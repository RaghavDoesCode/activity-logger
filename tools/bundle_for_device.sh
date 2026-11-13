#!/usr/bin/env bash
# Prints a block of commands to paste into a NEW device's terminal. It writes
# that device's config and the server's CA certificate, so nothing is typed by hand.
#   ./tools/bundle_for_device.sh ~/asha.out
set -euo pipefail
OUT="${1:?the file saved from add_device.sh, e.g. ~/asha.out}"
CA="${CA_FILE:-/etc/activity-logger/ca.pem}"
[ -r "$OUT" ] || { echo "cannot read $OUT"; exit 1; }
[ -r "$CA" ]  || { echo "cannot read $CA"; exit 1; }
for k in DB_HOST DB_PORT DB_NAME DB_USER DB_PASS; do
    grep -q "^$k=" "$OUT" || { echo "missing $k in $OUT"; exit 1; }
done

echo "# ---- paste everything between the markers into the DEVICE's terminal ----"
echo "# ---- (run 'sudo true' there first so sudo does not ask for a password mid-paste)"
echo "sudo install -d -m 755 /etc/activity-logger"
echo "sudo tee /etc/activity-logger/ca.pem >/dev/null <<'CA_EOF'"
tr -d '\0' < "$CA"
echo "CA_EOF"
echo "sudo tee /etc/activity-logger/agent.conf >/dev/null <<'CONF_EOF'"
grep '^DB_' "$OUT"
echo "IDLE_SECONDS=10"
echo "CONF_EOF"
echo "sudo chmod 600 /etc/activity-logger/agent.conf; sudo chmod 644 /etc/activity-logger/ca.pem"
echo "sudo ls -l /etc/activity-logger"
echo "# ---- end of block ----"
