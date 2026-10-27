// Let's Go Mystery Gift admin: Keycloak sign-in (authorization code + PKCE S256 through /v1/auth/admin/*), then the
// admin API. Every value from the server goes into the page as text (textContent), never as HTML. The session token
// lives in memory only; the PKCE verifier, state and nonce live in sessionStorage across the Keycloak redirect.
"use strict";

const NATIVE = "lgpe-wb7-v1", BAG = "lgpe-items-v1";
const NATURES = ["Hardy", "Lonely", "Brave", "Adamant", "Naughty", "Bold", "Docile", "Relaxed", "Impish", "Lax", "Timid", "Hasty",
  "Serious", "Jolly", "Naive", "Modest", "Mild", "Quiet", "Bashful", "Rash", "Calm", "Gentle", "Sassy", "Careful", "Quirky"];
const STATS = ["HP", "Atk", "Def", "Spe", "SpA", "SpD"];
const TABS = {
  campaigns: ["Campaigns", "Each campaign is a gift players claim in the launcher or with a code, then receive in the game's own Mystery Gift."],
  accounts: ["Players", "Accounts that signed in to Let's Go online. Disabling one signs it out everywhere."],
  relay: ["Co-op sessions", "Live rooms on the co-op relay. Closing a room sends its players back to solo play."],
  audit: ["Activity", "What staff and players did, newest first. Codes and credentials are never recorded."],
};

let token = "", catalog = { items: [], species: [], moves: [] }, campaigns = [], filter = "all", current = "campaigns";
const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined && text !== null) e.textContent = String(text); return e; };
const img = (src, cls, alt = "") => { const i = el("img", cls); i.src = src; i.alt = alt; i.loading = "lazy"; return i; };
const button = (text, cls, fn) => { const b = el("button", "btn " + (cls || ""), text); b.type = "button"; b.addEventListener("click", fn); return b; };
const badge = (text, cls) => el("span", "badge " + (cls || ""), text);
const when = (iso) => iso ? new Date(iso).toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" }) : "";
const day = (iso) => iso ? new Date(iso).toLocaleDateString(undefined, { dateStyle: "medium" }) : "";
const short = (id) => id ? String(id).slice(0, 8) : "";
const hue = (s) => [...String(s)].reduce((h, c) => (h * 31 + c.charCodeAt(0)) % 360, 7);
const avatar = (name) => { const a = el("span", "avatar", (name || "?").trim().slice(0, 2).toUpperCase()); a.style.background = `linear-gradient(150deg, hsl(${hue(name)} 75% 65%), hsl(${(hue(name) + 40) % 360} 70% 52%))`; return a; };

// ---------------------------------------------------------------- feedback
function toast(message, bad) { const t = el("div", "toast" + (bad ? " bad" : ""), message); $("#toasts").append(t); setTimeout(() => t.remove(), bad ? 6000 : 3500); }
function confirmAction(title, text, ok) {
  return new Promise((resolve) => {
    const d = $("#confirm-dialog"); $("#confirm-title").textContent = title; $("#confirm-text").textContent = text; $("#confirm-ok").textContent = ok;
    d.addEventListener("close", () => resolve(d.returnValue === "ok"), { once: true }); d.returnValue = ""; d.showModal();
  });
}

async function api(method, path, body) {
  const r = await fetch(path, { method, cache: "no-store", headers: { "Authorization": "Bearer " + token, ...(body ? { "Content-Type": "application/json" } : {}) },
                               body: body ? JSON.stringify(body) : undefined });
  const data = await r.json().catch(() => ({}));
  const code = data.error?.code;
  if (r.status === 401 || code === "recent_mfa_required") { signedOut("Your admin session ended. Sign in again."); throw new Error("signed out"); }
  if (!r.ok) {
    const text = { move_not_in_lets_go: "One of the moves isn't in Let's Go.", invalid_request: "Some fields aren't valid. Check the gift and the dates.",
                   relay_unavailable: "The co-op relay isn't reachable.", admin_required: "This account isn't staff." }[code] || "Request failed: " + (code || r.status);
    toast(text, true); throw new Error(code || String(r.status));
  }
  return data;
}

