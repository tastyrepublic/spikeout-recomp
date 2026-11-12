// Rollback's guesses measured on recorded games (docs/netplay.md "Smarter guesses"): each seat's inputs in a network
// game's recording (NAME.inputs, tools/net_record.hpp) played as if they came L frames late, rollback as the game's
// (tools::InputTable: a frame's guess kept until its inputs come or it is gone back over; a wrong one: back to it, the
// frames after it guessed again from what has come). Per guesser: frames guessed wrong, goings back, frames run again.
//   guess-study [--latency L,L,...] FILE.inputs...
#include "input_guess.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using Inputs = tools::Recording::Inputs;

static bool same(const Inputs& a, const Inputs& b) {
    return a.bank0 == b.bank0 && a.bank1 == b.bank1 && a.controls == b.controls && a.controls2 == b.controls2;
}

struct Result {
    uint64_t wrong = 0, backs = 0, again = 0, frames = 0;
};

// kind 0: held last; 1: learned run lengths.
static Result run(const std::vector<Inputs>& in, uint64_t L, int kind) {
    Result r;
    const uint64_t n = in.size();
    std::vector<Inputs> guess(n);
    tools::InputGuess learn;
    uint64_t known = 0; // (frames come: 0..known-1)
    auto make = [&](uint64_t frame) { // (guessed from what has come)
        if (frame < known) return in[frame];
        if (known == 0) return Inputs{};
        if (kind == 0) return in[known - 1];
        return learn.guess(frame - (known - 1));
    };
    for (uint64_t t = 0; t < n; ++t) {
        guess[t] = make(t); // (frame t run: guessed)
        if (t >= L) { // (frame t-L comes)
            const uint64_t a = t - L;
            learn.add(in[a]);
            known = a + 1;
            if (!same(guess[a], in[a])) {
                ++r.wrong, ++r.backs, r.again += t - a;
                for (uint64_t x = a + 1; x <= t; ++x) guess[x] = make(x);
            }
        }
        ++r.frames;
    }
    return r;
}

int main(int argc, char** argv) {
    std::vector<uint64_t> lats = {2, 4, 6, 8};
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--latency") && i + 1 < argc) {
            lats.clear();
            for (char* p = argv[++i]; *p;) {
                lats.push_back(std::strtoull(p, &p, 10));
                if (*p == ',') ++p;
            }
        } else
            files.push_back(argv[i]);
    }
    std::map<std::pair<uint64_t, int>, Result> total;
    for (const auto& f : files) {
        std::ifstream s(f, std::ios::binary);
        char magic[8];
        uint32_t version = 0, boards = 0;
        uint64_t first = 0;
        s.read(magic, 8), s.read(reinterpret_cast<char*>(&version), 4), s.read(reinterpret_cast<char*>(&boards), 4),
            s.read(reinterpret_cast<char*>(&first), 8);
        if (!s || std::memcmp(magic, "SPKNETIN", 8) || boards == 0 || boards > 4) {
            std::printf("%s: not a recording\n", f.c_str());
            continue;
        }
        std::vector<std::vector<Inputs>> seats(boards);
        for (;;) {
            uint8_t b[16];
            if (!s.read(reinterpret_cast<char*>(b), 4 * boards)) break;
            for (uint32_t k = 0; k < boards; ++k) seats[k].push_back(Inputs{b[4 * k], b[4 * k + 1], b[4 * k + 2], b[4 * k + 3]});
        }
        for (uint32_t k = 0; k < boards; ++k) {
            uint64_t changes = 0;
            for (std::size_t i = 1; i < seats[k].size(); ++i) changes += !same(seats[k][i], seats[k][i - 1]);
            std::printf("%s seat %u: %zu frames, %llu changes\n", f.c_str(), k + 1, seats[k].size(), (unsigned long long)changes);
            if (!changes) continue;
            for (uint64_t L : lats) {
                const Result a = run(seats[k], L, 0), b = run(seats[k], L, 1);
                std::printf("  late %2llu: held %6llu wrong, %6llu run again | learned %6llu wrong (%+.0f%%), %6llu run again (%+.0f%%)\n",
                            (unsigned long long)L, (unsigned long long)a.wrong, (unsigned long long)a.again, (unsigned long long)b.wrong,
                            a.wrong ? 100.0 * (double(b.wrong) - double(a.wrong)) / double(a.wrong) : 0.0, (unsigned long long)b.again,
                            a.again ? 100.0 * (double(b.again) - double(a.again)) / double(a.again) : 0.0);
                for (int kind = 0; kind < 2; ++kind) {
                    Result& t = total[{L, kind}];
                    const Result& x = kind ? b : a;
                    t.wrong += x.wrong, t.backs += x.backs, t.again += x.again, t.frames += x.frames;
                }
            }
        }
    }
    std::printf("all:\n");
    for (uint64_t L : lats) {
        const Result& a = total[{L, 0}];
        const Result& b = total[{L, 1}];
        std::printf("  late %2llu: held %llu wrong, %llu run again | learned %llu wrong (%+.1f%%), %llu run again (%+.1f%%)\n",
                    (unsigned long long)L, (unsigned long long)a.wrong, (unsigned long long)a.again, (unsigned long long)b.wrong,
                    a.wrong ? 100.0 * (double(b.wrong) - double(a.wrong)) / double(a.wrong) : 0.0, (unsigned long long)b.again,
                    a.again ? 100.0 * (double(b.again) - double(a.again)) / double(a.again) : 0.0);
    }
}
