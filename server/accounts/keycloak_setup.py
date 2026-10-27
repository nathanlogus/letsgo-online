"""Add an isolated native client to an existing Keycloak realm; never modify other clients.
Credentials are read from environment or an operator-selected env file, never CLI arguments.
Requires the development dependencies (httpx).
"""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
from urllib.parse import quote, urlsplit
import httpx


def load_env(path):
    values = {}
    for line in Path(path).read_text(encoding="utf-8-sig").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


@contextmanager
def admin_client(base_url, values):
    client = httpx.Client(base_url=base_url.rstrip("/"), timeout=15, trust_env=False)
    try:
        result = client.post("/realms/master/protocol/openid-connect/token", data={
            "grant_type": "password", "client_id": "admin-cli",
            "username": values["KEYCLOAK_ADMIN_USERNAME"], "password": values["KEYCLOAK_ADMIN_PASSWORD"]})
        result.raise_for_status()
        client.headers["Authorization"] = "Bearer " + result.json()["access_token"]
        yield client
    finally:
        client.close()


def ensure_client(client, realm, native_id, redirect_uri):
    path = "/admin/realms/" + quote(realm, safe="")
    expected = {
        "clientId": native_id, "name": "Let's Go native login", "enabled": True,
        "protocol": "openid-connect", "publicClient": True, "standardFlowEnabled": True,
        "directAccessGrantsEnabled": False, "implicitFlowEnabled": False,
        "serviceAccountsEnabled": False, "fullScopeAllowed": False,
        "redirectUris": [redirect_uri], "webOrigins": [],
        "attributes": {"pkce.code.challenge.method": "S256", "oauth2.device.authorization.grant.enabled": "false"},
        "protocolMappers": [{"name": "letsgo-api-audience", "protocol": "openid-connect",
            "protocolMapper": "oidc-audience-mapper", "consentRequired": False,
            "config": {"included.custom.audience": "letsgo-api", "id.token.claim": "false", "access.token.claim": "true"}}],
    }
    response = client.get(path + "/clients", params={"clientId": native_id}); response.raise_for_status()
    existing = response.json()
    created = False
    if not existing:
        response = client.post(path + "/clients", json=expected); response.raise_for_status()
        response = client.get(path + "/clients", params={"clientId": native_id}); response.raise_for_status()
        existing = response.json(); created = True
    if len(existing) != 1:
        raise RuntimeError("Expected exactly one dedicated Let’s Go client")
    actual = existing[0]
    for key in ("publicClient", "standardFlowEnabled", "directAccessGrantsEnabled", "implicitFlowEnabled", "serviceAccountsEnabled", "redirectUris"):
        if actual.get(key) != expected[key]:
            raise RuntimeError(f"Existing client has different {key}; refusing to overwrite it")
    if actual.get("attributes", {}).get("pkce.code.challenge.method") != "S256":
        raise RuntimeError("Existing client does not enforce PKCE S256")
    mappers = actual.get("protocolMappers", [])
    if not any(m.get("protocolMapper") == "oidc-audience-mapper" and
               m.get("config", {}).get("included.custom.audience") == "letsgo-api" and
               m.get("config", {}).get("access.token.claim") == "true" and
               m.get("config", {}).get("id.token.claim") == "false" for m in mappers):
        raise RuntimeError("Existing client has no matching access-token-only API audience")
    return {"client_id": native_id, "internal_id": actual["id"], "created": created, "redirect_uri": redirect_uri}