const PAGE = 100;  // (the admin API's page size limit)
async function allPages(path) {
  const rows = [], join = path.includes("?") ? "&" : "?";
  for (let offset = 0; offset < 100000; offset += PAGE) {
    const page = await api("GET", `${path}${join}limit=${PAGE}&offset=${offset}`);
    rows.push(...page);
    if (page.length < PAGE) break;
  }
  return rows;
}
// A "Load more" button under a list that pages by 100: calls more() and removes itself.
function loadMore(list, more) {
  const holder = el("div", "load-more"), b = button("Load more", "small", async () => { holder.remove(); await more(); });
  holder.append(b); list.append(holder);
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
  location.assign(config.authorization_endpoint + "?" + q);  // (prompt=login: a fresh sign-in)
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
  $("#signin").hidden = false; $("#app").hidden = true; $("#who").hidden = true;
  for (const n of document.querySelectorAll("nav.tabs")) n.hidden = true;
  const e = $("#signin-error"); e.textContent = message || ""; e.hidden = !message;
}

// ---------------------------------------------------------------- icons and names
const itemById = (id) => catalog.items.find((i) => i.id === id);
const speciesById = (id) => catalog.species.find((s) => s.id === id);
const moveById = (id) => catalog.moves.find((m) => m.id === id);
const itemName = (id) => itemById(id)?.name || `Item ${id}`;
const speciesName = (id) => speciesById(id)?.name || `#${id}`;

function speciesIcon(id, form = 0, cls = "") {
  const box = el("span", "icon-item pk " + cls), i = img(`art/pk-${id}-${form}.png`, "", speciesName(id));
  i.onerror = () => { if (form) { form = 0; i.src = `art/pk-${id}-0.png`; } else box.replaceChildren(el("span", "icon-fallback", "#" + id)); };
  box.append(i); return box;
}
function itemIcon(id, qty, cls = "") {
  const box = el("span", "icon-item " + cls), i = img(`art/it-${id}.png`, "", itemName(id));
  i.onerror = () => {  // (a species candy has no icon of its own: the species with a candy badge)
    const species = catalog.species.find((s) => itemName(id) === s.name + " Candy");
    if (species && !i.dataset.candy) { i.dataset.candy = "1"; i.src = `art/pk-${species.id}-0.png`; const c = img("art/it-50.png", "candy"); box.append(c); }
    else box.replaceChildren(el("span", "icon-fallback", itemName(id).slice(0, 2)));
  };
  box.append(i);
  if (qty > 1) box.append(el("span", "qty", "×" + qty));
  return box;
}

function giftText(reward) {
  if (reward.kind === "pokemon") {
    const p = reward.pokemon;
    return `${p.form ? "Alolan " : ""}${speciesName(p.species)}, Lv. ${p.level}${p.shiny === "always" ? ", shiny" : p.shiny === "random" ? ", may be shiny" : ""}`;
  }
  return (reward.items || []).map((i) => `${itemName(i.item_id)} ×${i.quantity}`).join(", ") || "No items yet";
}

