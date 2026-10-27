// Let's Go admin dashboard: Keycloak sign-in (authorization code + PKCE S256 through /v1/auth/admin/*), then the admin
// API. Every value from the server goes into the page as text (textContent), never as HTML. The session token lives in
// memory only; the PKCE verifier, state and nonce live in sessionStorage across the Keycloak redirect.
"use strict";

let token = "";
const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, text, cls) => { const e = document.createElement(tag); if (text !== undefined) e.textContent = String(text); if (cls) e.className = cls; return e; };
const row = (cells) => { const tr = el("tr"); for (const c of cells) { const td = el("td"); if (c instanceof Node) td.append(c); else td.textContent = c ?? ""; tr.append(td); } return tr; };
const button = (text, fn) => { const b = el("button", text); b.type = "button"; b.addEventListener("click", fn); return b; };
const when = (iso) => iso ? new Date(iso).toISOString().replace("T", " ").slice(0, 16) : "";
const short = (id) => id ? String(id).slice(0, 8) : "";

function showError(message) { const e = $("#error"); e.textContent = message || ""; e.hidden = !message; }

async function api(method, path, body) {
  const r = await fetch(path, { method, headers: { "Authorization": "Bearer " + token, ...(body ? { "Content-Type": "application/json" } : {}) },
                               body: body ? JSON.stringify(body) : undefined, cache: "no-store" });
  const data = await r.json().catch(() => ({}));
  if (r.status === 401 || (r.status === 403 && data.error?.code === "recent_mfa_required")) { signedOut("Your admin session ended. Sign in again."); throw new Error("signed out"); }
  if (!r.ok) { const code = data.error?.code || r.status; showError("Request failed: " + code); throw new Error(code); }
  showError("");
  return data;
}

// ---------------------------------------------------------------- sign-in
const b64url = (bytes) => btoa(String.fromCharCode(...new Uint8Array(bytes))).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
const random = (n) => b64url(crypto.getRandomValues(new Uint8Array(n)));

async function signIn() {
  const config = await (await fetch("/v1/auth/admin/config", { cache: "no-store" })).json();
  const verifier = random(48), state = random(24), nonce = random(32);
  const challenge = b64url(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(verifier)));
  sessionStorage.setItem("pkce", JSON.stringify({ verifier, state, nonce }));
  const q = new URLSearchParams({ response_type: "code", client_id: config.client_id, redirect_uri: config.redirect_uri, scope: "openid",
                                  code_challenge: challenge, code_challenge_method: "S256", state, nonce, prompt: "login" });
  location.assign(config.authorization_endpoint + "?" + q);  // (prompt=login: a fresh sign-in, the MFA the admin API wants)
}

async function completeSignIn(params) {
  const saved = JSON.parse(sessionStorage.getItem("pkce") || "{}");
  sessionStorage.removeItem("pkce");
  history.replaceState(null, "", location.pathname);  // (the code leaves the address bar and the history)
  if (!saved.state || params.get("state") !== saved.state) throw new Error("The sign-in reply didn't match this page. Sign in again.");
  const r = await fetch("/v1/auth/admin/complete", { method: "POST", headers: { "Content-Type": "application/json" }, cache: "no-store",
    body: JSON.stringify({ code: params.get("code"), code_verifier: saved.verifier, nonce: saved.nonce }) });
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error("Sign-in failed: " + (data.error?.code || r.status));
  token = data.access_token;
}

function signedOut(message) {
  token = "";
  $("#signin").hidden = false;
  for (const s of document.querySelectorAll(".tab")) s.hidden = true;
  $("#tabs").hidden = true;
  $("#who").textContent = "";
  showError(message);
}

// ---------------------------------------------------------------- campaigns
function giftText(reward) {
  if (reward.kind === "pokemon") { const p = reward.pokemon; return `Pokémon #${p.species}${p.form ? " (Alolan)" : ""}, Lv. ${p.level}${p.shiny !== "never" ? `, shiny ${p.shiny}` : ""}`; }
  return reward.items.map((i) => `item ${i.item_id} ×${i.quantity}`).join(", ");
}
const delivery = (adapters) => adapters.includes("lgpe-wb7-v1") ? (adapters.includes("lgpe-items-v1") ? "card + bag" : "card") : "bag";

