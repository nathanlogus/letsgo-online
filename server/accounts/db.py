from hashlib import sha256
from pathlib import Path
import psycopg
from psycopg.rows import dict_row
from psycopg_pool import ConnectionPool


def migrate(url):
    """Explicit, serialized, checksum-verified SQL migrations; never create tables on API startup."""
    with psycopg.connect(url, row_factory=dict_row) as conn:
        conn.execute("SELECT pg_advisory_xact_lock(73817382)")
        conn.execute("CREATE TABLE IF NOT EXISTS schema_migrations (name TEXT PRIMARY KEY, checksum TEXT NOT NULL)")
        for path in sorted((Path(__file__).parent / "migrations").glob("*.sql")):
            sql = path.read_text(encoding="utf-8")
            checksum = sha256(sql.encode()).hexdigest()
            row = conn.execute("SELECT checksum FROM schema_migrations WHERE name=%s", (path.name,)).fetchone()
            if row:
                if row["checksum"] != checksum:
                    raise RuntimeError(f"Migration changed after application: {path.name}")
                continue
            conn.execute(sql)
            conn.execute("INSERT INTO schema_migrations VALUES (%s,%s)", (path.name, checksum))


class Database:
    def __init__(self, url):
        self.pool = ConnectionPool(url, min_size=1, max_size=16, open=False,
                                   kwargs={"row_factory": dict_row, "connect_timeout": 5,
                                           "options": "-c statement_timeout=10000 -c lock_timeout=5000"})

    def open(self):
        self.pool.open(wait=True, timeout=10)
        with self.pool.connection() as conn:
            applied = {r["name"]: r["checksum"] for r in conn.execute("SELECT name,checksum FROM schema_migrations")}
            for p in (Path(__file__).parent / "migrations").glob("*.sql"):
                if applied.get(p.name) != sha256(p.read_text(encoding="utf-8").encode()).hexdigest():
                    raise RuntimeError("Run the accounts migrate command before serving")

    def close(self):
        self.pool.close()