// A campaign (or the drawer's draft) as a gift card.
function giftCard(c, actions) {
  const pokemon = c.reward.kind === "pokemon";
  const card = el("article", "gift" + (pokemon ? " pokemon" : "")), inner = el("div", "gift-inner"), visual = el("div", "gift-visual"), icons = el("div", "icons");
  if (c.state === "published") visual.append(img("art/tag.png", "tag art-only"));
  if (pokemon) icons.append(c.reward.pokemon.species ? speciesIcon(c.reward.pokemon.species, c.reward.pokemon.form) : el("span", "icon-fallback", "?"));
  else for (const i of c.reward.items || []) icons.append(itemIcon(i.item_id, i.quantity));
  visual.append(icons);
  const body = el("div", "gift-body"), title = el("div", "gift-title");
  title.append(el("h3", "", c.title || "Untitled gift"), badge(c.state === "published" ? "Live" : c.state === "paused" ? "Paused" : "Draft", c.state));
  const meta = el("div", "gift-meta");
  const native = c.adapters.includes(NATIVE), bag = c.adapters.includes(BAG);
  meta.append(badge(native ? "Mystery Gift card" : "Bag", "plain card"));
  if (native && bag) meta.append(badge("+ bag fallback", "plain"));
  if (c.requires_code) meta.append(badge("Code", "plain"));
  for (const e of c.editions) meta.append(badge(e === "pikachu" ? "Pikachu" : "Eevee", "plain"));
  body.append(title, el("p", "gift-what", giftText(c.reward)), meta, el("p", "gift-dates", `${day(c.starts_at)} – ${day(c.ends_at)}`));
  const allocated = c.allocated || 0, meter = el("div", "meter"), fill = el("i");
  fill.style.width = c.stock ? Math.min(100, 100 * allocated / c.stock) + "%" : (allocated ? "100%" : "0");
  if (!c.stock) fill.style.opacity = ".35";
  meter.append(fill);
  const label = el("div", "meter-label"); label.append(el("span", "", `${allocated} claimed`), el("span", "", c.stock ? `of ${c.stock}` : "no limit"));
  body.append(label, meter);
  if (actions) body.append(actions);
  inner.append(visual, body); card.append(inner);
  return card;
}

// ---------------------------------------------------------------- campaigns
async function loadCampaigns() {
  campaigns = await allPages("/v1/admin/campaigns");
  campaigns.sort((a, b) => (b.created_at || "").localeCompare(a.created_at || ""));
  const stats = $("#campaign-stats"); stats.replaceChildren();
  const stat = (n, label, tone, art) => { const s = el("div", "stat"), i = el("div", "icon " + tone); if (art) i.append(img("art/" + art, "art-only")); const t = el("div"); t.append(el("b", "", n), el("span", "", label)); s.append(i, t); return s; };
  stats.append(stat(campaigns.filter((c) => c.state === "published").length, "live gifts", "tone-orange", "present.png"),
               stat(campaigns.reduce((n, c) => n + (c.allocated || 0), 0), "gifts claimed", "tone-teal", "bag.png"),
               stat(campaigns.filter((c) => c.reward.kind === "pokemon").length, "Pokémon gifts", "tone-violet", "ball.png"),
               stat(campaigns.filter((c) => c.requires_code).length, "need a code", "tone-lime", "joyful.png"));
  renderCampaigns();
}

function renderCampaigns() {
  const grid = $("#campaign-grid"), q = $("#campaign-search").value.trim().toLowerCase();
  grid.replaceChildren();
  const shown = campaigns.filter((c) => (filter === "all" || c.state === filter) && (!q || (c.title + " " + giftText(c.reward)).toLowerCase().includes(q)));
  for (const c of shown) {
    const actions = el("div", "gift-actions");
    actions.append(button(c.state === "published" ? "Pause" : "Publish", c.state === "published" ? "small" : "primary small", async () => {
      await api("POST", `/v1/admin/campaigns/${c.id}/action`, { action: c.state === "published" ? "pause" : "publish" });
      toast(c.state === "published" ? "Paused." : "Published: players can claim it now."); loadCampaigns();
    }));
    actions.append(button("New code", "small", () => newCode(c)), button("Claims", "small ghost", () => showClaims(c)));
    grid.append(giftCard(c, actions));
  }
  if (!shown.length) {
    const e = el("div", "empty"); e.append(img("art/site-pikachu.png", "art-only"), el("p", "", campaigns.length ? "No campaign matches." : "No campaigns yet. Create the first gift!"));
    grid.append(e);
  }
}

