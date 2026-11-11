-- Step 1a: access control. Run once, as root: sudo mysql < file
USE activity_monitor;

DROP PROCEDURE IF EXISTS start_session;
DROP PROCEDURE IF EXISTS end_session;
DROP TRIGGER IF EXISTS trg_events_owner;
DROP TRIGGER IF EXISTS trg_snapshots_owner;

DELIMITER //

-- A device opens a session. WHO it is comes from the MySQL account it logged
-- in with (USER()), never from anything the client sends.
CREATE DEFINER = CURRENT_USER PROCEDURE start_session(IN p_machine VARCHAR(100))
SQL SECURITY DEFINER
BEGIN
    DECLARE v_dev INT;
    DECLARE v_emp INT;

    SELECT d.device_id, d.employee_id INTO v_dev, v_emp
    FROM devices d
    JOIN employees e ON e.employee_id = d.employee_id
    WHERE d.db_user = SUBSTRING_INDEX(USER(), '@', 1)
      AND d.is_active AND e.is_active;

    IF v_dev IS NULL THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'device not registered or disabled';
    END IF;

    INSERT INTO sessions (employee_id, device_id, machine_name)
    VALUES (v_emp, v_dev, p_machine);

    SELECT LAST_INSERT_ID() AS session_id;
END//

-- A device closes ITS OWN open session only.
CREATE DEFINER = CURRENT_USER PROCEDURE end_session(IN p_session_id INT)
SQL SECURITY DEFINER
BEGIN
    UPDATE sessions s
    JOIN devices d ON d.device_id = s.device_id
    SET s.end_time = NOW()
    WHERE s.session_id = p_session_id
      AND s.end_time IS NULL
      AND d.db_user = SUBSTRING_INDEX(USER(), '@', 1);
END//

-- Rows may only be added to an open session that belongs to the calling device
CREATE DEFINER = CURRENT_USER
TRIGGER trg_events_owner BEFORE INSERT ON process_events
FOR EACH ROW
BEGIN
    IF NOT EXISTS (SELECT 1 FROM sessions s
                   JOIN devices d ON d.device_id = s.device_id
                   WHERE s.session_id = NEW.session_id
                     AND s.end_time IS NULL
                     AND d.is_active
                     AND d.db_user = SUBSTRING_INDEX(USER(), '@', 1)) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'session does not belong to this device';
    END IF;
END//

CREATE DEFINER = CURRENT_USER
TRIGGER trg_snapshots_owner BEFORE INSERT ON process_snapshots
FOR EACH ROW
BEGIN
    IF NOT EXISTS (SELECT 1 FROM sessions s
                   JOIN devices d ON d.device_id = s.device_id
                   WHERE s.session_id = NEW.session_id
                     AND s.end_time IS NULL
                     AND d.is_active
                     AND d.db_user = SUBSTRING_INDEX(USER(), '@', 1)) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'session does not belong to this device';
    END IF;
END//

DELIMITER ;
