// The Spikeout internet lobby (docs/netplay.md), a Cloudflare Worker with
// one Durable Object holding every open game. It only introduces players:
// the game itself goes between their machines (peer to peer, Cloudflare's
// TURN relay when their routers won't allow that).
//
// One WebSocket a player, JSON text messages, {"t": type, ...}:
//
//   hello  {build, name}                 -> welcome {address, country, turn} (country: Cloudflare's two letters for the
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
// ROMs and version); private games only by their code.

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
		for (const p of this.players()) if (p.a.game && p.a.game.code === code && p.a.build === build) return p;
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
				ws.serializeAttachment(a);
				return send({ t: "welcome", address: a.ip, country: a.country, turn: Boolean(this.env.TURN_KEY_ID && this.env.TURN_KEY_API_TOKEN) });

			case "list": {
				// Those waiting for players first, then those in progress with an
				// empty cabinet; the nearest (estimate) first in each, then the newest. A page of LIST_PAGE from
				// `from`, and how many there are (the game asks for the next page).
				const open = [];
				for (const p of this.players())
					if (p.a.game && p.a.game.public && p.a.build === a.build) // (in progress too; full ones to watch)
						open.push(p.a);
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
				return send({ t: "games", games, from, total: open.length });
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
				if (!host) return send({ t: "error", why: "no game with that code" });
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
