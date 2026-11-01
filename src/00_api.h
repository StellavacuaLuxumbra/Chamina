// chamina.h — the whole Chamina engine as a single-header C++ library.
//
// GENERATED FILE: assembled by tools/amalgamate.cpp from src/*.cpp. Edit the
// chunks, not chamina.h; build.bat regenerates the header before compiling.
//
// The deliverable software: fonts (stb_truetype), layout, extruded-glyph
// meshes (earcut), animation/camera/backgrounds (ports of js/anim.mjs +
// src/animation.rs), software rasterizer, full audio visualisation and the
// post chain, plus the two content subcommands (gen: LRC→scene JSON;
// audio: track→spectrum envelope). Drop-in, stb-style:
//
//   #define CHAMINA_IMPLEMENTATION      // in exactly one .cpp
//   #include "chamina.h"
//
// Other translation units include the header for the API only. Compile the
// implementation TU with -fopenmp for multithreaded rendering (optional —
// the header also builds single-threaded); on Windows add -lshell32
// (CommandLineToArgvW). The vendored single-header dependencies
// (nlohmann/json.hpp, stb_truetype.h, mapbox/earcut.hpp, stb_image_write.h)
// are found through the include path (vendor in this repo).
//
// Minimal embedder:
//
//   #define CHAMINA_IMPLEMENTATION
//   #include "chamina.h"
//
//   int main() {
//     std::string scene, env;
//     chamina::read_file("scenes/bird.json", scene);
//     chamina::read_file("target/chamina/audio_env.json", env);
//     char err[512];
//     auto* e = chamina::engine_create(scene.c_str(), env.c_str(), err, sizeof err);
//     if (!e) { fprintf(stderr, "error: %s\n", err); return 1; }
//     std::vector<unsigned char> rgb((size_t)chamina::engine_width(e) *
//                                    chamina::engine_height(e) * 3);
//     chamina::engine_render_rgb(e, 8.0f, rgb.data());    // RGB24, sRGB, post applied
//     chamina::engine_still_png(e, 20.35f, "frame.png");  // or straight to PNG
//     chamina::engine_encode_video(e, 0, 0, "out.mp4");   // 0 frames = to the end
//     chamina::engine_destroy(e);
//   }
//
// Scene JSON contract: whatever js/lrc2scene.mjs / `chamina-cpp gen` emits
// (fields at the root, no meta wrapper). The audio-env JSON is optional;
// without it the spectrum ring/bars/sparks/呼吸 components stay off.
// Not thread-safe across engines: frame buffers are static by design
// (parity with the original single-TU engine).
#ifndef CHAMINA_H_INCLUDE
#define CHAMINA_H_INCLUDE

#include <cstddef>
#include <cstdint>
#include <string>

namespace chamina {

// Opaque engine: parsed scene + fonts + prepared meshes.
struct Engine;

// Build an engine from UTF-8 JSON strings. audio_env_json may be null/empty.
// On failure returns nullptr and fills err (when err_cap > 0).
Engine* engine_create(const char* scene_json, const char* audio_env_json,
                      char* err, int err_cap);
void engine_destroy(Engine*);

// Render frame at time t (seconds) into rgb: width*height*3 bytes, RGB,
// full post chain applied. Never throws; on internal error the buffer is
// zeroed and a message goes to stderr.
void engine_render_rgb(Engine*, float t, unsigned char* rgb);

// Render + encode a PNG at a UTF-8 path. Returns 0 on success, 1 on error.
int engine_still_png(Engine*, float t, const char* png_path);

// Encode frames [from, from+frames) at the scene fps into an mp4 via the
// ffmpeg pipe (bundled installer / CHAMINA_FFMPEG / PATH). frames == 0
// means "until the end". Returns 0 on success, 1 on error. Progress goes
// to stderr; CPP_PROF=1 keeps the per-stage profiling report.
int engine_encode_video(Engine*, unsigned from, unsigned frames,
                        const char* out_path);

int engine_width(const Engine*);
int engine_height(const Engine*);
unsigned engine_fps(const Engine*);
unsigned engine_total_frames(const Engine*);

// Per-frame trace prints (also enabled by env CPP_TRACE=1 at engine_create).
void set_trace(bool on);

// Windows-codepage-proof file read (UTF-8 in, Chinese paths fine).
// Returns false and leaves out empty on failure.
bool read_file(const char* path, std::string& out);

// Windows: re-decode the process command line to UTF-8 and return a new
// argv (valid for the program's lifetime); other platforms pass through.
char** argv_utf8(int argc, char** argv, int* out_argc);

// CLI-shaped content tools — the exe is a thin wrapper around these.
// argv[0] is the subcommand token itself ("gen" / "audio"), exactly as the
// node tools see their own process.argv.slice(1).
int gen_main(int argc, char** argv);    // LRC → scene JSON
int audio_main(int argc, char** argv);  // audio → spectrum envelope JSON

} // namespace chamina
