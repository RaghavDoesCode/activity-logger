-- Step 4: reliable delivery. Run once, as root: sudo mysql < file
USE activity_monitor;

-- A run of the logger now has an id made by the device (a UUID), so a session
-- can be created, or re-created, safely at any time.
ALTER TABLE sessions
    ADD COLUMN session_uuid CHAR(36) NULL AFTER device_id,
    ADD COLUMN server_received_at DATETIME NULL,
    ADD UNIQUE KEY uq_sess_uuid (session_uuid);

-- One row per segment already delivered. The primary key makes it impossible
-- to store the same segment twice.
CREATE TABLE IF NOT EXISTS ingest_log (
    device_id    INT         NOT NULL,
    segment_name VARCHAR(64) NOT NULL,
    session_id   INT         NOT NULL,
    received_at  DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (device_id, segment_name),
    CONSTRAINT fk_ing_dev  FOREIGN KEY (device_id)  REFERENCES devices(device_id),
    CONSTRAINT fk_ing_sess FOREIGN KEY (session_id) REFERENCES sessions(session_id)
);

DROP PROCEDURE IF EXISTS start_session;
DROP PROCEDURE IF EXISTS end_session;
DROP PROCEDURE IF EXISTS begin_segment;
DROP PROCEDURE IF EXISTS end_session_at;

DELIMITER //

-- Called by a device before it sends a segment. Finds or creates the session
-- of that run, and reports whether this exact segment was already delivered.
-- Who is calling still comes from the account (USER()), not from the client.
CREATE DEFINER = CURRENT_USER PROCEDURE begin_segment(
    IN p_uuid CHAR(36), IN p_machine VARCHAR(100),
    IN p_started BIGINT, IN p_segment VARCHAR(64))
SQL SECURITY DEFINER
BEGIN
    DECLARE v_dev INT;
    DECLARE v_emp INT;
    DECLARE v_sid INT;

    SELECT d.device_id, d.employee_id INTO v_dev, v_emp
    FROM devices d
    JOIN employees e ON e.employee_id = d.employee_id
    WHERE d.db_user = SUBSTRING_INDEX(USER(), '@', 1)
      AND d.is_active AND e.is_active;

    IF v_dev IS NULL THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'device not registered or disabled';
    END IF;

    SELECT session_id INTO v_sid
    FROM sessions
    WHERE session_uuid = p_uuid AND device_id = v_dev;

    IF v_sid IS NULL THEN
        INSERT INTO sessions (employee_id, device_id, session_uuid, machine_name,
                              start_time, server_received_at)
        VALUES (v_emp, v_dev, p_uuid, p_machine, FROM_UNIXTIME(p_started), NOW());
        SET v_sid = LAST_INSERT_ID();
    END IF;

    IF EXISTS (SELECT 1 FROM ingest_log
               WHERE device_id = v_dev AND segment_name = p_segment) THEN
        SELECT v_sid AS session_id, 1 AS already_done;
    ELSE
        INSERT INTO ingest_log (device_id, segment_name, session_id)
        VALUES (v_dev, p_segment, v_sid);
        SELECT v_sid AS session_id, 0 AS already_done;
    END IF;
END//

-- Closes the caller's own session, at the time the DEVICE says the run ended
CREATE DEFINER = CURRENT_USER PROCEDURE end_session_at(
    IN p_session_id INT, IN p_ended BIGINT)
SQL SECURITY DEFINER
BEGIN
    UPDATE sessions s
    JOIN devices d ON d.device_id = s.device_id
    SET s.end_time = FROM_UNIXTIME(p_ended)
    WHERE s.session_id = p_session_id
      AND s.end_time IS NULL
      AND d.db_user = SUBSTRING_INDEX(USER(), '@', 1);
END//

DELIMITER ;

-- Close the open sessions left behind by the earlier logger version
-- (end = time of their last snapshot, or their start if they have none)
UPDATE sessions s
SET s.end_time = COALESCE((SELECT MAX(p.log_time) FROM process_snapshots p
                            WHERE p.session_id = s.session_id), s.start_time)
WHERE s.end_time IS NULL AND s.session_uuid IS NULL;