async function newCode(c) {
  const d = $("#uses-dialog"); d.returnValue = ""; d.showModal();
  await new Promise((r) => d.addEventListener("close", r, { once: true }));
  if (d.returnValue !== "ok") return;
  const uses = Number($("#uses-form").elements.uses.value);
  const code = await api("POST", `/v1/admin/campaigns/${c.id}/codes`, { max_uses: uses });
  $("#code-for").textContent = `For "${c.title}", ${uses} use${uses > 1 ? "s" : ""}.`;
  $("#code-value").textContent = code.code;
  $("#code-dialog").showModal();
}

async function showClaims(c) {
  const rows = await allPages(`/v1/admin/campaigns/${c.id}/claims`);
  $("#claims-title").textContent = `Claims of "${c.title}"`;
  const body = $("#claims-rows"); body.replaceChildren();
  for (const r of rows) {
    const tr = el("tr");
    for (const [v, cls] of [[short(r.id), "mono"], [short(r.account_id), "mono"], [null], [r.card_id ?? "–"], [when(r.created_at)], [when(r.acknowledged_at) || "–"]]) {
      const td = el("td", cls, v); if (v === null) td.append(badge(r.state === "allocated" ? "Waiting" : "Received", r.state === "allocated" ? "paused" : "published")); tr.append(td);
    }
    body.append(tr);
  }
  if (!rows.length) { const tr = el("tr"), td = el("td", "", "Nobody claimed it yet."); td.colSpan = 6; tr.append(td); body.append(tr); }
  $("#claims-dialog").showModal();
}

// ---------------------------------------------------------------- new campaign drawer
const draft = { kind: "item_bundle", items: [], species: 0, shiny: "never" };
const form = () => $("#new-campaign");

function openDrawer() {
  const f = form(); f.reset();
  f.elements.ball.value = "4";  // (reset selects the first ball: the Master Ball)
  Object.assign(draft, { kind: "item_bundle", items: [], species: 0, shiny: "never" });
  const now = new Date(), end = new Date(now.getTime() + 30 * 86400000), local = (d) => d.toISOString().slice(0, 16);
  f.elements.starts.value = local(now); f.elements.ends.value = local(end);
  setSegment("#kind", "kind", "item_bundle"); setSegment("#shiny", "v", "never");
  for (const i of $("#ivs").querySelectorAll("input")) i.value = "";
  for (const m of $("#moves").querySelectorAll("input")) m.value = "";
  $("#species-search").value = ""; $("#item-search").value = "";
  renderDraft();
  $("#drawer").hidden = false; $("#scrim").hidden = false;
  f.elements.title.focus();
}
function closeDrawer() { $("#drawer").hidden = true; $("#scrim").hidden = true; }
function setSegment(sel, key, value) { for (const b of $(sel).querySelectorAll("button")) b.classList.toggle("on", b.dataset[key] === value); }

function draftCampaign() {
  const f = form(), v = (n) => f.elements[n].value;
  const delivery = draft.kind === "pokemon" ? [NATIVE] : { both: [NATIVE, BAG], native: [NATIVE], bag: [BAG] }[v("delivery")];
  const reward = draft.kind === "pokemon"
    ? { kind: "pokemon", pokemon: { species: draft.species, form: f.elements.form.checked ? 1 : 0, level: Number(v("level")) || 1, shiny: draft.shiny,
        nature: Number(v("nature")), gender: v("gender"),
        ivs: [...$("#ivs").querySelectorAll("input")].map((i) => i.value === "" ? -1 : Number(i.value)),
        moves: [...$("#moves").querySelectorAll("input")].map((i) => catalog.moves.find((m) => m.name.toLowerCase() === i.value.trim().toLowerCase())?.id).filter(Boolean),
        ball: Number(v("ball")) || 4 } }
    : { kind: "item_bundle", items: draft.items.map((i) => ({ item_id: i.id, quantity: i.quantity })) };
  return { title: v("title"), starts_at: v("starts") ? new Date(v("starts") + "Z").toISOString() : "", ends_at: v("ends") ? new Date(v("ends") + "Z").toISOString() : "",
           requires_code: f.elements.requires_code.checked, editions: ["eevee", "pikachu"].filter((e) => f.elements[e].checked), adapters: delivery, reward,
           stock: v("stock") ? Number(v("stock")) : null, state: "draft", allocated: 0 };
}

