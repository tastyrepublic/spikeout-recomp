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
	assert.equal((await host.next("welcome")).turn, false);
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
	const { games } = await guest.next("games");
	assert.deepEqual(games, [{ code, name: "Ann's game", seats: 2, taken: 1, started: false }]);

	guest.send({ t: "join", code: code.toLowerCase(), port: 47100, candidates: ["10.0.0.7:47100"] });
	const joining = await guest.next("joining");
	assert.equal(joining.session, "abcdef");
	assert.ok(joining.candidates.includes("192.168.1.5:47000"));
	assert.ok(!joining.candidates.includes("bad"));
	const joiner = await host.next("joiner");
	assert.equal(joiner.name, "Bo");
	assert.ok(joiner.candidates.includes("10.0.0.7:47100"));

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
	assert.deepEqual((await guest.next("games")).games, [{ code, name: "Ann's game", seats: 2, taken: 1, started: true }]);
	guest.send({ t: "join", code, port: 1, candidates: [] });
	assert.equal((await guest.next("joining")).session, "abcdef");
	host.send({ t: "update", taken: 2, started: true }); // full: off the list, not joinable
	guest.send({ t: "list" });
	assert.deepEqual((await guest.next("games")).games, []);
	guest.send({ t: "join", code, port: 1, candidates: [] });
	assert.equal((await guest.next("error")).why, "that game is full");

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
