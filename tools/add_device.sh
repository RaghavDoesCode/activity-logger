#!/usr/bin/env bash
# Enrol one device for one employee.
#   ./tools/add_device.sh EMP002 "Asha Rawat" asha-laptop [Department]
# Optional env: ALLOW_HOST (MySQL host pattern the device may connect from,
# default 'localhost'), SERVER_ADDR (shown in the printed config).
set -euo pipefail

CODE="${1:?employee code, e.g. EMP002}"
NAME="${2:?full name, e.g. \"Asha Rawat\"}"
DEVICE="${3:?device name, e.g. asha-laptop}"
DEPT="${4:-}"
ALLOW_HOST="${ALLOW_HOST:-localhost}"
SERVER_ADDR="${SERVER_ADDR:-$(hostname -I | awk '{print $1}')}"
MYSQL_CMD="${MYSQL_CMD:-sudo mysql}"

# Inputs go into SQL text, so accept only safe characters.
re_code='^[A-Za-z0-9_-]{1,50}$'
re_name='^[A-Za-z0-9 .-]{1,100}$'
re_dev='^[A-Za-z0-9_.-]{1,100}$'
re_dept='^[A-Za-z0-9 &-]{0,100}$'
re_host='^[A-Za-z0-9_.%:-]+$'
[[ $CODE       =~ $re_code ]] || { echo "bad employee code"; exit 1; }
[[ $NAME       =~ $re_name ]] || { echo "bad name"; exit 1; }
[[ $DEVICE     =~ $re_dev  ]] || { echo "bad device name"; exit 1; }
[[ $DEPT       =~ $re_dept ]] || { echo "bad department"; exit 1; }
[[ $ALLOW_HOST =~ $re_host ]] || { echo "bad ALLOW_HOST"; exit 1; }

DBUSER="dev_$(openssl rand -hex 4)"                      # 12 chars, random
DBPASS="$(openssl rand -base64 24 | tr -d '/+=' | cut -c1-24)"

$MYSQL_CMD <<SQL
USE activity_monitor;

INSERT INTO employees (employee_code, full_name, department)
VALUES ('$CODE', '$NAME', NULLIF('$DEPT', ''))
ON DUPLICATE KEY UPDATE employee_id = LAST_INSERT_ID(employee_id);

INSERT INTO devices (employee_id, device_name, db_user)
VALUES (LAST_INSERT_ID(), '$DEVICE', '$DBUSER');

CREATE USER '$DBUSER'@'$ALLOW_HOST' IDENTIFIED BY '$DBPASS';
GRANT EXECUTE ON PROCEDURE activity_monitor.start_session TO '$DBUSER'@'$ALLOW_HOST';
GRANT EXECUTE ON PROCEDURE activity_monitor.end_session   TO '$DBUSER'@'$ALLOW_HOST';
GRANT INSERT ON activity_monitor.process_events    TO '$DBUSER'@'$ALLOW_HOST';
GRANT INSERT ON activity_monitor.process_snapshots TO '$DBUSER'@'$ALLOW_HOST';
ALTER USER '$DBUSER'@'$ALLOW_HOST' REQUIRE SSL;
SQL

cat <<CONF

=== Device enrolled: $DEVICE ($CODE, $NAME) ===
Put this in /etc/activity-logger/agent.conf on that device (shown only once):

DB_HOST=$SERVER_ADDR
DB_PORT=3306
DB_NAME=activity_monitor
DB_USER=$DBUSER
DB_PASS=$DBPASS
CONF
