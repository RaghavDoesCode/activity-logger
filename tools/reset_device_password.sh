#!/usr/bin/env bash
# Issue a NEW password for a device that is already enrolled (the old one was
# lost or leaked). Safe to repeat: it changes nothing else.
#   SERVER_ADDR=192.168.56.101 ./tools/reset_device_password.sh asha-pc > ~/asha.out.new && mv ~/asha.out.new ~/asha.out
# (Write to a temporary file and rename it, so a failure can never erase a good file.)
set -euo pipefail

DEVICE="${1:?device name, e.g. asha-pc}"
SERVER_ADDR="${SERVER_ADDR:?set SERVER_ADDR, e.g. SERVER_ADDR=192.168.56.101}"
MYSQL_CMD="${MYSQL_CMD:-sudo mysql}"

re_dev='^[A-Za-z0-9_.-]{1,100}$'
[[ $DEVICE =~ $re_dev ]] || { echo "bad device name" >&2; exit 1; }

ROW=$($MYSQL_CMD -N -B activity_monitor -e \
  "SELECT d.db_user, u.host FROM devices d JOIN mysql.user u ON u.user = d.db_user WHERE d.device_name = '$DEVICE'")
if [ -z "$ROW" ] || [ "$(wc -l <<<"$ROW")" -ne 1 ]; then
    echo "device '$DEVICE' not found, or the name is not unique" >&2
    exit 1
fi
DBUSER=$(cut -f1 <<<"$ROW")
DBHOST=$(cut -f2 <<<"$ROW")
DBPASS="$(openssl rand -base64 24 | tr -d '/+=' | cut -c1-24)"

$MYSQL_CMD -e "ALTER USER '$DBUSER'@'$DBHOST' IDENTIFIED BY '$DBPASS'"

cat <<CONF
=== New password issued for device: $DEVICE ===
DB_HOST=$SERVER_ADDR
DB_PORT=3306
DB_NAME=activity_monitor
DB_USER=$DBUSER
DB_PASS=$DBPASS
CONF