async function loadCampaigns() {
  const rows = await api("GET", "/v1/admin/campaigns?limit=100");
  const body = $("#campaign-table tbody"); body.replaceChildren();
  for (const c of rows) {
    const state = el("span", c.state, "state-" + c.state);
    const actions = el("div");
    actions.append(button(c.state === "published" ? "Pause" : "Publish", async () => { await api("POST", `/v1/admin/campaigns/${c.id}/action`, { action: c.state === "published" ? "pause" : "publish" }); loadCampaigns(); }));
    actions.append(button("New code", async () => {
      const uses = Number(prompt("How many uses for this code?", "1"));
      if (!Number.isInteger(uses) || uses < 1) return;
      const code = await api("POST", `/v1/admin/campaigns/${c.id}/codes`, { max_uses: uses });
      const shown = $("#code-shown"); shown.hidden = false;
      shown.textContent = `Code for "${c.title}" (${uses} use${uses > 1 ? "s" : ""}): ${code.code} — shown once, copy it now.`;
    }));
    actions.append(button("Claims", () => loadClaims(c)));
    body.append(row([c.title, state, `${when(c.starts_at)} → ${when(c.ends_at)}`, giftText(c.reward), delivery(c.adapters) + (c.requires_code ? ", code" : ""),
                     `${c.allocated}${c.stock ? " / " + c.stock : ""}`, actions]));
  }
}

async function loadClaims(c) {
  const rows = await api("GET", `/v1/admin/campaigns/${c.id}/claims?limit=100`);
  $("#claims").hidden = false;
  $("#claims-title").textContent = `Claims of "${c.title}"`;
  const body = $("#claims tbody"); body.replaceChildren();
  for (const r of rows) body.append(row([short(r.id), short(r.account_id), r.state === "allocated" ? "waiting" : "received", r.card_id ?? "", when(r.created_at), when(r.acknowledged_at)]));
  if (!rows.length) body.append(row(["No claims yet."]));
}

function numbers(text) { return text.trim() ? text.trim().split(/[\s,]+/).map(Number) : []; }

async function createCampaign(event) {
  event.preventDefault();
  const f = event.target, v = (n) => f.elements[n].value;
  const editions = ["eevee", "pikachu"].filter((e) => f.elements[e].checked);
  const adapters = { both: ["lgpe-wb7-v1", "lgpe-items-v1"], native: ["lgpe-wb7-v1"], bag: ["lgpe-items-v1"] }[v("delivery")];
  let reward;
  if (v("kind") === "pokemon") {
    reward = { kind: "pokemon", pokemon: { species: Number(v("species")), form: f.elements.form.checked ? 1 : 0, level: Number(v("level")),
      shiny: v("shiny"), nature: Number(v("nature")), gender: v("gender"), ivs: numbers(v("ivs")), moves: numbers(v("moves")), ball: Number(v("ball")) } };
  } else {
    reward = { kind: "item_bundle", items: v("items").trim().split("\n").filter(Boolean).map((line) => { const [id, n] = numbers(line); return { item_id: id, quantity: n }; }) };
  }
  const body = { title: v("title"), starts_at: new Date(v("starts") + "Z").toISOString(), ends_at: new Date(v("ends") + "Z").toISOString(),
                 requires_code: f.elements.requires_code.checked, editions, adapters: reward.kind === "pokemon" ? ["lgpe-wb7-v1"] : adapters, reward };
  if (v("stock")) body.stock = Number(v("stock"));
  await api("POST", "/v1/admin/campaigns", body);
  f.reset();
  loadCampaigns();
}

