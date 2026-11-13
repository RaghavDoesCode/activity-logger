#!/bin/bash
# Creates the read-only MySQL account for the admin dashboard; prints settings ONCE.
#   ./tools/setup_admin_db.sh > ~/dashboard.conf.new && mv ~/dashboard.conf.new ~/dashboard.conf
# Re-running rotates the password.
set -euo pipefail
cd "$(dirname "$0")/.."
DBN=activity_monitor
PASS=$(openssl rand -hex 12)
sudo mysql -e "
  CREATE USER IF NOT EXISTS 'activity_admin'@'localhost' IDENTIFIED BY '$PASS';
  ALTER USER 'activity_admin'@'localhost' IDENTIFIED BY '$PASS';
  REVOKE ALL PRIVILEGES, GRANT OPTION FROM 'activity_admin'@'localhost';
  GRANT SELECT ON $DBN.v_samples         TO 'activity_admin'@'localhost';
  GRANT SELECT ON $DBN.v_session_summary TO 'activity_admin'@'localhost';
  GRANT SELECT ON $DBN.v_device_status   TO 'activity_admin'@'localhost';
  GRANT SELECT ON $DBN.v_daily_activity  TO 'activity_admin'@'localhost';
  GRANT SELECT ON $DBN.v_hourly_activity TO 'activity_admin'@'localhost';
  GRANT SELECT ON $DBN.v_top_apps        TO 'activity_admin'@'localhost';
  GRANT SELECT (admin_id, username, pass_hash, is_active) ON $DBN.admin_users TO 'activity_admin'@'localhost';
" >&2
echo "DB_HOST=localhost"
echo "DB_NAME=$DBN"
echo "DB_USER=activity_admin"
echo "DB_PASS=$PASS"
