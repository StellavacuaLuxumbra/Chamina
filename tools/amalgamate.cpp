// amalgamate.cpp — assemble chamina.h from the <=1000-line source chunks.
//
// The deliverable is still the single-header library chamina.h; the editable
// sources live in src/. Run this tool from the repository root:
//
//   g++ -O2 -std=c++17 tools/amalgamate.cpp -o dist/amalgamate.exe
//   dist/amalgamate.exe            # regenerate chamina.h
//   dist/amalgamate.exe --check    # verify chunks <=1000 and header current
//
// Every chunk is normalized to LF and gets exactly one trailing newline, so
// the generated header is stable across checkouts. Chunks must stay at or
// below kMaxLines; the build fails loudly if one grows past the limit.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Part {
    const char* path;
};

// Order matters: 00_api.h is the public API, 01_core.cpp opens the
// CHAMINA_IMPLEMENTATION block, 10_audio_engine.cpp closes it.
static const Part kParts[] = {
    {"src/00_api.h"},
    {"src/01_core.cpp"},
    {"src/02_draw.cpp"},
    {"src/03_post.cpp"},
    {"src/04_skydome_simd.cpp"},
    {"src/05_gen_core.cpp"},
    {"src/06_gen_layout.cpp"},
    {"src/07_gen_field.cpp"},
    {"src/08_gen_cards.cpp"},
    {"src/09_gen_cli.cpp"},
    {"src/10_audio_engine.cpp"},
};

static constexpr size_t kMaxLines = 1000;
static const char* kOut = "chamina.h";

static std::string read_all(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[amalgamate] cannot read %s\n", path);
        std::exit(1);
    }
    std::string s((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
    return s;
}

static void normalize(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n') continue;
            out.push_back('\n');
        } else {
            out.push_back(s[i]);
        }
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    out.push_back('\n');
    s.swap(out);
}

static size_t line_count(const std::string& s) {
    return (size_t)std::count(s.begin(), s.end(), '\n');
}

int main(int argc, char** argv) {
    bool check = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--check") check = true;
        else {
            std::fprintf(stderr, "usage: amalgamate [--check]\n");
            return 2;
        }
    }

    std::string out;
    bool bad = false;
    for (const Part& p : kParts) {
        std::string s = read_all(p.path);
        normalize(s);
        size_t n = line_count(s);
        std::printf("[amalgamate] %-28s %4zu lines\n", p.path, n);
        if (n > kMaxLines) {
            std::fprintf(stderr, "[amalgamate] ERROR: %s has %zu lines (max %zu)\n",
                         p.path, n, kMaxLines);
            bad = true;
        }
        out += s;
    }
    if (bad) return 1;

    if (check) {
        std::ifstream in(kOut, std::ios::binary);
        std::string cur((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
        if (cur != out) {
            std::fprintf(stderr, "[amalgamate] %s is stale; run without --check\n", kOut);
            return 1;
        }
        std::printf("[amalgamate] %s is current (%zu bytes)\n", kOut, out.size());
        return 0;
    }

    const std::string tmp = std::string(kOut) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::fprintf(stderr, "[amalgamate] cannot write %s\n", tmp.c_str());
            return 1;
        }
        f.write(out.data(), (std::streamsize)out.size());
        if (!f) {
            std::fprintf(stderr, "[amalgamate] write failed for %s\n", tmp.c_str());
            return 1;
        }
    }
    std::error_code ec;
    fs::remove(kOut, ec);
    fs::rename(tmp, kOut, ec);
    if (ec) {
        std::fprintf(stderr, "[amalgamate] rename failed: %s\n", ec.message().c_str());
        return 1;
    }
    std::printf("[amalgamate] wrote %s (%zu lines, %zu bytes)\n", kOut,
                line_count(out), out.size());
    return 0;
}
