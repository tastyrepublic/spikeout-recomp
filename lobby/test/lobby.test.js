// The lobby, run on this machine (wrangler dev), driven by WebSocket
// clients as the game would: `npm test`.
import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { generateKeyPairSync, createVerify } from "node:crypto";

const PORT = 8794;        // (the lobby with a plain token)
const APP_PORT = 8795;    // (the lobby as a GitHub App: its private key)
const GITHUB_PORT = 8796; // (a GitHub of the tests' own: the reports' issues, kept here)
let dev, appDev, github;
const seen = []; // (what the fake GitHub was asked: {method, path, auth, body})
const users = new Set(["realplayer"]);
// (the app's key, as GitHub gives it: PKCS#1)
const { publicKey, privateKey } = generateKeyPairSync("rsa", {
	modulusLength: 2048,
	publicKeyEncoding: { type: "spki", format: "pem" },
	privateKeyEncoding: { type: "pkcs1", format: "pem" },
});
const jwtValid = (auth) => { // (signed with the app's key, by app 4242, not run out)
	const [h, p, s] = (auth || "").replace(/^Bearer /, "").split(".");
	if (!s) return false;
	const ok = createVerify("RSA-SHA256").update(`${h}.${p}`).verify(publicKey, Buffer.from(s, "base64url"));
	const claims = JSON.parse(Buffer.from(p, "base64url"));
	return ok && claims.iss === "4242" && claims.exp > Date.now() / 1000;
};

function startDev(port, vars) {
	const args = ["wrangler", "dev", "--port", String(port), "--ip", "127.0.0.1", "--persist-to", `.wrangler/test-${port}`, "--var", `GITHUB_API:http://127.0.0.1:${GITHUB_PORT}`];
	for (const v of vars) args.push("--var", v);
	const p = spawn("npx", args, { cwd: new URL("..", import.meta.url), shell: true, stdio: ["ignore", "pipe", "pipe"] });
	return [p, new Promise((resolve, reject) => {
		const timer = setTimeout(() => reject(new Error("wrangler dev didn't start")), 60000);
		const look = (chunk) => {
			if (/Ready on/.test(chunk.toString())) {
				clearTimeout(timer);
				resolve();
			}
		};
		p.stdout.on("data", look);
		p.stderr.on("data", look);
	})];
}

before(async () => {
	let issues = 0;
	github = createServer((req, res) => {
		let body = "";
		req.on("data", (d) => (body += d));
		req.on("end", () => {
			seen.push({ method: req.method, path: req.url, auth: req.headers.authorization, body: body ? JSON.parse(body) : null });
			const reply = (status, j) => (res.writeHead(status, { "Content-Type": "application/json" }), res.end(JSON.stringify(j)));
			if (req.url === "/repos/tastyrepublic/spikeout-recomp/installation")
				return jwtValid(req.headers.authorization) ? reply(200, { id: 77 }) : reply(401, { message: "bad JWT" });
			if (req.method === "POST" && req.url === "/app/installations/77/access_tokens")
				return jwtValid(req.headers.authorization) ? reply(201, { token: "installation-token" }) : reply(401, { message: "bad JWT" });
			const user = req.url.match(/^\/users\/([^/]+)$/);
			if (user) return users.has(user[1].toLowerCase()) ? reply(200, { login: "RealPlayer" }) : reply(404, { message: "Not Found" });
			if (req.method === "POST" && /\/issues$/.test(req.url)) {
				issues += 1;
				return reply(201, { number: 1000 + issues, html_url: `https://github.example/issues/${1000 + issues}` });
			}
			if (req.method === "POST" && /\/comments$/.test(req.url)) return reply(201, { id: 1 });
			reply(404, { message: "Not Found" });
		});
	}).listen(GITHUB_PORT, "127.0.0.1");
	let ready, appReady;
	[dev, ready] = startDev(PORT, ["REPORT_TOKEN:test-token"]);
	[appDev, appReady] = startDev(APP_PORT, ["APP_ID:4242", `APP_PRIVATE_KEY:${Buffer.from(privateKey).toString("base64")}`]);
	await Promise.all([ready, appReady]);
});

after(() => {
	for (const p of [dev, appDev])
		if (process.platform === "win32") spawn("taskkill", ["/pid", String(p.pid), "/T", "/F"]);
		else p.kill();
	github.close();
});

