-- Additional operational history tables. Existing table definitions are unchanged.
CREATE TABLE IF NOT EXISTS data_batch (
  id CHAR(36) NOT NULL PRIMARY KEY,
  name VARCHAR(100) NOT NULL,
  data_source VARCHAR(10) NOT NULL,
  generator_version VARCHAR(20) NOT NULL,
  seed BIGINT NOT NULL,
  starts_at_utc DATETIME(3) NOT NULL,
  ends_at_utc DATETIME(3) NOT NULL,
  parameters_json LONGTEXT NOT NULL CHECK (JSON_VALID(parameters_json)),
  created_at_utc DATETIME(3) NOT NULL,
  CHECK (starts_at_utc < ends_at_utc)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS operation_context (
  id CHAR(36) NOT NULL PRIMARY KEY,
  batch_id CHAR(36) NOT NULL,
  room_id VARCHAR(50) NOT NULL,
  session_id INT NULL,
  context_type VARCHAR(30) NOT NULL,
  starts_at_utc DATETIME(3) NOT NULL,
  ends_at_utc DATETIME(3) NOT NULL,
  data_source VARCHAR(10) NOT NULL,
  details_json LONGTEXT NOT NULL CHECK (JSON_VALID(details_json)),
  KEY ix_context_room_time (room_id, starts_at_utc),
  FOREIGN KEY (batch_id) REFERENCES data_batch(id),
  FOREIGN KEY (session_id) REFERENCES borrowing_session(id),
  CHECK (starts_at_utc < ends_at_utc)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS cabinet_event (
  id CHAR(36) NOT NULL PRIMARY KEY,
  batch_id CHAR(36) NOT NULL,
  cabinet_id INT NOT NULL,
  room_id VARCHAR(50) NOT NULL,
  session_id INT NULL,
  command_id CHAR(32) NULL,
  event_type VARCHAR(40) NOT NULL,
  occurred_at_utc DATETIME(3) NOT NULL,
  received_at_utc DATETIME(3) NOT NULL,
  data_source VARCHAR(10) NOT NULL,
  details_json LONGTEXT NOT NULL CHECK (JSON_VALID(details_json)),
  KEY ix_cabinet_event_time (cabinet_id, occurred_at_utc),
  FOREIGN KEY (batch_id) REFERENCES data_batch(id),
  FOREIGN KEY (cabinet_id) REFERENCES cabinet(id),
  FOREIGN KEY (session_id) REFERENCES borrowing_session(id),
  CHECK (received_at_utc >= occurred_at_utc)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS anomaly_event (
  id CHAR(36) NOT NULL PRIMARY KEY,
  batch_id CHAR(36) NOT NULL,
  room_id VARCHAR(50) NOT NULL,
  session_id INT NULL,
  anomaly_type VARCHAR(40) NOT NULL,
  starts_at_utc DATETIME(3) NOT NULL,
  ends_at_utc DATETIME(3) NOT NULL,
  label_source VARCHAR(30) NOT NULL,
  details_json LONGTEXT NOT NULL CHECK (JSON_VALID(details_json)),
  KEY ix_anomaly_room_time (room_id, starts_at_utc),
  FOREIGN KEY (batch_id) REFERENCES data_batch(id),
  FOREIGN KEY (session_id) REFERENCES borrowing_session(id),
  CHECK (starts_at_utc < ends_at_utc)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Batch membership without altering the existing iot_sensor schema.
CREATE TABLE IF NOT EXISTS data_batch_sensor (
  batch_id CHAR(36) NOT NULL,
  sensor_id BIGINT NOT NULL,
  PRIMARY KEY (batch_id, sensor_id),
  UNIQUE KEY uq_batch_sensor (sensor_id),
  FOREIGN KEY (batch_id) REFERENCES data_batch(id),
  FOREIGN KEY (sensor_id) REFERENCES iot_sensor(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