def ensure_staff_mfa(client, realm, client_id):
    """The dashboard client's own sign-in: password then a required one-time code (TOTP; Keycloak asks to set one up the
    first time), reported as amr "mfa" in its tokens (the admin API's check). A copy of the realm's browser flow, bound to
    this client only: other clients and the realm's own flows are left as they are."""
    path = "/admin/realms/" + quote(realm, safe="")
    alias = client_id + " browser"
    flows = client.get(path + "/authentication/flows"); flows.raise_for_status()
    if not any(f["alias"] == alias for f in flows.json()):
        client.post(path + "/authentication/flows/browser/copy", json={"newName": alias}).raise_for_status()
        flows = client.get(path + "/authentication/flows"); flows.raise_for_status()
    flow = next(f for f in flows.json() if f["alias"] == alias)
    executions = lambda: client.get(path + "/authentication/flows/" + quote(alias, safe="") + "/executions").json()
    def require(execution, requirement):
        if execution["requirement"] != requirement:
            client.put(path + "/authentication/flows/" + quote(alias, safe="") + "/executions",
                       json={"id": execution["id"], "requirement": requirement}).raise_for_status()
    steps = executions()
    forms = next(e for e in steps if e.get("authenticationFlow") and e["level"] == 0 and "forms" in e["displayName"].lower())
    for e in steps:
        if e.get("providerId") == "auth-cookie":
            require(e, "DISABLED")  # (an earlier sign-in's cookie would skip the code)
        if e.get("authenticationFlow") and e["level"] == 1 and "conditional" in e["displayName"].lower():
            require(e, "DISABLED")  # (the realm's optional second factor: replaced by the required one below)
    otp = [e for e in steps if e.get("providerId") == "auth-otp-form" and e["level"] == 1 and not e.get("authenticationFlow")
           and e.get("authenticationConfig")]
    if not otp:
        client.post(path + "/authentication/flows/" + quote(forms["displayName"], safe="") + "/executions/execution",
                    json={"provider": "auth-otp-form"}).raise_for_status()
        added = [e for e in executions() if e.get("providerId") == "auth-otp-form" and e["level"] == 1 and not e.get("authenticationConfig")]
        client.post(path + "/authentication/executions/" + added[-1]["id"] + "/config",
                    json={"alias": client_id + "-otp-mfa", "config": {"default.reference.value": "mfa"}}).raise_for_status()
        otp = [e for e in executions() if e["id"] == added[-1]["id"]]
    require(otp[0], "REQUIRED")
    for _ in range(8):  # (after the password form: the code needs the user it belongs to)
        level1 = [e for e in executions() if e["level"] == 1 and e.get("parentFlow", forms.get("flowId")) == forms.get("flowId")]
        mine = next(e for e in level1 if e["id"] == otp[0]["id"])
        password = next(e for e in level1 if e.get("providerId") == "auth-username-password-form")
        if mine["index"] > password["index"]:
            break
        client.post(path + "/authentication/executions/" + mine["id"] + "/lower-priority").raise_for_status()
    found = client.get(path + "/clients", params={"clientId": client_id}); found.raise_for_status()
    app = found.json()[0]
    if app.get("authenticationFlowBindingOverrides", {}).get("browser") != flow["id"]:
        app["authenticationFlowBindingOverrides"] = {**app.get("authenticationFlowBindingOverrides", {}), "browser": flow["id"]}
        client.put(path + "/clients/" + app["id"], json=app).raise_for_status()
    if not any(m.get("protocolMapper") == "oidc-amr-mapper" for m in app.get("protocolMappers", [])):
        client.post(path + "/clients/" + app["id"] + "/protocol-mappers/models", json={
            "name": "amr", "protocol": "openid-connect", "protocolMapper": "oidc-amr-mapper",
            "config": {"access.token.claim": "true", "id.token.claim": "true", "introspection.token.claim": "true"}}).raise_for_status()
    return {"flow": alias, "steps": [(e["displayName"], e["requirement"]) for e in executions()]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--credentials-env", type=Path)
    parser.add_argument("--base-url", default="http://localhost:8080")
    parser.add_argument("--realm", default="johto")
    parser.add_argument("--client-id", default="letsgo-native")
    parser.add_argument("--redirect-uri", default="http://127.0.0.1:7386/callback")
    parser.add_argument("--staff-mfa", action="store_true", help="The dashboard client: require a one-time code, reported as amr mfa")
    args = parser.parse_args()
    parsed = urlsplit(args.base_url)
    if parsed.scheme != "https" and parsed.hostname not in ("localhost", "127.0.0.1", "::1"):
        parser.error("Use HTTPS for non-local Keycloak administration")
    values = load_env(args.credentials_env) if args.credentials_env else os.environ
    with admin_client(args.base_url, values) as client:
        result = ensure_client(client, args.realm, args.client_id, args.redirect_uri)
        if args.staff_mfa:
            result["staff_mfa"] = ensure_staff_mfa(client, args.realm, args.client_id)
        discovery = client.get(f"/realms/{quote(args.realm, safe='')}/.well-known/openid-configuration")
        discovery.raise_for_status()
        metadata = discovery.json()
    result.update(issuer=metadata["issuer"], jwks_url=metadata["jwks_uri"], audience="letsgo-api")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