test("as the GitHub App: a token made from its key, the issue opened with it", async () => {
	seen.length = 0;
	const res = await fetch(`http://127.0.0.1:${APP_PORT}/v1/report`, {
		method: "POST",
		headers: { "Content-Type": "application/json", "CF-Connecting-IP": "10.1.0.1" },
		body: JSON.stringify({
			title: "As the app", comments: ["log"],
			body: "words\n\n_Sent from the game (Esc > Game > Report a problem)._\n",
		}),
	});
	assert.equal(res.status, 200, JSON.stringify(await res.clone().json()));
	const issue = seen.find((s) => s.method === "POST" && s.path.endsWith("/issues"));
	assert.equal(issue.auth, "Bearer installation-token");
	assert.ok(seen.some((s) => s.path.endsWith("/comments") && s.auth === "Bearer installation-token"));
});

// Bug reports: as the game sends them (src/net/report.cpp), to the fake GitHub.
const sendReport = (r, ip = "1.2.3.4") =>
	fetch(`http://127.0.0.1:${PORT}/v1/report`, {
		method: "POST",
		headers: { "Content-Type": "application/json", "CF-Connecting-IP": ip },
		body: JSON.stringify(r),
	});
const aReport = (more = {}) => ({
	title: "My pad isn't seen (0.1.0-preview3, Windows 11 Pro)",
	body: "It doesn't work, ask @someone\n\n_Sent from the game (Esc > Game > Report a problem). Its logs are in the comments below._\n",
	comments: ["**spikeout.log (this run)**\n\n```text\nhello\n```\n", "**settings.ini**\n\n```text\nvolume = 100\n```\n"],
	github_name: "",
	...more,
});

test("a report becomes an issue with its logs as comments, no one notified", async () => {
	seen.length = 0;
	const res = await sendReport(aReport(), "10.0.0.1");
	assert.equal(res.status, 200);
	const answer = await res.json();
	assert.ok(answer.number > 1000);
	const issue = seen.find((s) => s.method === "POST" && /\/repos\/tastyrepublic\/spikeout-recomp\/issues$/.test(s.path));
	assert.ok(issue);
	assert.equal(issue.auth, "Bearer test-token");
	assert.deepEqual(issue.body.labels, ["player report"]);
	assert.ok(issue.body.body.includes("@​someone")); // (the player's @: a mention of nobody)
	assert.ok(!issue.body.body.includes("Reported by"));
	const comments = seen.filter((s) => s.path.endsWith(`/issues/${answer.number}/comments`));
	assert.equal(comments.length, 2);
	assert.ok(comments[0].body.body.includes("spikeout.log"));
});

test("a report with a GitHub name mentions it; a name that doesn't exist is refused", async () => {
	seen.length = 0;
	const res = await sendReport(aReport({ github_name: "realplayer" }), "10.0.0.2");
	assert.equal(res.status, 200);
	const issue = seen.find((s) => s.method === "POST" && s.path.endsWith("/issues"));
	assert.ok(issue.body.body.includes("Reported by @RealPlayer"));
	const bad = await sendReport(aReport({ github_name: "nobody-here" }), "10.0.0.2");
	assert.equal(bad.status, 400);
	assert.match((await bad.json()).why, /no GitHub user called nobody-here/);
	const wrong = await sendReport(aReport({ github_name: "not a name!" }), "10.0.0.2");
	assert.equal(wrong.status, 400);
});

test("only the game's reports, not too big, a few an hour from one address", async () => {
	assert.equal((await sendReport({ title: "spam", body: "buy things", comments: [] }, "10.0.0.3")).status, 400);
	assert.equal((await sendReport(aReport({ comments: ["x".repeat(70000)] }), "10.0.0.3")).status, 400);
	assert.equal((await sendReport(aReport({ comments: ["a", "b", "c", "d", "e"] }), "10.0.0.3")).status, 400);
	const statuses = [];
	for (let i = 0; i < 6; i++) statuses.push((await sendReport(aReport(), "10.0.0.4")).status);
	assert.deepEqual(statuses, [200, 200, 200, 200, 200, 429]);
	assert.equal((await sendReport(aReport(), "10.0.0.5")).status, 200); // (another address: its own count)
	assert.equal((await fetch(`http://127.0.0.1:${PORT}/v1/report`)).status, 405);
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
