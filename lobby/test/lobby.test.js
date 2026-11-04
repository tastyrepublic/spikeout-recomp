// The lobby, run on this machine (wrangler dev), driven by WebSocket
// clients as the game would: `npm test`.
import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";

const PORT = 8790;
let dev;

before(async () => {
	dev = spawn("npx", ["wrangler", "dev", "--port", String(PORT), "--ip", "127.0.0.1"], {
		cwd: new URL("..", import.meta.url),
		shell: true,
		stdio: ["ignore", "pipe", "pipe"],
	});
	await new Promise((resolve, reject) => {
		const timer = setTimeout(() => reject(new Error("wrangler dev didn't start")), 60000);
		const look = (chunk) => {
			if (/Ready on/.test(chunk.toString())) {
				clearTimeout(timer);
				resolve();
			}
		};
		dev.stdout.on("data", look);
		dev.stderr.on("data", look);
	});
});

after(() => {
	if (process.platform === "win32") spawn("taskkill", ["/pid", String(dev.pid), "/T", "/F"]);
	else dev.kill();
});

// A player: send, and wait for the next message of a type.
async function player() {
	const ws = new WebSocket(`ws://127.0.0.1:${PORT}/v1/lobby`);
	const inbox = [];
	const waiting = [];
	ws.addEventListener("message", (e) => {
		const m = e.data === "pong" ? { t: "pong" } : JSON.parse(e.data);
		const w = waiting.findIndex((x) => x.t === m.t);
		if (w >= 0) waiting.splice(w, 1)[0].resolve(m);
		else inbox.push(m);
	});
	await new Promise((resolve, reject) => {
		ws.addEventListener("open", resolve);
		ws.addEventListener("error", reject);
	});
	return {
		send: (m) => ws.send(typeof m === "string" ? m : JSON.stringify(m)),
		next: (t) =>
			new Promise((resolve, reject) => {
				const i = inbox.findIndex((m) => m.t === t);
				if (i >= 0) return resolve(inbox.splice(i, 1)[0]);
				const timer = setTimeout(() => reject(new Error(`no ${t}`)), 5000);
				waiting.push({ t, resolve: (m) => (clearTimeout(timer), resolve(m)) });
			}),
		close: () => ws.close(),
	};
}

test("a public game listed (in progress too, while a cabinet is empty), joined by its code; a private one only by its code", async () => {
	const host = await player();
	host.send({ t: "hello", build: "b1", name: "Ann" });
	const welcome = await host.next("welcome");
	assert.equal(welcome.turn, false);
	assert.equal(typeof welcome.country, "string"); // (Cloudflare's; "" here, run locally)
	host.send({ t: "host", name: "Ann's game", seats: 2, public: true, session: "abcdef", port: 47000, candidates: ["192.168.1.5:47000", "bad"] });
	const { code } = await host.next("hosted");
	assert.match(code, /^[A-HJ-NP-Z2-9]{6}$/);

	const secret = await player();
	secret.send({ t: "hello", build: "b1", name: "Cy" });
	await secret.next("welcome");
	secret.send({ t: "host", name: "Cy's", seats: 3, public: false, session: "123", port: 47001, candidates: [] });
	const secretCode = (await secret.next("hosted")).code;

	const guest = await player();
	guest.send({ t: "hello", build: "b1", name: "Bo" });
	await guest.next("welcome");
	guest.send({ t: "list" });
	const { games, total } = await guest.next("games");
	assert.equal(total, 1);
	for (const g of games) { // (Cloudflare's country and the estimate from where it puts them; whatever wrangler dev says)
		assert.equal(typeof g.country, "string"), delete g.country;
		assert.ok(Number.isInteger(g.ms) && g.ms >= -1), delete g.ms;
	}
	assert.deepEqual(games, [{ code, name: "Ann's game", seats: 2, taken: 1, watchers: 0, started: false, session: "abcdef" }]); // (the session: a game found nearby too shown once)

	guest.send({ t: "join", code: code.toLowerCase(), port: 47100, candidates: ["10.0.0.7:47100"] });
	const joining = await guest.next("joining");
	assert.equal(joining.session, "abcdef");
	assert.ok(joining.candidates.includes("192.168.1.5:47000"));
	assert.ok(!joining.candidates.includes("bad"));
	assert.equal(typeof joining.country, "string"); // (the host's, as the lobby has it)
	const joiner = await host.next("joiner");
	assert.equal(joiner.name, "Bo");
	assert.ok(joiner.candidates.includes("10.0.0.7:47100"));
	assert.equal(typeof joiner.country, "string"); // (the joiner's)

	guest.send({ t: "join", code: secretCode, port: 47100, candidates: [] }); // (by its code: yes)
	assert.equal((await guest.next("joining")).seats, 3);

	const other = await player(); // another build sees nothing, joins nothing
	other.send({ t: "hello", build: "b2", name: "Di" });
	await other.next("welcome");
	other.send({ t: "list" });
	assert.deepEqual((await other.next("games")).games, []);
	other.send({ t: "join", code, port: 1, candidates: [] });
	assert.equal((await other.next("error")).why, "no game with that code");

	host.send({ t: "update", taken: 1, started: true }); // in progress, a cabinet empty: still listed, joinable
	guest.send({ t: "list" });
	assert.deepEqual(
		(await guest.next("games")).games.map(({ country, ms, ...g }) => g),
		[{ code, name: "Ann's game", seats: 2, taken: 1, watchers: 0, started: true, session: "abcdef" }],
	);
	guest.send({ t: "join", code, port: 1, candidates: [] });
	assert.equal((await guest.next("joining")).session, "abcdef");
	assert.equal((await host.next("joiner")).watch, false);
	host.send({ t: "update", taken: 2, started: true, watchers: 1 }); // full: still listed (to watch), not joinable
	guest.send({ t: "list" });
	assert.deepEqual(
		(await guest.next("games")).games.map(({ code: c, taken, watchers }) => ({ c, taken, watchers })),
		[{ c: code, taken: 2, watchers: 1 }],
	);
	guest.send({ t: "join", code, port: 1, candidates: [] });
	assert.equal((await guest.next("error")).why, "that game is full");
	guest.send({ t: "join", code, port: 1, candidates: [], watch: true }); // a spectator: yes
	assert.equal((await guest.next("joining")).session, "abcdef");
	assert.equal((await host.next("joiner")).watch, true);

	guest.send("ping");
	await guest.next("pong");
	guest.send({ t: "turn" });
	assert.equal((await guest.next("error")).why, "no relay here"); // (no key set up here)

	secret.close(); // a host gone: its game too
	await new Promise((r) => setTimeout(r, 300));
	guest.send({ t: "join", code: secretCode, port: 1, candidates: [] });
	assert.equal((await guest.next("error")).why, "no game with that code");
	for (const p of [host, guest, other]) p.close();
});

