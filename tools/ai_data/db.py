"""Local MySQL CLI access. Credentials stay in User Secrets / child environment."""
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
MYSQL = Path(r"E:/localhost/xampp/mysql/bin/mysql.exe")
SECRET_ID = "8440d348-7540-4d65-b3c0-00df83ab0560"


def settings():
    secret = Path(os.environ["APPDATA"]) / "Microsoft/UserSecrets" / SECRET_ID / "secrets.json"
    config = json.loads(secret.read_text(encoding="utf-8-sig"))
    connection = os.environ.get("ConnectionStrings__DefaultConnection") or config.get(
        "ConnectionStrings:DefaultConnection", config.get("ConnectionStrings", {}).get("DefaultConnection")
    )
    if not connection:
        raise RuntimeError("DefaultConnection not configured")
    # Quoted connection-string values may contain semicolons and doubled quotes.
    parts = re.findall(r'''(?:[^;'"\s][^;'"=]*=)?(?:"(?:[^"]|"")*"|'(?:[^']|'')*'|[^;])*;?''', connection)
    values = {}
    for part in parts:
        part = part.strip().rstrip(";")
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            quote = value[0]
            value = value[1:-1].replace(quote * 2, quote)
        values[key.strip().lower()] = value
    return values


def run(sql):
    cfg = settings()
    def get(*keys, default=""):
        return next((cfg[k] for k in keys if k in cfg), default)
    password = get("password", "pwd")
    env = dict(os.environ, MYSQL_PWD=password)
    args = [str(MYSQL), "--protocol=TCP", "--host=" + get("server", "host", "data source", default="localhost"),
            "--port=" + get("port", default="3306"),
            "--user=" + get("user id", "userid", "uid", "user", "username"),
            "--database=" + get("database", "initial catalog"),
            "--connect-timeout=5", "--default-character-set=utf8mb4", "--batch", "--raw", "--skip-column-names"]
    result = subprocess.run(args, input=sql, capture_output=True, text=True, encoding="utf-8", env=env)
    if result.returncode:
        error = result.stderr.replace(password, "[REDACTED]") if password else result.stderr
        raise RuntimeError(error)
    return result.stdout


def rows(select):
    return [json.loads(line) for line in run("START TRANSACTION READ ONLY;\n" + select + ";\nCOMMIT;").splitlines() if line]


def literal(value):
    if value is None:
        return "NULL"
    if isinstance(value, (int, float)):
        return str(value)
    data = str(value).encode("utf-8").hex()
    # Explicit collation also works across the existing general_ci tables and
    # the new unicode_ci history tables (MariaDB otherwise rejects some comparisons).
    return "CONVERT(0x" + data + " USING utf8mb4) COLLATE utf8mb4_unicode_ci" if data else "''"


if __name__ == "__main__":
    checks = {
        "tables": "SELECT JSON_OBJECT('table',TABLE_NAME,'engine',ENGINE) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() ORDER BY TABLE_NAME",
        "schema": "SELECT JSON_OBJECT('table',TABLE_NAME,'column',COLUMN_NAME,'type',COLUMN_TYPE,'nullable',IS_NULLABLE,'default',COLUMN_DEFAULT,'extra',EXTRA) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('iot_sensor','iot_node','room','cabinet','borrowing_session') ORDER BY TABLE_NAME,ORDINAL_POSITION",
        "nodes": "SELECT JSON_OBJECT('id',id,'room_id',room_id,'code',node_code,'source',data_source,'status',status,'interval',publish_interval_seconds) FROM iot_node ORDER BY id",
        "sensors": "SELECT JSON_OBJECT('node_id',node_id,'readings',readings_json,'source',data_source) FROM iot_sensor ORDER BY id LIMIT 3",
        "coverage": "SELECT JSON_OBJECT('room_id',room_id,'sessions',COUNT(*),'valid',SUM(received_at IS NOT NULL AND returned_at>received_at),'first',MIN(borrowing_date),'last',MAX(borrowing_date)) FROM borrowing_session WHERE room_id IN (SELECT room_id FROM iot_node) GROUP BY room_id ORDER BY COUNT(*) DESC",
        "constraints": "SELECT JSON_OBJECT('table',TABLE_NAME,'index',INDEX_NAME,'unique',NON_UNIQUE,'column',COLUMN_NAME) FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('iot_sensor','iot_node') ORDER BY TABLE_NAME,INDEX_NAME,SEQ_IN_INDEX"
    }
    report = {name: rows(query) for name, query in checks.items()}
    out = ROOT / ".codex-temp/ai-data"
    out.mkdir(parents=True, exist_ok=True)
    (out / "inspection.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
