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

export default {
	async fetch(request, env) {
		const url = new URL(request.url);
		if (url.pathname === "/v1/lobby") {
			if (request.method !== "GET" || request.headers.get("Upgrade") !== "websocket")
				return new Response("expected a WebSocket", { status: 426 });
			return env.LOBBY.getByName("lobby").fetch(request);
		}
		if (url.pathname === "/") return new Response("Spikeout lobby\n", { headers: { "Content-Type": "text/plain" } });
		if (url.pathname === "/v1/stats" || url.pathname === "/v1/problem" || url.pathname === "/v1/inputs") return reportIn(request, env, url);
		if (url.pathname.startsWith("/v1/admin/")) return admin(request, env, url);
		return new Response("not found", { status: 404 });
	},
};

// ---- Reports sent by the game itself (the player's yes asked first: tools/problem_report.hpp, part 2) ----
//   POST /v1/stats?version=V    a network game's numbers, a JSON object (MAX_STATS bytes at most)
//   POST /v1/problem?version=V  a problem bundle, locked to the project owner's key ("SPKLOCK1...")
//   POST /v1/inputs?version=V   a network game's inputs (every seat's presses, frame by frame), locked the same way
// Locked ones kept in R2 (the REPORTS bucket, problems/ and inputs/; listed in the Reports database), unreadable without
// that key. Never past R2's free 10 GB: the oldest let go once KEPT (8 GB) is passed, the database adding up every
// file's size; and the bucket's own rule deletes anything older than 90 days whatever the count. Each kind a day from
// one address at most its LOCKED[kind].day, a file at most its .most (the address only counted, hashed: never kept).
// The owner's (a secret, ADMIN_TOKEN: `npx wrangler secret put ADMIN_TOKEN`; without it, none of these):
//   GET /v1/admin/stats?after=ID      the numbers, oldest first (ADMIN_PAGE a time)
//   GET /v1/admin/problems?after=ID   the bundles' list; /v1/admin/inputs?after=ID the inputs' list
//   GET /v1/admin/file/KEY            one of them (tools/fetch-reports.ps1 takes them, gamerun --unlock opens them)
const MAX_STATS = 8192;
const STATS_A_DAY = 60;
const LOCKED = {
	problem: { most: 24 << 20, day: 12, folder: "problems", ext: ".zip.locked" },
	inputs: { most: 8 << 20, day: 30, folder: "inputs", ext: ".inputs.locked" },
};
const ADMIN_PAGE = 500;
const KEPT = 8 * 1024 * 1024 * 1024;
// R2's free operations a month: 1,000,000 class A (a file stored, a list), 10,000,000 class B (a file read). Half of
// each at most (counted here, a month at a time: past it a file is refused, "full for this month", and the game keeps it
// to send another time; nothing is charged). Deletes (free on R2) counted with class A, to be safe.
const R2_A_MONTH = 500_000;
const R2_B_MONTH = 5_000_000;

async function reportIn(request, env, url) {
	if (request.method !== "POST") return new Response("POST only", { status: 405 });
	const kind = url.pathname.slice("/v1/".length); // stats, problem, inputs
	const version = text(url.searchParams.get("version"), 40);
	const body = new Uint8Array(await request.arrayBuffer());
	const locked = LOCKED[kind];
	if (!locked) {
		if (body.length === 0 || body.length > MAX_STATS) return new Response("too big", { status: 413 });
		let parsed;
		try {
			parsed = JSON.parse(new TextDecoder().decode(body));
		} catch {
			return new Response("not JSON", { status: 400 });
		}
		if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) return new Response("not an object", { status: 400 });
	} else {
		if (body.length < 80 || body.length > locked.most) return new Response("wrong size", { status: 413 });
		if (new TextDecoder().decode(body.slice(0, 8)) !== "SPKLOCK1") return new Response("not locked", { status: 400 });
		if (!env.REPORTS) return new Response("not kept here", { status: 503 });
	}
	const who = await hashed(request.headers.get("CF-Connecting-IP") || "");
	const country = (request.cf && request.cf.country) || "";
	const db = env.REPORTS_DB.getByName("reports");
	if (!(await db.allow(who, kind, locked ? locked.day : STATS_A_DAY))) return new Response("enough for today", { status: 429 });
	if (locked && !(await db.spend("r2a", Number(env.REPORTS_A_MONTH) || R2_A_MONTH, 1)))
		return new Response("full for this month", { status: 503 });
	if (!locked) {
		await db.addStats(version, country, new TextDecoder().decode(body));
	} else {
		const key = `${locked.folder}/${new Date().toISOString().slice(0, 10)}/${crypto.randomUUID()}${locked.ext}`;
		await env.REPORTS.put(key, body);
		const kept = Number(env.REPORTS_KEPT) || KEPT; // (tests: a smaller cap)
		for (const old of await db.addFile(kind, version, country, key, body.length, kept)) {
			await db.spend("r2a", Infinity, 1); // (counted, never refused: the cap must hold)
			await env.REPORTS.delete(old); // (past it: the oldest)
		}
	}
	return new Response("thanks\n", { headers: { "Content-Type": "text/plain" } });
}