test("the list: waiting games first, then those in progress, the newest first in each; 50 a page, and the total", async () => {
	const hosts = [];
	const host = async (name, started) => {
		const h = await player();
		h.send({ t: "hello", build: "order", name });
		await h.next("welcome");
		h.send({ t: "host", name, seats: 2, public: true, session: name, port: 1, candidates: [] });
		await h.next("hosted");
		if (started) h.send({ t: "update", taken: 1, started: true });
		hosts.push(h);
		await new Promise((r) => setTimeout(r, 15)); // (a later game: newer)
	};
	await host("old-waiting", false);
	await host("old-playing", true);
	await host("new-waiting", false);
	await host("new-playing", true);

	const guest = await player();
	guest.send({ t: "hello", build: "order", name: "Ed" });
	await guest.next("welcome");
	guest.send({ t: "list" });
	const first = await guest.next("games");
	assert.deepEqual(first.games.map((g) => g.name), ["new-waiting", "old-waiting", "new-playing", "old-playing"]);
	assert.equal(first.total, 4);

	for (let i = 0; i < 48; ++i) await host(`more-${i}`, true); // 52 in all: two pages
	guest.send({ t: "list" });
	const page1 = await guest.next("games");
	assert.equal(page1.total, 52);
	assert.equal(page1.from, 0);
	assert.equal(page1.games.length, 50);
	guest.send({ t: "list", from: 50 });
	const page2 = await guest.next("games");
	assert.equal(page2.from, 50);
	assert.deepEqual(page2.games.map((g) => g.name), ["new-playing", "old-playing"]); // (the oldest in progress, last)
	for (const h of [...hosts, guest]) h.close();
});

test("a game in progress whose host goes: a player of it takes it over under its code; others can't", async () => {
	const host = await player();
	host.send({ t: "hello", build: "takeover", name: "Ann" });
	await host.next("welcome");
	host.send({ t: "host", name: "Ann's room", seats: 3, public: true, session: "s3ss10n", port: 1, candidates: [] });
	const { code } = await host.next("hosted");
	host.send({ t: "update", taken: 2, started: true });
	await new Promise((r) => setTimeout(r, 100));
	host.close(); // (gone: the room remembered)
	await new Promise((r) => setTimeout(r, 300));

	const stranger = await player(); // (not of that room: a session of its own)
	stranger.send({ t: "hello", build: "takeover", name: "Cy" });
	await stranger.next("welcome");
	stranger.send({ t: "host", name: "Cy", seats: 2, session: "other", takeover: code, port: 1, candidates: [] });
	assert.notEqual((await stranger.next("hosted")).code, code);

	const bo = await player(); // a player of it, hosting it now
	bo.send({ t: "hello", build: "takeover", name: "Bo" });
	await bo.next("welcome");
	bo.send({ t: "host", name: "Bo", seats: 2, session: "s3ss10n", takeover: code, port: 1, candidates: [] });
	assert.equal((await bo.next("hosted")).code, code);
	bo.send({ t: "update", taken: 1, started: true });
	const guest = await player();
	guest.send({ t: "hello", build: "takeover", name: "Ed" });
	await guest.next("welcome");
	guest.send({ t: "list" });
	const g = (await guest.next("games")).games.find((x) => x.code === code);
	assert.ok(g);
	assert.equal(g.name, "Ann's room"); // (the room's name and seats, as it was)
	assert.equal(g.seats, 3);
	guest.send({ t: "join", code, port: 1, candidates: [] });
	assert.equal((await guest.next("joining")).session, "s3ss10n");
	for (const p of [stranger, bo, guest]) p.close();
});
