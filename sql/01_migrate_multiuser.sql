-- Step 1a: multi-employee / multi-device schema (run once, as root: sudo mysql < file)
USE activity_monitor;

-- Who works here
CREATE TABLE IF NOT EXISTS employees (
    employee_id   INT AUTO_INCREMENT PRIMARY KEY,
    employee_code VARCHAR(50)  NOT NULL UNIQUE,
    full_name     VARCHAR(100) NOT NULL,
    department    VARCHAR(100) NULL,
    is_active     BOOLEAN      NOT NULL DEFAULT TRUE,
    created_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- Which machines are enrolled. db_user is the MySQL account that machine
-- logs in with, so the database can tell WHICH device is calling.
CREATE TABLE IF NOT EXISTS devices (
    device_id     INT AUTO_INCREMENT PRIMARY KEY,
    employee_id   INT          NOT NULL,
    device_name   VARCHAR(100) NOT NULL,
    db_user       VARCHAR(32)  NOT NULL UNIQUE,
    is_active     BOOLEAN      NOT NULL DEFAULT TRUE,
    registered_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT fk_dev_emp FOREIGN KEY (employee_id) REFERENCES employees(employee_id),
    UNIQUE KEY uq_emp_device (employee_id, device_name)
);

-- Keep the old rows: turn the old free-text employee_id into real employees
INSERT IGNORE INTO employees (employee_code, full_name)
SELECT DISTINCT COALESCE(employee_id, 'LEGACY'), COALESCE(employee_id, 'LEGACY')
FROM sessions;

ALTER TABLE sessions
    ADD COLUMN emp_id    INT NULL AFTER session_id,
    ADD COLUMN device_id INT NULL AFTER emp_id;

UPDATE sessions s
JOIN employees e ON e.employee_code = COALESCE(s.employee_id, 'LEGACY')
SET s.emp_id = e.employee_id;

ALTER TABLE sessions DROP COLUMN employee_id;
ALTER TABLE sessions CHANGE emp_id employee_id INT NOT NULL;

ALTER TABLE sessions
    ADD CONSTRAINT fk_sess_emp FOREIGN KEY (employee_id) REFERENCES employees(employee_id),
    ADD CONSTRAINT fk_sess_dev FOREIGN KEY (device_id)   REFERENCES devices(device_id),
    ADD INDEX idx_sess_emp_start (employee_id, start_time),
    ADD INDEX idx_sess_dev (device_id);
