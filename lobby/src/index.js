// The Spikeout internet lobby (docs/netplay.md), a Cloudflare Worker with
// one Durable Object holding every open game. It only introduces players:
// the game itself goes between their machines (peer to peer, Cloudflare's
// TURN relay when their routers won't allow that).
//
// One WebSocket a player, JSON text messages, {"t": type, ...}:
//
//   hello  {build, name, version}        -> welcome {address, country, turn} (country: Cloudflare's two letters for the
//                                           player, "" if none: a host shows its own with its seat)
//   list   {from}                        -> games {games: [{code, name, seats, taken, watchers, started, session, country,
//                                                  ms}], from, total} (LIST_PAGE from `from`: waiting first, then in
//                                                  progress with a cabinet free, then full ones (to watch))
//   host   {name, seats, public, session, port, candidates[, takeover: code]}
//                                        -> hosted {code}
//   update {taken, started[, watchers]}  (the host's game)
//   unhost {}
//   join   {code, port, candidates[, watch]}
//                                        -> joining {code, name, seats, session, candidates, country} (watch: a
//                                           spectator, let into a full game too)
//                                           and the host gets joiner {name, candidates, country, watch} (each the
//                                           other's country, as the lobby has it: never what a game says)
//   turn   {}                            -> turn {iceServers} (short-lived TURN credentials)
//   "ping"                               -> "pong" (answered without waking the object)
//
// Errors: error {why}. Candidates are "ip:port" strings: the addresses a
// machine can be reached at (its own on its networks, its public one as
// STUN saw it); the lobby adds the address it sees the player at.
// Games are listed only to players with the same build (the same game,
// ROMs and version); private games only by their code. The list says how
// many public games are on other builds (others), and a join by the code
// of one says its version (the game asks the player to update).

import { DurableObject } from "cloudflare:workers";

const CODE_LETTERS = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // (no I, O, 0, 1)
const CODE_LENGTH = 6;
const MAX_MESSAGE = 4096;
const MAX_CANDIDATES = 8;
const LIST_PAGE = 50; // games a list message
const ROOM_KEPT = 30 * 60 * 1000; // ms a game in progress is kept after its host goes, to take over

// Bug reports (src/net/report.hpp in the game): POST /v1/report {title, body, comments[], github_name} -> {number,
// url} or {why}. The lobby opens a GitHub issue on REPORT_REPO as the project's GitHub App (the one players log in
// with from the game; its only permission "Issues: read and write", installed on that repository): the issue shows
// as opened by the app's bot. Its id and private key are the lobby's: `npx wrangler secret put APP_ID` and
// `APP_PRIVATE_KEY` (the .pem file GitHub gives, as it is); a short-lived token is made from them for each report.
// (REPORT_TOKEN instead: a token used as it is, for the tests.) The logs go in as the issue's comments; "Reported by
// @name" if the player gave a GitHub name that exists. It keeps nothing. No "@" of the player's notifies anyone; at
// most REPORTS_AN_HOUR from one address (the Reports object counts them), REPORTS_A_DAY in all.
const REPORT_BYTES = 1 << 20;
const REPORT_TEXT = 65000; // (GitHub: 65,536 characters an issue or a comment)
const REPORT_COMMENTS = 4;
const REPORTS_AN_HOUR = 5;
const REPORTS_A_DAY = 300;
const REPORT_MARK = "_Sent from the game"; // (the game's own reports: the line it ends the issue with)

function quiet(s) { // (an @ the player typed mentions nobody: a zero-width space after it)
	return s.replace(/@(?!​)/g, "@​");
}