// ---------------------------------------------------------------- accounts, relay, audit
async function loadAccounts(query = "") {
  const rows = await api("GET", "/v1/admin/accounts?limit=100&query=" + encodeURIComponent(query));
  const body = $("#accounts tbody"); body.replaceChildren();
  for (const a of rows) {
    const next = a.status === "active" ? "disabled" : "active";
    body.append(row([a.display_name, a.id, a.role, a.status, a.profiles, a.claims, when(a.created_at),
      button(next === "disabled" ? "Disable" : "Enable", async () => { await api("POST", `/v1/admin/accounts/${a.id}/status`, { status: next }); loadAccounts(query); })]));
  }
}

async function loadRelay() {
  const body = $("#relay tbody"); body.replaceChildren();
  let data;
  try { data = await api("GET", "/v1/admin/relay"); } catch (e) { $("#relay-summary").textContent = "The relay is not reachable."; return; }
  const s = data.summary;
  $("#relay-summary").textContent = `Protocol ${s.protocol}, ${s.rooms} session(s), ${s.players} player(s) online, up ${Math.round(s.uptime / 60)} min.`;
  for (const r of data.rooms) {
    const players = r.players.map((p) => `${p.name}${p.host ? " (host)" : ""}${p.connected ? "" : " (reconnecting)"}`).join(", ");
    body.append(row([r.id, players, `${Math.round(r.seconds / 60)} min`, r.locked ? "yes" : "no", r.team || "", r.battle || "",
      button("Close", async () => { if (confirm("Close this session? Its players continue solo.")) { await api("POST", `/v1/admin/relay/rooms/${r.id}/close`); loadRelay(); } })]));
  }
  if (!data.rooms.length) body.append(row(["No sessions."]));
}

async function loadAudit() {
  const rows = await api("GET", "/v1/admin/audit?limit=100");
  const body = $("#audit tbody"); body.replaceChildren();
  for (const e of rows) body.append(row([when(e.created_at), e.actor_name || (e.actor_id ? short(e.actor_id) : "operator"), e.action, short(e.target_id), short(e.request_id)]));
}

// ---------------------------------------------------------------- page
const loaders = { campaigns: loadCampaigns, accounts: () => loadAccounts(), relay: loadRelay, audit: loadAudit };

function showTab(name) {
  for (const b of document.querySelectorAll("#tabs button")) b.classList.toggle("on", b.dataset.tab === name);
  for (const s of document.querySelectorAll(".tab")) s.hidden = s.id !== name;
  loaders[name]().catch(() => {});
}

function artwork() {  // the game's artwork when the server has it (ACCOUNT_DASHBOARD_ASSETS); plain colours otherwise
  const probe = new Image();
  probe.onload = () => document.body.classList.add("art");
  probe.src = "art/bg.png";
}

async function main() {
  artwork();
  $("#signin-button").addEventListener("click", () => signIn().catch((e) => showError(String(e.message || e))));
  for (const b of document.querySelectorAll("#tabs button")) b.addEventListener("click", () => showTab(b.dataset.tab));
  $("#new-campaign").addEventListener("submit", (e) => createCampaign(e).catch(() => {}));
  $("#new-campaign").elements.kind.addEventListener("change", (e) => { $("#items-fields").hidden = e.target.value === "pokemon"; $("#pokemon-fields").hidden = e.target.value !== "pokemon"; });
  $("#account-search").addEventListener("submit", (e) => { e.preventDefault(); loadAccounts(e.target.elements.query.value).catch(() => {}); });
  $("#relay-refresh").addEventListener("click", () => loadRelay());
  const params = new URLSearchParams(location.search);
  try {
    if (params.has("code")) await completeSignIn(params);
    else if (params.has("error")) { history.replaceState(null, "", location.pathname); throw new Error("Sign-in was cancelled or refused."); }
  } catch (e) { signedOut(String(e.message || e)); return; }
  if (!token) { signedOut(""); return; }
  const me = await api("GET", "/v1/me");
  if (me.role !== "admin") { signedOut(`${me.display_name} is not a staff account.`); return; }
  $("#who").textContent = me.display_name || "";
  $("#signin").hidden = true;
  $("#tabs").hidden = false;
  showTab("campaigns");
}

main();