function renderDraft() {
  const pokemon = draft.kind === "pokemon", f = form();
  $("#items-fields").hidden = pokemon; $("#pokemon-fields").hidden = !pokemon;
  f.elements.delivery.disabled = pokemon;
  const rows = $("#item-rows"); rows.replaceChildren();
  for (const it of draft.items) {
    const r = el("div", "item-row"), name = el("div", "name", itemName(it.id)), q = el("input");
    name.append(el("small", "", itemById(it.id)?.pocket || ""));
    q.type = "number"; q.min = 1; q.max = 999; q.value = it.quantity; q.setAttribute("aria-label", "Quantity");
    q.addEventListener("input", () => { it.quantity = Math.max(1, Math.min(999, Number(q.value) || 1)); preview(); });
    r.append(itemIcon(it.id, 0, "small"), name, q, button("Remove", "small ghost", () => { draft.items = draft.items.filter((x) => x !== it); renderDraft(); }));
    rows.append(r);
  }
  const card = f.elements.delivery.value !== "bag";
  $("#item-hint").textContent = draft.items.length ? (card && draft.items.length > 6 ? "A Mystery Gift card holds at most 6 items." : "") : "Add 1 to " + (card ? 6 : 32) + " different items.";
  const pick = $("#species-pick"); pick.replaceChildren();
  if (draft.species) {
    const s = speciesById(draft.species), box = el("div", "species-pick");
    box.append(speciesIcon(draft.species, f.elements.form.checked ? 1 : 0), el("span", "", `#${String(draft.species).padStart(3, "0")} ${s?.name || ""}`));
    pick.append(box);
  }
  f.elements.form.disabled = !speciesById(draft.species)?.alolan;
  if (f.elements.form.disabled) f.elements.form.checked = false;
  preview();
}
function preview() { $("#preview").replaceChildren(giftCard(draftCampaign())); }

function picker(input, list, search, choose) {
  const show = () => {
    const results = search(input.value.trim().toLowerCase()); list.replaceChildren();
    let group = "";
    for (const r of results) {  // (the whole catalog: 153 Pokemon, 207 items)
      if (r.group && r.group !== group) { group = r.group; list.append(el("div", "group", group)); }
      const b = el("button"); b.type = "button"; b.append(r.icon, el("span", "", r.name)); if (r.note) b.append(el("small", "", r.note));
      b.addEventListener("mousedown", (e) => { e.preventDefault(); choose(r); input.value = ""; list.hidden = true; });
      list.append(b);
    }
    list.hidden = !results.length;
  };
  input.addEventListener("input", show); input.addEventListener("focus", show);
  input.addEventListener("blur", () => setTimeout(() => { list.hidden = true; }, 120));
}