// The GitHub App's own token for the repository: a JWT signed with its private key (RS256), then the installation's
// token for it (an hour long).
const b64url = (bytes) => btoa(String.fromCharCode(...new Uint8Array(bytes))).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
const derLength = (n) => (n < 128 ? [n] : n < 256 ? [0x81, n] : [0x82, n >> 8, n & 0xff]);
async function appToken(env, api, repo) {
	let pem = env.APP_PRIVATE_KEY.trim();
	if (!pem.includes("BEGIN")) pem = atob(pem); // (also taken as base64 of the .pem: one line)
	let der = Uint8Array.from(atob(pem.replace(/-----[^-]+-----/g, "").replace(/\s+/g, "")), (c) => c.charCodeAt(0));
	if (pem.includes("BEGIN RSA PRIVATE KEY")) { // (GitHub's: PKCS#1, wrapped as PKCS#8 for WebCrypto)
		const algorithm = [0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00];
		const inner = [0x02, 0x01, 0x00, ...algorithm, 0x04, ...derLength(der.length)];
		der = Uint8Array.from([0x30, ...derLength(inner.length + der.length), ...inner, ...der]);
	}
	const key = await crypto.subtle.importKey("pkcs8", der, { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" }, false, ["sign"]);
	const now = Math.floor(Date.now() / 1000);
	const text = (o) => b64url(new TextEncoder().encode(JSON.stringify(o)));
	const unsigned = `${text({ alg: "RS256", typ: "JWT" })}.${text({ iat: now - 60, exp: now + 540, iss: String(env.APP_ID) })}`;
	const jwt = `${unsigned}.${b64url(await crypto.subtle.sign("RSASSA-PKCS1-v1_5", key, new TextEncoder().encode(unsigned)))}`;
	const headers = { Accept: "application/vnd.github+json", Authorization: `Bearer ${jwt}`, "User-Agent": "spikeout-lobby" };
	const installation = await fetch(`${api}/repos/${repo}/installation`, { headers });
	if (!installation.ok) throw new Error(`the app isn't installed on ${repo} (${installation.status})`);
	const { id } = await installation.json();
	const made = await fetch(`${api}/app/installations/${id}/access_tokens`, { method: "POST", headers });
	if (!made.ok) throw new Error(`no token for the app (${made.status})`);
	return (await made.json()).token;
}

async function report(request, env) {
	const fail = (status, why) => Response.json({ why }, { status });
	if (request.method !== "POST") return fail(405, "a report is sent with POST");
	if (!env.REPORT_TOKEN && !(env.APP_ID && env.APP_PRIVATE_KEY)) return fail(503, "reports aren't set up on the lobby yet");
	const raw = await request.text();
	if (raw.length > REPORT_BYTES) return fail(413, "the report is too big");
	let r;
	try {
		r = JSON.parse(raw);
	} catch {
		return fail(400, "the report couldn't be read");
	}
	const comments = Array.isArray(r.comments) ? r.comments : [];
	const name = typeof r.github_name === "string" ? r.github_name.trim() : "";
	if (typeof r.title !== "string" || !r.title || r.title.length > 200 || typeof r.body !== "string" ||
		r.body.length > REPORT_TEXT || !r.body.includes(REPORT_MARK) || comments.length > REPORT_COMMENTS ||
		comments.some((c) => typeof c !== "string" || c.length > REPORT_TEXT))
		return fail(400, "that isn't a report from the game");
	if (name && !/^[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})$/.test(name)) return fail(400, "that isn't a GitHub name");
	const ip = request.headers.get("CF-Connecting-IP") || "local";
	const allowed = await env.REPORTS.getByName("reports").allow(ip, Date.now());
	if (!allowed) return fail(429, "too many reports from here: try again in an hour");

	const api = env.GITHUB_API || "https://api.github.com";
	const repo = env.REPORT_REPO || "tastyrepublic/spikeout-recomp";
	let token = env.REPORT_TOKEN;
	if (!token)
		try {
			token = await appToken(env, api, repo);
		} catch (e) {
			return fail(503, `the lobby can't reach GitHub as the app: ${e.message}`);
		}
	const github = (path, init = {}) =>
		fetch(api + path, {
			...init,
			headers: {
				Accept: "application/vnd.github+json",
				Authorization: `Bearer ${token}`,
				"User-Agent": "spikeout-lobby",
				"X-GitHub-Api-Version": "2022-11-28",
				...(init.body ? { "Content-Type": "application/json" } : {}),
			},
		});
	let by = "";
	if (name) {
		const user = await github(`/users/${encodeURIComponent(name)}`);
		if (user.status === 404) return fail(400, `there's no GitHub user called ${name}`);
		if (user.ok) by = `\n\nReported by @${(await user.json()).login} (the name given in the game, not checked).`;
	}
	const issue = await github(`/repos/${repo}/issues`, {
		method: "POST",
		body: JSON.stringify({ title: quiet(r.title), body: quiet(r.body) + by, labels: ["player report"] }),
	});
	if (issue.status !== 201) return fail(502, `GitHub didn't take the report (${issue.status})`);
	const made = await issue.json();
	for (const c of comments)
		await github(`/repos/${repo}/issues/${made.number}/comments`, { method: "POST", body: JSON.stringify({ body: quiet(c) }) });
	return Response.json({ number: made.number, url: made.html_url });
}

// The reports' counts by address (memory only: a restart forgets them, as the limit can).
export class Reports extends DurableObject {
	allow(ip, now) {
		this.hours ??= new Map();
		this.day ??= [];
		const mine = (this.hours.get(ip) || []).filter((t) => t > now - 3600 * 1000);
		this.day = this.day.filter((t) => t > now - 24 * 3600 * 1000);
		if (mine.length >= REPORTS_AN_HOUR || this.day.length >= REPORTS_A_DAY) return false;
		mine.push(now);
		this.hours.set(ip, mine);
		this.day.push(now);
		return true;
	}
}

export default {
	async fetch(request, env) {
		const url = new URL(request.url);
		if (url.pathname === "/v1/report") return report(request, env);
		if (url.pathname === "/v1/lobby") {
			if (request.method !== "GET" || request.headers.get("Upgrade") !== "websocket")
				return new Response("expected a WebSocket", { status: 426 });
			return env.LOBBY.getByName("lobby").fetch(request);
		}
		if (url.pathname === "/") return new Response("Spikeout lobby\n", { headers: { "Content-Type": "text/plain" } });
		return new Response("not found", { status: 404 });
	},
};

function text(v, max) {
	return typeof v === "string" ? v.slice(0, max) : "";
}

// A listed game's speed for the player asking, estimated (no traffic to the host): the round trip, ms, from
// how far apart Cloudflare puts them, light in fibre (200 km a ms) over a route half as long again, and 5 ms
// for the routers at each end. -1 when either place is unknown. (The real one shows once in the game.)
function estimate(a, b) {
	if (![a.lat, a.lon, b.lat, b.lon].every(Number.isFinite)) return -1;
	const rad = Math.PI / 180;
	const h = Math.sin(((b.lat - a.lat) * rad) / 2) ** 2 +
		Math.cos(a.lat * rad) * Math.cos(b.lat * rad) * Math.sin(((b.lon - a.lon) * rad) / 2) ** 2;
	const km = 2 * 6371 * Math.asin(Math.min(1, Math.sqrt(h)));
	return Math.round(5 + (2 * km * 1.5) / 200);
}

function candidates(list, seen) {
	const out = [];
	if (Array.isArray(list))
		for (const c of list.slice(0, MAX_CANDIDATES))
			if (typeof c === "string" && /^\d{1,3}(\.\d{1,3}){3}:\d{1,5}$/.test(c) && !out.includes(c)) out.push(c);
	if (seen && !out.includes(seen)) out.push(seen);
	return out;
}

export class Lobby extends DurableObject {
	constructor(ctx, env) {
		super(ctx, env);
		this.ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
	}

	async fetch(request) {
		const [client, server] = Object.values(new WebSocketPair());
		this.ctx.acceptWebSocket(server);
		server.serializeAttachment({
			ip: request.headers.get("CF-Connecting-IP") || "",
			country: (request.cf && request.cf.country) || "", // (Cloudflare's: two letters, shown with a listed game)
			lat: request.cf ? parseFloat(request.cf.latitude) : NaN, // (roughly where: a listed game's estimated speed)
			lon: request.cf ? parseFloat(request.cf.longitude) : NaN,
			name: "",
			build: "",
			game: null, // the game this player offers
		});
		return new Response(null, { status: 101, webSocket: client });
	}

	players() {
		return this.ctx.getWebSockets().map((ws) => ({ ws, a: ws.deserializeAttachment() || {} }));
	}

	findGame(code, build) {
		for (const p of this.players()) if (p.a.game && p.a.game.code === code && (build === null || p.a.build === build)) return p; // (null: any build)
		return null;
	}

	newCode() {
		for (;;) {
			const bytes = crypto.getRandomValues(new Uint8Array(CODE_LENGTH));
			const code = Array.from(bytes, (b) => CODE_LETTERS[b % CODE_LETTERS.length]).join("");
			if (!this.players().some((p) => p.a.game && p.a.game.code === code)) return code;
		}
	}

	async webSocketMessage(ws, message) {
		const send = (m) => ws.send(JSON.stringify(m));
		if (typeof message !== "string" || message.length > MAX_MESSAGE) return send({ t: "error", why: "message too big" });
		let m;
		try {
			m = JSON.parse(message);
		} catch {
			return send({ t: "error", why: "not JSON" });
		}
		const a = ws.deserializeAttachment() || {};
		const seen = a.ip && /^\d{1,3}(\.\d{1,3}){3}$/.test(a.ip) ? a.ip : ""; // (IPv4 only for now)

		switch (m.t) {
			case "hello":
				a.build = text(m.build, 32);
				a.name = text(m.name, 32);
				a.version = text(m.version, 32); // ("" from builds before 2026-11-05)
				ws.serializeAttachment(a);
				return send({ t: "welcome", address: a.ip, country: a.country, turn: Boolean(this.env.TURN_KEY_ID && this.env.TURN_KEY_API_TOKEN) });

			case "list": {
				// Those waiting for players first, then those in progress with an
				// empty cabinet; the nearest (estimate) first in each, then the newest. A page of LIST_PAGE from
				// `from`, and how many there are (the game asks for the next page).
				const open = [];
				let others = 0; // (public games on other versions: the game tells the player there are some)
				for (const p of this.players())
					if (p.a.game && p.a.game.public && p.a.build === a.build) // (in progress too; full ones to watch)
						open.push(p.a);
					else if (p.a.game && p.a.game.public) ++others;
				const near = (h) => { const e = estimate(a, h); return e < 0 ? 1e9 : e; }; // (nearest first; unknown last)
				const full = (g) => Number(g.taken >= g.seats);
				open.sort((x, y) => full(x.game) - full(y.game) || Number(x.game.started) - Number(y.game.started) || near(x) - near(y) || (y.game.created || 0) - (x.game.created || 0));
				const from = Math.max(0, Number(m.from) | 0);
				const games = open.slice(from, from + LIST_PAGE).map((h) => ({
					code: h.game.code,
					name: h.game.name,
					seats: h.game.seats,
					taken: h.game.taken,
					watchers: h.game.watchers || 0,
					started: h.game.started,
					session: h.game.session,
					country: h.country || "",
					ms: estimate(a, h),
				}));
				return send({ t: "games", games, from, total: open.length, others });
			}

			case "host": {
				if (!a.build) return send({ t: "error", why: "say hello first" });
				const seats = Math.min(4, Math.max(2, Number(m.seats) | 0));
				const port = Number(m.port) | 0;
				const session = text(m.session, 20);
				a.game = {
					code: a.game ? a.game.code : this.newCode(),
					name: text(m.name, 32) || a.name,
					seats,
					taken: 1,
					started: false,
					public: m.public !== false,
					session,
					created: Date.now(),
					candidates: candidates(m.candidates, seen && port ? `${seen}:${port}` : ""),
				};
				// Taking over a room whose host left its game in progress (a player
				// in it hosts it now): the same code, name, seats and listing, if the
				// session is that room's (only its players know it) and it's free.
				if (m.takeover) {
					const code = text(m.takeover, CODE_LENGTH).toUpperCase();
					const was = await this.ctx.storage.get(`room:${code}`);
					if (was && was.until > Date.now() && was.build === a.build && was.session === session && session &&
						!this.players().some((p) => p.a.game && p.a.game.code === code)) {
						Object.assign(a.game, { code, name: was.name, seats: was.seats, public: was.public, created: was.created });
						await this.ctx.storage.delete(`room:${code}`);
					}
				}
				ws.serializeAttachment(a);
				return send({ t: "hosted", code: a.game.code });
			}

			case "update":
				if (!a.game) return send({ t: "error", why: "no game offered" });
				a.game.taken = Math.min(a.game.seats, Math.max(1, Number(m.taken) | 0));
				a.game.started = Boolean(m.started);
				if (m.watchers !== undefined) a.game.watchers = Math.min(4, Math.max(0, Number(m.watchers) | 0));
				ws.serializeAttachment(a);
				return;

			case "unhost":
				a.game = null;
				ws.serializeAttachment(a);
				return;

			case "join": {
				const code = text(m.code, CODE_LENGTH).toUpperCase();
				const host = this.findGame(code, a.build);
				if (!host) {
					const elsewhere = this.findGame(code, null);
					if (!elsewhere) return send({ t: "error", why: "no game with that code" });
					const v = elsewhere.a.version;
					return send({ t: "error", why: v && v !== a.version ? `that game is on version ${v} (this is ${a.version || "older"})` : "that game is on another version", version: v || "" });
				}
				const g = host.a.game;
				if (g.taken >= g.seats && !m.watch) return send({ t: "error", why: "that game is full" });
				const port = Number(m.port) | 0;
				const mine = candidates(m.candidates, seen && port ? `${seen}:${port}` : "");
				host.ws.send(JSON.stringify({ t: "joiner", name: a.name, candidates: mine, country: a.country, watch: Boolean(m.watch) }));
				return send({ t: "joining", code, name: g.name, seats: g.seats, session: g.session, candidates: g.candidates, country: host.a.country });
			}

			case "turn": {
				if (!this.env.TURN_KEY_ID || !this.env.TURN_KEY_API_TOKEN) return send({ t: "error", why: "no relay here" });
				const r = await fetch(
					`https://rtc.live.cloudflare.com/v1/turn/keys/${this.env.TURN_KEY_ID}/credentials/generate-ice-servers`,
					{
						method: "POST",
						headers: { Authorization: `Bearer ${this.env.TURN_KEY_API_TOKEN}`, "Content-Type": "application/json" },
						body: JSON.stringify({ ttl: 86400 }),
					},
				);
				if (!r.ok) return send({ t: "error", why: `relay: ${r.status}` });
				const body = await r.json();
				return send({ t: "turn", iceServers: body.iceServers });
			}

			default:
				return send({ t: "error", why: "unknown message" });
		}
	}

	// A host gone: its game goes with its connection, but is remembered for a
	// while (ROOM_KEPT), for a player of a game in progress to take over.
	async remember(ws) {
		const a = ws.deserializeAttachment() || {};
		const g = a.game;
		if (g && g.started && g.code && g.session)
			await this.ctx.storage.put(`room:${g.code}`, {
				session: g.session, build: a.build, name: g.name, seats: g.seats, public: g.public, created: g.created,
				until: Date.now() + ROOM_KEPT,
			});
	}

	async webSocketClose(ws, code, reason) {
		await this.remember(ws);
		ws.close(code, reason); // (its game, in its attachment, goes with it)
	}

	async webSocketError(ws) {
		await this.remember(ws);
		ws.close(1011, "error");
	}
}
