// chamina_cli.cpp — thin command-line shell around the chamina.h library.
//
// The engine itself lives in chamina.h (stb-style single header); this TU
// only re-decodes the Windows command line, parses the CLI flags and calls
// the public API. Build:
//
//   g++ -O2 -std=c++17 -fopenmp -static -static-libgcc -static-libstdc++ \
//       -Icpp/vendor cpp/src/chamina_cli.cpp -o dist/chamina-cpp.exe -lshell32
//
// CLI (unchanged from the single-TU engine):
//   chamina-cpp <scene.json> [--audio-env f] -o out.mp4 [--from n] [--frames n]
//   chamina-cpp <scene.json> --still t --png f
//   chamina-cpp gen <song.lrc> -o scene.json [--help]
//   chamina-cpp audio <audio-file> -o audio_env.json

#define CHAMINA_IMPLEMENTATION
#include "chamina.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace std;

int main(int argc, char** argv) {
#ifdef _WIN32
    // cmd.exe hands argv over in the ANSI codepage; re-decode to UTF-8 so
    // Chinese paths and text flags survive into JSON (mirrors how node
    // receives the same bytes as UTF-16 → UTF-8).
    int nargc = argc;
    char** nargs = chamina::argv_utf8(argc, argv, &nargc);
    if (nargs) {
        argc = nargc;
        argv = nargs;
    }
#endif
    if (argc >= 2 && string(argv[1]) == "gen")
        return chamina::gen_main(argc - 1, argv + 1);
    if (argc >= 2 && string(argv[1]) == "audio")
        return chamina::audio_main(argc - 1, argv + 1);

    string input, audio_env_path, out = "cpp_out.mp4", png;
    float still = -1;
    unsigned from = 0, frames = 0;
    // One guard around flag parsing and the whole renderer: malformed scenes
    // or flags must surface a clean error (exit 1), never an uncaught
    // exception (which Windows reports as 0xC0000409-style crash codes).
    try {
        for (int i = 1; i < argc; i++) {
            string a = argv[i];
            auto next = [&]() { return string(argv[++i]); };
            if (a == "--audio-env") audio_env_path = next();
            else if (a == "-o" || a == "--out") out = next();
            else if (a == "--still") still = stof(next());
            else if (a == "--png") png = next();
            else if (a == "--from") from = stoul(next());
            else if (a == "--frames") frames = stoul(next());
            else if (!a.empty() && a[0] != '-') input = a;
        }
    } catch (const exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "error: unknown exception\n");
        return 1;
    }
    if (input.empty()) {
        fprintf(stderr, "usage: chamina-cpp <scene.json> [--audio-env f] -o out.mp4 "
                        "[--from n] [--frames n] [--still t --png f]\n"
                        "       chamina-cpp gen <song.lrc> -o scene.json [--help]\n"
                        "       chamina-cpp audio <audio-file> -o audio_env.json\n");
        return 1;
    }

    // scene/audio-env read through chamina::read_file: MinGW's
    // ifstream(char*) opens via the ANSI codepage and fails on Chinese paths.
    string scene_str, env_str;
    if (!chamina::read_file(input.c_str(), scene_str)) {
        fprintf(stderr, "error: cannot read %s\n", input.c_str());
        return 1;
    }
    if (!audio_env_path.empty() &&
        !chamina::read_file(audio_env_path.c_str(), env_str)) {
        fprintf(stderr, "error: cannot read %s\n", audio_env_path.c_str());
        return 1;
    }

    char err[512];
    chamina::Engine* e = chamina::engine_create(
        scene_str.c_str(),
        audio_env_path.empty() ? nullptr : env_str.c_str(),
        err, (int)sizeof err);
    if (!e) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    int rc;
    if (still >= 0) {
        rc = chamina::engine_still_png(e, still, png.c_str());
        if (rc == 0) printf("still t=%g -> %s\n", still, png.c_str());
    } else {
        rc = chamina::engine_encode_video(e, from, frames, out.c_str());
    }
    chamina::engine_destroy(e);
    return rc;
}
