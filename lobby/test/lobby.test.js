// The lobby, run on this machine (wrangler dev), driven by WebSocket
// clients as the game would: `npm test`.
import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { tmpdir } from "node:os";
import { join } from "node:path";

const PORT = 8790;
let dev;

before(async () => {
	dev = spawn("npx", ["wrangler", "dev", "--port", String(PORT), "--ip", "127.0.0.1", "--var", "ADMIN_TOKEN:test-admin", "--var", "REPORTS_KEPT:6000000", "--var", "REPORTS_A_MONTH:40", "--persist-to", join(tmpdir(), `spikeout-lobby-test-${Date.now()}`)], {
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
	host.send({ t: "hello", build: "b1", name: "Ann", version: "0.1.0" });
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
	assert.deepEqual(games, [{ code, name: "Ann's game", seats: 2, taken: 1, watchers: 0, started: false, stage: 0, area: 0, session: "abcdef" }]); // (the session: a game found nearby too shown once)

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

	const other = await player(); // another build sees nothing, joins nothing: told how many there are, and the version
	other.send({ t: "hello", build: "b2", name: "Di", version: "0.0.9" });
	await other.next("welcome");
	other.send({ t: "list" });
	const elsewhere = await other.next("games");
	assert.deepEqual(elsewhere.games, []);
	assert.equal(elsewhere.others, 1); // (Ann's; Cy's is private)
	other.send({ t: "join", code, port: 1, candidates: [] });
	assert.equal((await other.next("error")).why, "that game is on version 0.1.0 (this is 0.0.9)");
	other.send({ t: "join", code: secretCode, port: 1, candidates: [] }); // (a host from before versions were sent)
	assert.equal((await other.next("error")).why, "that game is on another version");
	other.send({ t: "join", code: "ZZZZZZ", port: 1, candidates: [] });
	assert.equal((await other.next("error")).why, "no game with that code");

	host.send({ t: "update", taken: 1, started: true, stage: 2, area: 3 }); // in progress (stage 2, the game's area 2), a cabinet empty: still listed, joinable
	guest.send({ t: "list" });
	assert.deepEqual(
		(await guest.next("games")).games.map(({ country, ms, ...g }) => g),
		[{ code, name: "Ann's game", seats: 2, taken: 1, watchers: 0, started: true, stage: 2, area: 3, session: "abcdef" }],
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

// Reports sent by the game itself (src/index.js): numbers as JSON, problem bundles only locked, the owner's list behind a
// key, and each address's day limited.
const BASE = `http://127.0.0.1:${PORT}`;
const ADMIN = { Authorization: "Bearer test-admin" };

test("reports: a game's numbers kept, read back with the owner's key only", async () => {
	const numbers = { version: "test", seats: 2, rollback: true, round_trips: [0, 140] };
	let r = await fetch(`${BASE}/v1/stats?version=test`, { method: "POST", body: JSON.stringify(numbers) });
	assert.equal(r.status, 200);
	r = await fetch(`${BASE}/v1/stats?version=test`, { method: "POST", body: "not json" });
	assert.equal(r.status, 400);
	r = await fetch(`${BASE}/v1/stats?version=test`, { method: "POST", body: "[1,2]" });
	assert.equal(r.status, 400);
	r = await fetch(`${BASE}/v1/stats?version=test`, { method: "POST", body: "x".repeat(9000) });
	assert.equal(r.status, 413);
	assert.equal((await fetch(`${BASE}/v1/admin/stats`)).status, 404); // (no key: as if nothing were there)
	assert.equal((await fetch(`${BASE}/v1/admin/stats`, { headers: { Authorization: "Bearer wrong" } })).status, 404);
	const rows = await (await fetch(`${BASE}/v1/admin/stats`, { headers: ADMIN })).json();
	const mine = rows.find((x) => x.version === "test");
	assert.ok(mine);
	assert.deepEqual(JSON.parse(mine.json), numbers);
	assert.equal(typeof mine.country, "string");
	assert.ok(!("who" in mine)); // (no address kept with it)
});

test("reports: a problem bundle kept only locked, and fetched back whole", async () => {
	const locked = new Uint8Array(200);
	locked.set(new TextEncoder().encode("SPKLOCK1"));
	for (let i = 8; i < locked.length; i++) locked[i] = i & 0xff;
	let r = await fetch(`${BASE}/v1/problem?version=test`, { method: "POST", body: locked });
	assert.equal(r.status, 200);
	const plain = new Uint8Array(200).fill(65); // (not locked: refused)
	r = await fetch(`${BASE}/v1/problem?version=test`, { method: "POST", body: plain });
	assert.equal(r.status, 400);
	const list = await (await fetch(`${BASE}/v1/admin/problems`, { headers: ADMIN })).json();
	assert.ok(list.length >= 1);
	const got = new Uint8Array(await (await fetch(`${BASE}/v1/admin/file/${encodeURIComponent(list.at(-1).key)}`, { headers: ADMIN })).arrayBuffer());
	assert.deepEqual(got, locked);
	assert.equal((await fetch(`${BASE}/v1/admin/file/${encodeURIComponent(list.at(-1).key)}`)).status, 404);
});

test("reports: one address's day limited", async () => {
	const statuses = [];
	for (let i = 0; i < 14; i++) {
		const locked = new Uint8Array(100);
		locked.set(new TextEncoder().encode("SPKLOCK1"));
		statuses.push((await fetch(`${BASE}/v1/problem?version=test`, { method: "POST", body: locked })).status);
	}
	assert.ok(statuses.includes(429)); // (12 a day: the earlier test's one, then these)
	assert.equal(statuses.filter((s) => s === 200).length, 11);
});

test("reports: a game's inputs kept only locked, listed apart from the bundles", async () => {
	const locked = new Uint8Array(300);
	locked.set(new TextEncoder().encode("SPKLOCK1"));
	for (let i = 8; i < locked.length; i++) locked[i] = (i * 7) & 0xff;
	assert.equal((await fetch(`${BASE}/v1/inputs?version=test`, { method: "POST", body: locked })).status, 200);
	assert.equal((await fetch(`${BASE}/v1/inputs?version=test`, { method: "POST", body: new Uint8Array(300) })).status, 400);
	const inputs = await (await fetch(`${BASE}/v1/admin/inputs`, { headers: ADMIN })).json();
	assert.equal(inputs.length, 1);
	assert.ok(inputs[0].key.startsWith("inputs/"));
	const problems = await (await fetch(`${BASE}/v1/admin/problems`, { headers: ADMIN })).json();
	assert.ok(problems.every((p) => p.key.startsWith("problems/")));
	const got = new Uint8Array(await (await fetch(`${BASE}/v1/admin/file/${encodeURIComponent(inputs[0].key)}`, { headers: ADMIN })).arrayBuffer());
	assert.deepEqual(got, locked);
	assert.equal((await fetch(`${BASE}/v1/admin/file/${encodeURIComponent("other/x")}`, { headers: ADMIN })).status, 404);
});

test("reports: a 2.5 MB file fetched back whole", async () => {
	const big = new Uint8Array(2_500_000);
	big.set(new TextEncoder().encode("SPKLOCK1"));
	for (let i = 8; i < big.length; i++) big[i] = (i * 13 + (i >> 9)) & 0xff;
	assert.equal((await fetch(`${BASE}/v1/inputs?version=big`, { method: "POST", body: big })).status, 200);
	const inputs = await (await fetch(`${BASE}/v1/admin/inputs`, { headers: ADMIN })).json();
	const mine = inputs.find((x) => x.version === "big");
	assert.equal(mine.size, big.length);
	const got = new Uint8Array(await (await fetch(`${BASE}/v1/admin/file/${encodeURIComponent(mine.key)}`, { headers: ADMIN })).arrayBuffer());
	assert.equal(got.length, big.length);
	assert.deepEqual(got, big);
});

test("reports: never past the cap: the oldest files let go (REPORTS_KEPT, 6 MB here; 8 GB live)", async () => {
	const before = [...(await (await fetch(`${BASE}/v1/admin/problems`, { headers: ADMIN })).json()),
		...(await (await fetch(`${BASE}/v1/admin/inputs`, { headers: ADMIN })).json())].sort((a, b) => a.id - b.id);
	const big = new Uint8Array(4_000_000);
	big.set(new TextEncoder().encode("SPKLOCK1"));
	assert.equal((await fetch(`${BASE}/v1/inputs?version=cap`, { method: "POST", body: big })).status, 200);
	const after = [...(await (await fetch(`${BASE}/v1/admin/problems`, { headers: ADMIN })).json()),
		...(await (await fetch(`${BASE}/v1/admin/inputs`, { headers: ADMIN })).json())];
	const total = after.reduce((t, f) => t + f.size, 0);
	assert.ok(total <= 6_000_000, `kept ${total} bytes`);
	assert.ok(after.some((f) => f.version === "cap")); // (the new one kept)
	assert.ok(!after.some((f) => f.id === before[0].id)); // (the oldest gone)
	assert.equal((await fetch(`${BASE}/v1/admin/file/${encodeURIComponent(before[0].key)}`, { headers: ADMIN })).status, 404); // (from the bucket too)
});

test("reports: R2's operations held under a monthly cap (REPORTS_A_MONTH, 40 here; 500,000 live)", async () => {
	const small = new Uint8Array(100);
	small.set(new TextEncoder().encode("SPKLOCK1"));
	let full = 0;
	for (let i = 0; i < 26 && !full; i++) {
		const r = await fetch(`${BASE}/v1/inputs?version=month`, { method: "POST", body: small });
		if (r.status === 503) full = (await r.text()).includes("full for this month") ? 1 : -1;
	}
	assert.equal(full, 1);
	const usage = await (await fetch(`${BASE}/v1/admin/usage`, { headers: ADMIN })).json();
	assert.ok(usage.r2a[0] <= 40, `class A ${usage.r2a[0]}`);
	assert.equal(usage.r2a[1], 40);
	assert.ok(usage.r2b[0] >= 1); // (the downloads above, counted)
	assert.match(usage.month, /^\d{4}-\d{2}$/);
});
