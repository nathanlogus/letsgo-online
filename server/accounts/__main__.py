"""Operator commands. No passwords, bootstrap tokens or secrets are accepted in arguments."""
import argparse
from uuid import UUID, uuid4
import psycopg
from .app import audit
from .config import Config
from .db import migrate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("migrate")
    grant = sub.add_parser("grant-admin", help="Grant a signed-in account a staff role; HTTP still requires recent MFA")
    grant.add_argument("account_id", type=UUID)
    revoke = sub.add_parser("revoke-sessions")
    revoke.add_argument("account_id", type=UUID)
    sub.add_parser("prune-rate-buckets", help="Remove rate buckets older than one day")
    args = parser.parse_args()
    config = Config.from_env()
    if args.command == "migrate":
        migrate(config.database_url)
        print("Account database migrations applied")
        return
    with psycopg.connect(config.database_url) as conn:
        if args.command == "prune-rate-buckets":
            conn.execute("DELETE FROM rate_buckets WHERE minute < extract(epoch FROM now())::bigint/60-1440")
        else:
            if not conn.execute("SELECT 1 FROM accounts WHERE id=%s FOR UPDATE", (args.account_id,)).fetchone():
                parser.error("Account not found; sign in first")
            if args.command == "grant-admin":
                conn.execute("UPDATE accounts SET role='admin' WHERE id=%s", (args.account_id,))
            else:
                conn.execute("UPDATE sessions SET revoked=true WHERE account_id=%s", (args.account_id,))
            audit(conn, None, "operator."+args.command, args.account_id, str(uuid4()))
    print("Operator command completed")


if __name__ == "__main__":
    main()
