// Minimal chamina.h embedder — a host project that renders through the
// library, no CLI involved. One TU owns the implementation:
//
//   g++ -O2 -std=c++17 -fopenmp -static -static-libgcc -static-libstdc++ \
//       -Icpp/vendor -Icpp/src cpp/example/main.cpp \
//       -o dist/chamina-example.exe -lshell32
//
//   dist\chamina-example.exe                       → example.png (t=20.35)
//   dist\chamina-example.exe scenes/bird.json target/chamina/audio_env.json example.png 20.35
#define CHAMINA_IMPLEMENTATION
#include "chamina.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace std;

int main(int argc, char** argv) {
    const char* scene_path = argc > 1 ? argv[1] : "scenes/bird.json";
    const char* env_path   = argc > 2 ? argv[2] : "target/chamina/audio_env.json";
    const char* out_path   = argc > 3 ? argv[3] : "example.png";
    float t = argc > 4 ? (float)atof(argv[4]) : 20.35f;

    string scene, env;
    if (!chamina::read_file(scene_path, scene)) {
        fprintf(stderr, "cannot read %s\n", scene_path);
        return 1;
    }
    bool have_env = env_path && *env_path && chamina::read_file(env_path, env);

    char err[512];
    chamina::Engine* e = chamina::engine_create(
        scene.c_str(), have_env ? env.c_str() : nullptr, err, (int)sizeof err);
    if (!e) {
        fprintf(stderr, "engine error: %s\n", err);
        return 1;
    }

    printf("engine: %dx%d @ %u fps, %u frames\n",
           chamina::engine_width(e), chamina::engine_height(e),
           chamina::engine_fps(e), chamina::engine_total_frames(e));

    // RGB24 straight into the app's own buffer (the bytes you would upload
    // to a GPU texture / SDL surface / Qt image).
    vector<unsigned char> rgb((size_t)chamina::engine_width(e) *
                              chamina::engine_height(e) * 3);
    chamina::engine_render_rgb(e, t, rgb.data());
    long long sum = 0;
    for (unsigned char v : rgb) sum += v;
    printf("rgb sum = %lld (nonzero = rendered)\n", sum);

    int rc = chamina::engine_still_png(e, t, out_path);
    if (rc == 0) printf("wrote %s\n", out_path);
    chamina::engine_destroy(e);
    return rc;
}
