// The Spikeout internet lobby (docs/netplay.md), a Cloudflare Worker with
// one Durable Object holding every open game. It only introduces players:
// the game itself goes between their machines (peer to peer, Cloudflare's
// TURN relay when their routers won't allow that).
//
// One WebSocket a player, JSON text messages, {"t": type, ...}:
//
//   hello  {build, name}                 -> welcome {address, turn}
//   list   {from}                        -> games {games: [{code, name, seats, taken, started, session, country}],
//                                                  from, total} (LIST_PAGE from `from`; waiting first, newest first)
//   host   {name, seats, public, session, port, candidates[, takeover: code]}
//                                        -> hosted {code}
//   update {taken, started}              (the host's game)
//   unhost {}
//   join   {code, port, candidates}      -> joining {code, name, seats, session, candidates}
//                                           and the host gets joiner {name, candidates}
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
				return send({ t: "welcome", address: a.ip, turn: Boolean(this.env.TURN_KEY_ID && this.env.TURN_KEY_API_TOKEN) });

			case "list": {
				// Those waiting for players first, then those in progress with an
				// empty cabinet; the newest first in each. A page of LIST_PAGE from
				// `from`, and how many there are (the game asks for the next page).
				const open = [];
				for (const p of this.players())
					if (p.a.game && p.a.game.public && p.a.game.taken < p.a.game.seats && p.a.build === a.build) // (in progress too: an empty cabinet)
						open.push(p.a);
				open.sort((x, y) => Number(x.game.started) - Number(y.game.started) || (y.game.created || 0) - (x.game.created || 0));
				const from = Math.max(0, Number(m.from) | 0);
				const games = open.slice(from, from + LIST_PAGE).map((h) => ({
					code: h.game.code,
					name: h.game.name,
					seats: h.game.seats,
					taken: h.game.taken,
					started: h.game.started,
					session: h.game.session,
					country: h.country || "",
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
				if (g.taken >= g.seats) return send({ t: "error", why: "that game is full" });
				const port = Number(m.port) | 0;
				const mine = candidates(m.candidates, seen && port ? `${seen}:${port}` : "");
				host.ws.send(JSON.stringify({ t: "joiner", name: a.name, candidates: mine }));
				return send({ t: "joining", code, name: g.name, seats: g.seats, session: g.session, candidates: g.candidates });
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