function setupDrawer() {
  const f = form();
  $("#nature").replaceChildren(new Option("Random", "-1"), ...NATURES.map((n, i) => new Option(n, String(i))));
  $("#ivs").replaceChildren(...STATS.map((s) => { const l = el("label", "", s), i = el("input"); i.type = "number"; i.min = 0; i.max = 31; i.placeholder = "?"; l.append(i); return l; }));
  $("#moves").replaceChildren(...[1, 2, 3, 4].map((n) => { const i = el("input"); i.setAttribute("list", "move-names"); i.placeholder = `Move ${n}`; i.setAttribute("aria-label", `Move ${n}`); return i; }));
  $("#move-names").replaceChildren(...catalog.moves.map((m) => new Option(m.name)));
  const balls = catalog.items.filter((i) => i.pocket === "Catching Items" && i.id <= 26);
  $("#ball").replaceChildren(...(balls.length ? balls : [{ id: 4, name: "Poké Ball" }]).map((b) => new Option(b.name, String(b.id))));
  $("#ball").value = "4";
  picker($("#item-search"), $("#item-results"), (q) => catalog.items
      .filter((i) => !draft.items.some((d) => d.id === i.id) && (!q || i.name.toLowerCase().includes(q) || i.pocket.toLowerCase().includes(q)))
      .sort((a, b) => a.pocket.localeCompare(b.pocket) || a.name.localeCompare(b.name))  // (one heading per bag pocket)
      .map((i) => ({ id: i.id, name: i.name, group: i.pocket, icon: itemIcon(i.id, 0, "small") })),
    (r) => { draft.items.push({ id: r.id, quantity: 1 }); renderDraft(); });
  picker($("#species-search"), $("#species-results"), (q) => catalog.species
      .filter((s) => !q || s.name.toLowerCase().includes(q) || String(s.id) === q.replace(/^#/, ""))
      .map((s) => ({ id: s.id, name: s.name, note: "#" + String(s.id).padStart(3, "0") + (s.alolan ? " · Alolan form" : ""), icon: speciesIcon(s.id, 0, "small") })),
    (r) => { draft.species = r.id; renderDraft(); });
  $("#kind").addEventListener("click", (e) => { const b = e.target.closest("button"); if (!b) return; draft.kind = b.dataset.kind; setSegment("#kind", "kind", draft.kind); renderDraft(); });
  $("#shiny").addEventListener("click", (e) => { const b = e.target.closest("button"); if (!b) return; draft.shiny = b.dataset.v; setSegment("#shiny", "v", draft.shiny); preview(); });
  f.addEventListener("input", (e) => { if (e.target.name === "form" || e.target.name === "delivery") renderDraft(); else preview(); });
  f.addEventListener("change", (e) => { if (e.target.name === "delivery") renderDraft(); });
  f.addEventListener("submit", async (e) => {
    e.preventDefault();
    const publish = e.submitter?.dataset.publish === "1", c = draftCampaign();
    if (!c.editions.length) return toast("Pick at least one game.", true);
    if (c.reward.kind === "pokemon" && !c.reward.pokemon.species) return toast("Pick a Pokémon.", true);
    if (c.reward.kind === "pokemon" && !c.reward.pokemon.moves.length) return toast("Give the Pokémon at least one move (names from the list).", true);
    if (c.reward.kind === "item_bundle" && !c.reward.items.length) return toast("Add at least one item.", true);
    const body = { title: c.title, starts_at: c.starts_at, ends_at: c.ends_at, requires_code: c.requires_code, editions: c.editions, adapters: c.adapters, reward: c.reward };
    if (c.stock) body.stock = c.stock;
    const created = await api("POST", "/v1/admin/campaigns", body);
    if (publish) await api("POST", `/v1/admin/campaigns/${created.id}/action`, { action: "publish" });
    toast(publish ? "Created and published." : "Saved as a draft.");
    closeDrawer(); loadCampaigns();
  });
  $("#drawer-close").addEventListener("click", closeDrawer);
  $("#scrim").addEventListener("click", closeDrawer);
  document.addEventListener("keydown", (e) => { if (e.key === "Escape" && !$("#drawer").hidden) closeDrawer(); });
}

// ---------------------------------------------------------------- players, co-op, activity
async function loadAccounts(query = "", offset = 0) {
  const rows = await api("GET", `/v1/admin/accounts?limit=${PAGE}&offset=${offset}&query=` + encodeURIComponent(query));
  const list = $("#account-list"); if (!offset) list.replaceChildren();
  for (const a of rows) {
    const r = el("div", "list-row"), who = el("div"), badges = el("div", "gift-meta");
    who.append(el("div", "name", a.display_name), el("div", "sub", a.id));
    if (a.role === "admin") badges.append(badge("Staff", "admin"));
    badges.append(badge(a.status === "active" ? "Active" : "Disabled", a.status === "active" ? "published" : "disabled"));
    const counts = el("div", "counts", `${a.profiles} save${a.profiles == 1 ? "" : "s"} · ${a.claims} gift${a.claims == 1 ? "" : "s"}`);
    const next = a.status === "active" ? "disabled" : "active";
    r.append(avatar(a.display_name), who, badges, button(next === "disabled" ? "Disable" : "Enable", "small", async () => {
      if (next === "disabled" && !(await confirmAction("Disable " + a.display_name + "?", "They are signed out everywhere and can't sign in until enabled again.", "Disable"))) return;
      await api("POST", `/v1/admin/accounts/${a.id}/status`, { status: next }); toast(next === "disabled" ? "Account disabled." : "Account enabled."); loadAccounts(query);
    }));
    r.insertBefore(counts, r.lastChild);
    list.append(r);
  }
  if (!rows.length && !offset) list.append(el("p", "empty", "No players found."));
  if (rows.length === PAGE) loadMore(list, () => loadAccounts(query, offset + PAGE));
}

async function loadRelay() {
  const stats = $("#relay-stats"), grid = $("#relay-grid"); stats.replaceChildren(); grid.replaceChildren();
  let data;
  try { data = await api("GET", "/v1/admin/relay"); } catch { grid.append(el("p", "empty", "The co-op relay isn't reachable right now.")); return; }
  const s = data.summary, stat = (n, label, tone, art) => { const d = el("div", "stat"), i = el("div", "icon " + tone); i.append(img("art/" + art, "art-only")); const t = el("div"); t.append(el("b", "", n), el("span", "", label)); d.append(i, t); return d; };
  stats.append(stat(s.rooms, "open rooms", "tone-teal", "ball.png"), stat(s.players, "players online", "tone-orange", "joyful.png"),
               stat(Math.round(s.uptime / 3600 * 10) / 10 + " h", "relay uptime", "tone-violet", "teary.png"), stat("v" + s.protocol, "protocol", "tone-lime", "bag.png"));
  for (const room of data.rooms) {
    const card = el("article", "frame session"), head = el("div", "session-head");
    head.append(el("code", "", room.id), badge(room.locked ? "Locked" : "Open", room.locked ? "paused" : "published"));
    card.append(head);
    for (const p of room.players) {
      const row = el("div", "player"); row.append(avatar(p.name), el("span", "", p.name));
      if (p.host) row.append(badge("Host", "admin"));
      row.append(badge(p.connected ? "Online" : "Reconnecting", p.connected ? "online" : "reconnecting"));
      card.append(row);
    }
    const info = [`open ${Math.round(room.seconds / 60)} min`];
    if (room.team) info.push("team-up in progress"); if (room.battle) info.push("battle in progress");
    card.append(el("p", "hint", info.join(" · ")), button("Close room", "danger small", async () => {
      if (!(await confirmAction("Close this room?", "Its players continue playing solo.", "Close room"))) return;
      await api("POST", `/v1/admin/relay/rooms/${room.id}/close`); toast("Room closed."); loadRelay();
    }));
    grid.append(card);
  }
  if (!data.rooms.length) { const e = el("div", "empty"); e.append(img("art/site-eevee.png", "art-only"), el("p", "", "Nobody is in a co-op room right now.")); grid.append(e); }
}

const ACTIONS = {
  "campaign.created": ["Created a campaign", "#f6913a"], "campaign.publish": ["Published a campaign", "#1f9d57"], "campaign.pause": ["Paused a campaign", "#b7791f"],
  "code.created": ["Made a code", "#8b5cf6"], "gift.allocated": ["Claimed a gift", "#2fd1c1"], "gift.client_acknowledged": ["Received a gift in the game", "#1f9d57"],
  "gift.card_reassigned": ["Got a new card number", "#4fb3f6"], "session.created": ["Signed in", "#b5e83c"], "profile.bound": ["Linked a save", "#4fb3f6"],
  "account.disabled": ["Disabled an account", "#e0435a"], "account.active": ["Enabled an account", "#1f9d57"], "relay.room_closed": ["Closed a co-op room", "#e0435a"],
};
async function loadAudit(offset = 0) {
  const rows = await api("GET", `/v1/admin/audit?limit=${PAGE}&offset=${offset}`), list = $("#audit-list");
  if (!offset) list.replaceChildren();
  for (const e of rows) {
    const [label, colour] = ACTIONS[e.action] || [e.action, "#a0a6b5"], li = el("li"), dot = el("span", "dot"), what = el("div");
    dot.style.background = colour;
    what.append(el("p", "what", label), el("p", "by", (e.actor_name || (e.actor_id ? short(e.actor_id) : "Server operator")) + (e.target_id ? ` · ${short(e.target_id)}` : "")));
    li.append(dot, what, el("time", "", when(e.created_at)));
    list.append(li);
  }
  if (!rows.length && !offset) list.append(el("li", "empty", "Nothing yet."));
  if (rows.length === PAGE) loadMore(list, () => loadAudit(offset + PAGE));
}

// ---------------------------------------------------------------- page
const loaders = { campaigns: loadCampaigns, accounts: () => loadAccounts(), relay: loadRelay, audit: () => loadAudit() };
function showTab(name) {
  current = name;
  for (const b of document.querySelectorAll("nav.tabs button")) b.classList.toggle("on", b.dataset.tab === name);
  for (const s of document.querySelectorAll(".tab")) s.hidden = s.id !== name;
  const [title, text] = TABS[name]; $("#banner-title").textContent = title; $("#banner-text").textContent = text;
  const action = $("#banner-action"); action.replaceChildren();
  if (name === "campaigns") action.append(button("New campaign", "primary", openDrawer));
  if (name === "relay") action.append(button("Refresh", "", () => loadRelay()));
  loaders[name]().catch(() => {});
}

function artwork() {  // the artwork when the server has it (ACCOUNT_DASHBOARD_ASSETS); plain gradients otherwise
  const probe = new Image();
  probe.onload = () => document.body.classList.add("art");
  probe.src = "art/site-pattern-ball.png";
}

async function main() {
  artwork();
  $("#signin-button").addEventListener("click", () => signIn().catch((e) => signedOut(String(e.message || e))));
  $("#signout").addEventListener("click", async () => {
    await fetch("/v1/auth/logout", { method: "POST", headers: { "Authorization": "Bearer " + token } }).catch(() => {});  // (revokes the session)
    signedOut("Signed out.");
  });
  for (const b of document.querySelectorAll("nav.tabs button")) b.addEventListener("click", () => showTab(b.dataset.tab));
  $("#campaign-filter").addEventListener("click", (e) => { const b = e.target.closest("button"); if (!b) return; filter = b.dataset.filter; setSegment("#campaign-filter", "filter", filter); renderCampaigns(); });
  $("#campaign-search").addEventListener("input", renderCampaigns);
  $("#account-search").addEventListener("submit", (e) => { e.preventDefault(); loadAccounts(e.target.elements.query.value).catch(() => {}); });
  $("#code-copy").addEventListener("click", async () => { try { await navigator.clipboard.writeText($("#code-value").textContent); toast("Code copied."); } catch { toast("Copy it by hand: the clipboard isn't available.", true); } });
  const params = new URLSearchParams(location.search);
  try {
    if (params.has("code")) await completeSignIn(params);
    else if (params.has("error")) { history.replaceState(null, "", location.pathname); throw new Error("Sign-in was cancelled or refused."); }
  } catch (e) { signedOut(String(e.message || e)); return; }
  if (!token) { signedOut(""); return; }
  const me = await api("GET", "/v1/me");
  if (me.role !== "admin") { signedOut(`${me.display_name} is not a staff account.`); return; }
  $("#who-name").textContent = me.display_name; $("#who-avatar").replaceWith(Object.assign(avatar(me.display_name), { id: "who-avatar" }));
  $("#signin").hidden = true; $("#app").hidden = false; $("#who").hidden = false;
  for (const n of document.querySelectorAll("nav.tabs")) n.hidden = false;
  catalog = await api("GET", "/v1/admin/catalog").catch(() => catalog);
  setupDrawer();
  showTab("campaigns");
}

main();