async function hashed(address) {
	const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode("spikeout reports " + address));
	return [...new Uint8Array(d).slice(0, 12)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function admin(request, env, url) {
	const token = (env.ADMIN_TOKEN || "").trim(); // (a secret typed or piped in may end in a line break)
	if (!token || (request.headers.get("Authorization") || "").trim() !== `Bearer ${token}`) return new Response("not found", { status: 404 });
	const db = env.REPORTS_DB.getByName("reports");
	const after = Number(url.searchParams.get("after")) || 0;
	if (url.pathname === "/v1/admin/stats") return Response.json(await db.stats(after, ADMIN_PAGE));
	if (url.pathname === "/v1/admin/problems") return Response.json(await db.files("problem", after, ADMIN_PAGE));
	if (url.pathname === "/v1/admin/inputs") return Response.json(await db.files("inputs", after, ADMIN_PAGE));
	if (url.pathname === "/v1/admin/usage")
		return Response.json({ month: await db.month(), r2a: [await db.spent("r2a"), Number(env.REPORTS_A_MONTH) || R2_A_MONTH],
			r2b: [await db.spent("r2b"), Number(env.REPORTS_B_MONTH) || R2_B_MONTH], stored: await db.stored(), kept: Number(env.REPORTS_KEPT) || KEPT });
	if (url.pathname.startsWith("/v1/admin/file/") && env.REPORTS) {
		const key = decodeURIComponent(url.pathname.slice("/v1/admin/file/".length));
		const ok = Object.values(LOCKED).some((l) => key.startsWith(l.folder + "/"));
		if (ok && !(await db.spend("r2b", Number(env.REPORTS_B_MONTH) || R2_B_MONTH, 1))) return new Response("full for this month", { status: 503 });
		const o = ok ? await env.REPORTS.get(key) : null;
		if (!o) return new Response("not found", { status: 404 });
		return new Response(o.body, { headers: { "Content-Type": "application/octet-stream" } });
	}
	return new Response("not found", { status: 404 });
}

// What came: a database of its own (not the lobby's: its games' traffic never waits on it).
export class Reports extends DurableObject {
	constructor(ctx, env) {
		super(ctx, env);
		const sql = this.ctx.storage.sql;
		sql.exec(`CREATE TABLE IF NOT EXISTS stats (id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER, version TEXT, country TEXT, json TEXT)`);
		sql.exec(`CREATE TABLE IF NOT EXISTS files (id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT, at INTEGER, version TEXT, country TEXT, key TEXT, size INTEGER)`);
		sql.exec(`CREATE TABLE IF NOT EXISTS uses (who TEXT, kind TEXT, day TEXT, n INTEGER, PRIMARY KEY (who, kind, day))`);
		sql.exec(`CREATE TABLE IF NOT EXISTS months (month TEXT, kind TEXT, n INTEGER, PRIMARY KEY (month, kind))`);
	}
	allow(who, kind, most) {
		const sql = this.ctx.storage.sql;
		const day = new Date().toISOString().slice(0, 10);
		sql.exec(`DELETE FROM uses WHERE day < ?`, day); // (yesterday's counts: gone)
		const row = sql.exec(`SELECT n FROM uses WHERE who = ? AND kind = ? AND day = ?`, who, kind, day).toArray()[0];
		const n = row ? row.n : 0;
		if (n >= most) return false;
		sql.exec(`INSERT OR REPLACE INTO uses (who, kind, day, n) VALUES (?, ?, ?, ?)`, who, kind, day, n + 1);
		return true;
	}
	month() {
		return new Date().toISOString().slice(0, 7); // (UTC, as Cloudflare's billing month: 2026-11)
	}
	spent(kind) {
		const row = this.ctx.storage.sql.exec(`SELECT n FROM months WHERE month = ? AND kind = ?`, this.month(), kind).toArray()[0];
		return row ? row.n : 0;
	}
	// This month's count of `kind` up by `n`, unless that would pass `most` (false: not done).
	spend(kind, most, n) {
		const had = this.spent(kind);
		if (had + n > most) return false;
		this.ctx.storage.sql.exec(`INSERT OR REPLACE INTO months (month, kind, n) VALUES (?, ?, ?)`, this.month(), kind, had + n);
		return true;
	}
	stored() {
		return this.ctx.storage.sql.exec(`SELECT COALESCE(SUM(size), 0) AS t FROM files`).one().t;
	}
	addStats(version, country, json) {
		this.ctx.storage.sql.exec(`INSERT INTO stats (at, version, country, json) VALUES (?, ?, ?, ?)`, Date.now(), version, country, json);
	}
	addFile(kind, version, country, key, size, most) { // (the keys of those let go, past `most` bytes in all: oldest first)
		const sql = this.ctx.storage.sql;
		sql.exec(`INSERT INTO files (kind, at, version, country, key, size) VALUES (?, ?, ?, ?, ?, ?)`, kind, Date.now(), version, country, key, size);
		const gone = [];
		let total = sql.exec(`SELECT COALESCE(SUM(size), 0) AS t FROM files`).one().t;
		while (total > most) {
			const old = sql.exec(`SELECT id, key, size FROM files ORDER BY id LIMIT 1`).one();
			sql.exec(`DELETE FROM files WHERE id = ?`, old.id);
			gone.push(old.key);
			total -= old.size;
		}
		return gone;
	}
	stats(after, most) {
		return this.ctx.storage.sql.exec(`SELECT * FROM stats WHERE id > ? ORDER BY id LIMIT ?`, after, most).toArray();
	}
	files(kind, after, most) {
		return this.ctx.storage.sql.exec(`SELECT * FROM files WHERE kind = ? AND id > ? ORDER BY id LIMIT ?`, kind, after, most).toArray();
	}
}

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
