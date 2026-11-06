// ---------------------------------------------------------------------------
// audio: offline spectral envelope (port of node js/audio-env.mjs)
//
//   chamina-cpp audio <audio.(mp3|wav|m4a|...)> [-o out.json] [--fps 60]
//                     [--bands 64] [--lo 40] [--hi 14000]
//
// Output: {"fps":..,"bands":..,"frames":..,"lo":..,"hi":..,"data":[[..]]} in
// compact JSON.stringify form — exactly what the renderer's --audio-env
// reads. Decoding shells out to ffmpeg with the same argument set as the JS
// tool; the analysis reproduces the JS Float32Array rounding chain (f32
// storage, double arithmetic) so the JSON matches node byte for byte.
// ---------------------------------------------------------------------------

// ffmpeg decode → mono f32le @ rate, stdout read back as raw PCM. The 'b'
// matters on the popen fallback: without it CRT text mode mangles 0x0A bytes
// inside floats. ffmpeg's stderr passes through to ours; node captured it
// for its error text, we surface just the exit code.
static wstring utf8_wide(const string& s) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    wstring w(n > 0 ? (size_t)n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
#else
    (void)s;
    return wstring();
#endif
}

static vector<float> decode_audio(const string& input, unsigned rate) {
    string bytes;
#ifdef _WIN32
    // popen() runs the command through cmd with an ANSI(=ACP) command line:
    // with ACP 936 the UTF-8 input path fails conversion outright and _popen
    // returns NULL. Spawn ffmpeg directly with UTF-16 arguments and an
    // anonymous pipe for the PCM stream instead — correct on any codepage.
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) throw runtime_error("cannot create pipe");
    // The child inherits every inheritable handle, pipe ends included; our
    // read end must not leak in or EOF tracking breaks.
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE err_dup = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE),
                    GetCurrentProcess(), &err_dup, 0, TRUE, DUPLICATE_SAME_ACCESS);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;        // JS decodes with stdio 'ignore'
    si.hStdOutput = wr;
    si.hStdError = err_dup ? err_dup : GetStdHandle(STD_ERROR_HANDLE);
    wstring cmd = L"\"" + utf8_wide(find_ffmpeg()) +
                  L"\" -hide_banner -loglevel error -i \"" + utf8_wide(input) +
                  L"\" -f f32le -acodec pcm_f32le -ac 1 -ar " + to_wstring(rate) +
                  L" pipe:1";
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                             nullptr, &si, &pi);
    if (nul) CloseHandle(nul);
    if (err_dup) CloseHandle(err_dup);
    CloseHandle(wr);  // our copy must go or ReadFile never sees EOF
    if (!ok) {
        CloseHandle(rd);
        throw runtime_error("cannot start ffmpeg");
    }
    char buf[1 << 16];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got > 0) bytes.append(buf, got);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) throw runtime_error("ffmpeg exited " + to_string(code));
#else
    string cmd = "\"\"" + find_ffmpeg() + "\" -hide_banner -loglevel error -i \""
                 + input + "\" -f f32le -acodec pcm_f32le -ac 1 -ar "
                 + to_string(rate) + " pipe:1\"";
    FILE* pipe = popen(cmd.c_str(), "rb");
    if (!pipe) throw runtime_error("cannot start ffmpeg");
    vector<char> buf(1 << 16);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0) bytes.append(buf.data(), n);
    int rc = pclose(pipe);
    if (rc != 0) throw runtime_error("ffmpeg exited " + to_string(rc));
#endif
    bytes.resize(bytes.size() - bytes.size() % 4);  // JS: buf.length >> 2 drops the tail
    vector<float> samples(bytes.size() / 4);
    for (size_t i = 0; i < samples.size(); i++) memcpy(&samples[i], &bytes[i * 4], 4);
    return samples;
}

// In-place iterative radix-2 FFT over re/im of length 2^n. Direct port of the
// JS FFT in js/audio-env.mjs — re/im are Float32Array there, so every
// butterfly store rounds to f32 while the twiddle products stay double; keep
// exactly that mix to reproduce node's bit patterns.
static void fft_env(float* re, float* im, size_t n) {
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            swap(re[i], re[j]);
            swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = (-2.0 * M_PI) / (double)len;
        double wr = cos(ang), wi = sin(ang);
        size_t half = len >> 1;
        for (size_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (size_t k = 0; k < half; k++) {
                size_t a = i + k, l = a + half;
                double tr = (double)re[l] * cr - (double)im[l] * ci;
                double ti = (double)re[l] * ci + (double)im[l] * cr;
                re[l] = (float)((double)re[a] - tr);
                im[l] = (float)((double)im[a] - ti);
                re[a] = (float)((double)re[a] + tr);
                im[a] = (float)((double)im[a] + ti);
                double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

static int audio_main(int argc, char** argv) {
    const char* usage =
        "audio-env\n"
        "chamina-cpp audio <audio-file> [-o out.json] [--fps 60] [--bands 64]\n"
        "                  [--lo 40] [--hi 14000]\n";
    string input, out;
    double fps = 60, bands = 64, lo = 40, hi = 14000;
    try {
        auto next = [&](int& i) -> string {
            i++;
            if (i >= argc) throw runtime_error(string(argv[i - 1]) + " needs a value");
            return argv[i];
        };
        for (int i = 1; i < argc; i++) {
            string a = argv[i];
            if (a == "-o") out = next(i);
            else if (a == "--fps") fps = parse_num(next(i));
            else if (a == "--bands") bands = parse_num(next(i));
            else if (a == "--lo") lo = parse_num(next(i));
            else if (a == "--hi") hi = parse_num(next(i));
            else if (a == "-h" || a == "--help") {
                fputs(usage, stdout);
                return 0;
            } else if (!a.empty() && a[0] == '-')
                throw runtime_error("unknown flag " + a);
            else input = a;  // JS overwrites; the last positional wins
        }
        if (input.empty()) {
            fputs(usage, stdout);  // JS: console.log(USAGE) then exit 1
            return 1;
        }

        const unsigned RATE = 44100;
        vector<float> samples = decode_audio(input, RATE);

        const double hop = (double)RATE / fps;  // samples per frame
        const size_t N = 2048;
        // JS: frames = Math.max(0, Math.floor((samples.length - N) / hop)).
        // A NaN hop would slip past JS's `if (!frames)` guard and emit a
        // garbage file — we fail instead (error path only).
        double frames_d = floor(((double)samples.size() - (double)N) / hop);
        if (!(frames_d >= 1.0))
            throw runtime_error("audio too short (" + to_string(samples.size()) +
                                " samples)");
        long long frames = (long long)frames_d;

        // Log-spaced band edges across [lo, hi].
        size_t B = (size_t)bands;  // fractional bands crash node (RangeError); we truncate
        vector<long long> edges;
        for (size_t b = 0; b <= B; b++)
            edges.push_back(
                (long long)js_round(lo * pow(hi / lo, (double)b / (double)B)));
        double binHz = (double)RATE / (double)N;
        vector<double> bandBin;  // kept double: NaN here must poison the row to nulls
        for (size_t b = 0; b <= B; b++)
            bandBin.push_back(
                js_min((double)(N >> 1), js_max(1.0, js_round((double)edges[b] / binHz))));

        // Hann window, precomputed (Float32Array in JS → f32 rounding).
        vector<float> win(N);
        for (size_t i = 0; i < N; i++)
            win[i] = (float)(0.5 - 0.5 * cos((2.0 * M_PI * (double)i) / (double)(N - 1)));

        string body;
        body.reserve((size_t)frames * (2 + B * 8) + 64);
        body += "{\"fps\":";
        js_dtoa(fps, body);
        body += ",\"bands\":";
        js_dtoa(bands, body);
        body += ",\"frames\":";
        body += to_string(frames);  // Math.floor()'d → integer double
        body += ",\"lo\":";
        js_dtoa(lo, body);
        body += ",\"hi\":";
        js_dtoa(hi, body);
        body += ",\"data\":[";

        vector<float> re(N), im(N);
        const double DB_LO = -60;  // dB window floor
        for (long long f = 0; f < frames; f++) {
            long long off = (long long)js_round((double)f * hop);
            for (size_t i = 0; i < N; i++) {
                // JS reads past the end only in degenerate configs, where
                // undefined→NaN poisons the row to nulls; mirror that.
                double s = (size_t)off + i < samples.size()
                               ? (double)samples[(size_t)off + i] * (double)win[i]
                               : numeric_limits<double>::quiet_NaN();
                re[i] = (float)s;
                im[i] = 0.0f;
            }
            fft_env(re.data(), im.data(), N);
            body += f ? ",[" : "[";
            for (size_t b = 0; b < B; b++) {
                double v;
                double blo = bandBin[b], bhi = js_max(blo + 1.0, bandBin[b + 1]);
                if (isfinite(blo) && isfinite(bhi)) {
                    double sum = 0;
                    for (long long k = (long long)blo; k < (long long)bhi; k++)
                        sum += (double)re[k] * (double)re[k] + (double)im[k] * (double)im[k];
                    // Divide by N/2 (Hann coherent gain) so a full-scale sine
                    // peaks at 0 dB; without this every band of a loud master
                    // saturates the -60..0 window.
                    double mag = sqrt(sum / (bhi - blo)) / ((double)N / 2);
                    double db = 20.0 * log10(mag + 1e-9);
                    v = js_max(0.0, js_min(1.0, (db - DB_LO) / -DB_LO));
                } else {
                    v = numeric_limits<double>::quiet_NaN();
                }
                if (b) body += ',';
                js_dtoa(round3(v), body);
            }
            body += ']';
        }
        body += "]}";

        string out_path = out.empty() ? "target/chamina/audio_env.json" : out;
        {
            error_code ec;
            filesystem::path op = filesystem::u8path(out_path);
            if (op.has_parent_path()) filesystem::create_directories(op.parent_path(), ec);
            ofstream f(op, ios::binary);
            if (!f) throw runtime_error("cannot write " + out_path);
            f.write(body.data(), (streamsize)body.size());
            f.close();
            if (!f) throw runtime_error("cannot write " + out_path);
        }

        printf("audio-env: %s  frames=%lld bands=%zu %sfps\n", out_path.c_str(), frames,
               B, js_num_str(fps).c_str());
        return 0;
    } catch (const exception& e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

// ffmpeg child fed raw RGB frames on stdin. Windows: direct CreateProcessW
// with UTF-16 arguments — popen's ANSI(=ACP) command line breaks non-ASCII
// -o paths (and dies outright when ACP can't decode the UTF-8 bytes); stdout
// is discarded and stderr passes through. Elsewhere: popen with cmd's
// quoting workaround.
struct VideoPipe {
#ifdef _WIN32
    HANDLE wr = nullptr, proc = nullptr, thread = nullptr;
    bool start(unsigned w, unsigned h, unsigned fps, const string& out) {
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE rd = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return false;
        // Same inheritance trap: our write end must not leak into the child,
        // or it never sees EOF and waits forever after the last frame.
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, 0, nullptr);
        HANDLE err_dup = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE),
                        GetCurrentProcess(), &err_dup, 0, TRUE, DUPLICATE_SAME_ACCESS);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = rd;
        si.hStdOutput = nul;
        si.hStdError = err_dup ? err_dup : GetStdHandle(STD_ERROR_HANDLE);
        wstring cmd = L"\"" + utf8_wide(find_ffmpeg()) +
                      L"\" -y -hide_banner -loglevel error -f rawvideo -pix_fmt rgb24 -s " +
                      to_wstring(w) + L"x" + to_wstring(h) + L" -framerate " +
                      to_wstring(fps) + L" -i pipe:0 -c:v libx264 -preset medium "
                      L"-crf 18 -pix_fmt yuv420p -vf scale=trunc(iw/2)*2:trunc(ih/2)*2 "
                      L"-movflags +faststart \"" + utf8_wide(out) + L"\"";
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                 nullptr, &si, &pi);
        if (nul) CloseHandle(nul);
        if (err_dup) CloseHandle(err_dup);
        CloseHandle(rd);
        if (!ok) {
            CloseHandle(wr);
            wr = nullptr;
            return false;
        }
        proc = pi.hProcess;
        thread = pi.hThread;
        return true;
    }
    bool write(const void* p, size_t n) {
        const uint8_t* q = (const uint8_t*)p;
        while (n) {
            DWORD w = 0;
            size_t chunk = n < (size_t)1 << 20 ? n : (size_t)1 << 20;
            if (!WriteFile(wr, q, (DWORD)chunk, &w, nullptr) || w == 0) return false;
            q += w;
            n -= w;
        }
        return true;
    }
    int finish() {
        if (wr) CloseHandle(wr);  // EOF so ffmpeg can finalise the container
        wr = nullptr;
        if (!proc) return -1;
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        CloseHandle(thread);
        proc = thread = nullptr;
        return (int)code;
    }
#else
    FILE* f = nullptr;
    bool start(unsigned w, unsigned h, unsigned fps, const string& out) {
        // popen shells out through `cmd /c`, which eats the leading quote when
        // a line holds several — wrapping the whole command in one extra quote
        // pair keeps every inner quote intact.
        string cmd = "\"\"" + find_ffmpeg() + "\" -y -hide_banner -loglevel error -f rawvideo "
                     "-pix_fmt rgb24 -s " + to_string(w) + "x" + to_string(h) +
                     " -framerate " + to_string(fps) + " -i pipe:0 -c:v libx264 -preset medium "
                     "-crf 18 -pix_fmt yuv420p -vf scale=trunc(iw/2)*2:trunc(ih/2)*2 "
                     "-movflags +faststart \"" + out + "\"\"";
        f = popen(cmd.c_str(), "wb");
        return f != nullptr;
    }
    bool write(const void* p, size_t n) {
        fwrite(p, 1, n, f);
        return !ferror(f);
    }
    int finish() {
        if (!f) return -1;
        int rc = pclose(f);
        f = nullptr;
        return rc;
    }
#endif
};

#ifdef _WIN32
static vector<string> utf8_argv(int argc, char** argv) {
    vector<string> out;
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc != argc) {
        // Extremely unlikely; keep the raw argv rather than failing to start.
        if (wargv) LocalFree(wargv);
        for (int i = 0; i < argc; i++) out.push_back(argv[i]);
        return out;
    }
    for (int i = 0; i < wargc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        string s(n > 1 ? (size_t)n - 1 : 0, '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], n, nullptr, nullptr);
        out.push_back(move(s));
    }
    LocalFree(wargv);
    return out;
}
#endif

// ---------------------------------------------------------------------------
// Public API implementation. This section used to be render_main() + main()
// in the single-TU engine: the scene parsing / font / mesh assembly moved
// into engine_create, the still branch into engine_still_png and the video
// branch into engine_encode_video. Everything above this section is the
// untouched engine core (only the file header comment and main() were
// dropped).
// ---------------------------------------------------------------------------
namespace chamina {

struct Engine {
    Engine(const json& s, const json& m) : scene(s), meta(m), world(scene, meta) {}
    // scene must outlive the meshes: PreparedText::anims / char_times point
    // into it (same lifetime rule as the original single-TU render_main).
    json scene;
    json meta;   // root settings (texts erased) — what finish() reads
    World world;
    FontEngine fe;
    int w = 0, h = 0;
    unsigned fps = 0, total = 0;
};

Engine* engine_create(const char* scene_json, const char* audio_env_json,
                      char* err, int err_cap) {
    auto fail = [&](const char* msg) -> Engine* {
        if (err && err_cap > 0) snprintf(err, (size_t)err_cap, "%s", msg);
        return nullptr;
    };
    try {
        json scene = json::parse(scene_json ? scene_json : "");

        // Malformed / minimal scene compat: fill in what older or hand-made
        // scenes omit (defaults mirror gen's own flags) instead of hard-throwing.
        // Valid scenes never hit these paths. One warning, once.
        {
            bool warned = false;
            auto warn_once = [&]() {
                if (!warned) {
                    fprintf(stderr, "[cpp] warn: scene had missing/invalid fields — filled with defaults\n");
                    warned = true;
                }
            };
            if (!scene.contains("texts") || !scene["texts"].is_array()) {
                scene["texts"] = json::array();
                warn_once();
            }
            for (auto& t : scene["texts"]) {
                if (!t.is_object()) {
                    t = json::object();
                    warn_once();
                }
                auto ensure_text = [&](const char* k, const char* d) {
                    if (!t.contains(k) || !t[k].is_string()) {
                        t[k] = d;
                        warn_once();
                    }
                };
                ensure_text("text", "");
                if (!t.contains("position") || !t["position"].is_array() ||
                    t["position"].size() < 3 || !t["position"][0].is_number() ||
                    !t["position"][1].is_number() || !t["position"][2].is_number()) {
                    t["position"] = json::array({0.0, -0.4, 0.09});
                    warn_once();
                }
                if (!t.contains("size") || !t["size"].is_number()) {
                    t["size"] = 1.4;
                    warn_once();
                }
                if (!t.contains("alpha") || !t["alpha"].is_number()) {
                    t["alpha"] = 1.0;
                    warn_once();
                }
                if (!t.contains("animations") || !t["animations"].is_array()) {
                    t["animations"] = json::array();
                    warn_once();
                } else
                    for (auto& an : t["animations"])
                        if (!an.is_object()) {
                            an = json::object();
                            warn_once();
                        }
                if (!t.contains("material") || !t["material"].is_object()) {
                    t["material"] = json{{"color", "#ffffff"}, {"strength", 1.6}};
                    warn_once();
                } else {
                    auto& m = t["material"];
                    if (!m.contains("color") || !m["color"].is_string()) {
                        m["color"] = "#ffffff";
                        warn_once();
                    }
                    if (m.contains("outline") && !m["outline"].is_object()) {
                        m.erase("outline");  // malformed rim → treat as none
                        warn_once();
                    } else if (m.contains("outline") &&
                               (!m["outline"].contains("color") ||
                                !m["outline"]["color"].is_string())) {
                        m.erase("outline");
                        warn_once();
                    }
                }
            }
            if (!scene.contains("backgrounds") || !scene["backgrounds"].is_array())
                scene["backgrounds"] = json::array();
            for (auto& bg : scene["backgrounds"]) {
                if (!bg.is_object()) {
                    bg = json::object();
                    warn_once();
                }
                auto ensure_str = [&](const char* k, const char* d) {
                    if (!bg.contains(k) || !bg[k].is_string()) {
                        bg[k] = d;
                        warn_once();
                    }
                };
                // Neutral sky so a half-filled key stays visible rather than fatal.
                ensure_str("top", "#0a1020");
                ensure_str("bottom", "#05070f");
                ensure_str("accent", "#3a4a66");
                if (!bg.contains("at") || !bg["at"].is_number()) {
                    bg["at"] = 0.0;
                    warn_once();
                }
            }
        }

        // The authored scene carries its settings at the root (no meta wrapper).
        json meta = scene;
        meta.erase("texts");
        {
            bool fps_ok = meta.contains("fps") && meta["fps"].is_number() &&
                          meta["fps"].get<float>() > 0;
            bool dur_ok = meta.contains("duration") && meta["duration"].is_number();
            if (!fps_ok) {
                meta["fps"] = 30;
                fprintf(stderr, "[cpp] warn: scene fps missing/invalid — defaulting to 30\n");
            }
            if (!dur_ok) {
                meta["duration"] = 300;
                fprintf(stderr, "[cpp] warn: scene duration missing — defaulting to 300s\n");
            }
            if (!meta.contains("total_frames"))
                meta["total_frames"] =
                    (unsigned)ceil(meta.at("duration").get<float>() * meta.at("fps").get<float>());
        }
        // Older / minimal scenes may omit whole sections; normalise once so every
        // downstream .at() is safe.
        if (!meta.contains("fx")) meta["fx"] = json{{"enabled", false}};
        if (!meta.contains("bloom")) meta["bloom"] = json{{"enabled", false}};
        if (!meta.contains("dof")) meta["dof"] = json{{"enabled", false}};
        if (!meta.contains("backgrounds")) meta["backgrounds"] = json::array();
        if (!meta.contains("camera"))
            meta["camera"] = json{{"fov", 0.8726646f}, {"near", 0.01f}, {"far", 500.0f}};

        Engine* e = new Engine(scene, meta);

        TRACE = getenv("CPP_TRACE") != nullptr;
        if (audio_env_json && *audio_env_json) {
            json ej = json::parse(audio_env_json);
            e->world.env.fps = ej.value("fps", 60.0f);
            for (const auto& row : ej.at("data")) e->world.env.data.push_back(row.get<vector<float>>());
            e->world.has_env = true;
        }

        // fonts
        bool font_ok = false;
        for (const auto& f : meta.value("font_fallback", json::array())) {
            if (e->fe.open(f.get<string>())) {
                font_ok = true;
                break;
            }
        }
        if (!font_ok) {
            delete e;
            return fail("no usable font");
        }

        float duration = meta.at("total_frames").get<float>() / meta.at("fps").get<float>();
        (void)duration;
        e->world.prepare({});
        // cues/outro/first-lyric are computed in prepare() — they need env-independent
        // info only, fine.

        for (const auto& spec : e->scene.at("texts")) {
            PreparedText tx;
            tx.position = Vec3{spec.at("position")[0].get<float>(),
                               spec.at("position")[1].get<float>(),
                               spec.at("position")[2].get<float>()};
            tx.size = spec.at("size").get<float>();
            tx.alpha = spec.value("alpha", 1.0f);
            tx.anims = &spec.at("animations");
            tx.char_times = spec.contains("char_times") ? &spec.at("char_times") : nullptr;
            {
                // Fade anims define the block's visible span: first char becomes
                // visible at ~min(start_at), all fade out by max(fade_out_at +
                // fade_out). A ±5s margin absorbs char_times/stagger edges. If any
                // fade lacks fade_out_at the block stays visible indefinitely, so
                // the upper bound is left open. Conservative: worst case we
                // evaluate a block whose chars are all transparent — never the
                // reverse.
                bool has_fade = false, all_bounded = true;
                float lo = INFINITY, hi = -INFINITY;
                for (const auto& a : spec.at("animations")) {
                    if (a.value("type", "") != "fade") continue;
                    has_fade = true;
                    lo = min(lo, a.value("start_at", 0.0f));
                    auto fo_it = a.find("fade_out_at");
                    if (fo_it != a.end() && fo_it->is_number())
                        hi = max(hi, fo_it->get<float>() + a.value("fade_out", 0.45f));
                    else
                        all_bounded = false;
                }
                if (has_fade) {
                    tx.vis_from = lo - 5.0f;
                    if (all_bounded) tx.vis_to = hi + 5.0f;
                }
            }
            tx.neon = true;
            auto& mat = spec.at("material");
            hex_linear(mat.at("color").get<string>(), tx.color_lin);
            tx.strength = mat.value("strength", 1.0f);
            for (int k = 0; k < 3; k++) tx.color_lin[k] *= tx.strength;
            tx.has_outline = mat.contains("outline");
            if (tx.has_outline) {
                hex_linear(mat.at("outline").at("color").get<string>(), tx.outline_lin);
                tx.outline_strength = mat.at("outline").value("strength", 1.0f);
                for (int k = 0; k < 3; k++) tx.outline_lin[k] *= tx.outline_strength;
            }
            // layout (port of src/text/layout.rs)
            const string& text = spec.at("text").get<string>();
            float cursor = 0, max_asc = -1e30f, min_desc = 1e30f;
            struct Item {
                uint32_t cp;
                int glyph;
                float advance_em, center_x;
            };
            vector<Item> items;
            for (size_t i = 0; i < text.size();) {
                size_t nx;
                uint32_t cp = e->fe.codepoint(text, i, nx);
                i = nx;
                int glyph = e->fe.find_glyph(cp);
                float adv, lsb;
                e->fe.h_metrics(glyph, adv, lsb);
                max_asc = max(max_asc, e->fe.ascender);
                min_desc = min(min_desc, e->fe.descender);
                items.push_back({cp, glyph, adv, cursor + adv * 0.5f});
                cursor += adv;
            }
            float total_width = cursor;
            float line_center_y = (max_asc + min_desc) * 0.5f;
            float half = total_width * 0.5f;
            // fit_width (port of export::build)
            float size = tx.size;
            if (spec.contains("fit_width")) {
                float budget = spec.at("fit_width").get<float>();
                float w = total_width * size;
                if (w > budget && budget > 0 && w > 0) size *= budget / w;
            }
            tx.size = size;
            tx.total_width_em = total_width;
            float depth = spec.value("depth", 0.09f);
            const json* cts = tx.char_times;
            for (size_t gi = 0; gi < items.size(); gi++) {
                auto& it = items[gi];
                float cx_local = it.center_x - half;
                float cy_local = -line_center_y;
                bool empty = it.glyph == 0 || e->fe.glyph_empty(it.glyph);
                PreparedChar ch;
                ch.idx = gi;
                ch.lands_at = 0;
                for (const auto& a : spec.at("animations")) {
                    if (a.value("type", "") == "fade") {
                        float sa = a.value("start_at", 0.0f), stg = a.value("stagger", 0.04f);
                        if (cts && gi < cts->size() && (*cts)[gi].is_number())
                            ch.lands_at = (*cts)[gi].get<float>();
                        else ch.lands_at = sa + (float)gi * stg;
                    }
                }
                if (empty) {
                    // no mesh, but the char still occupies the index stream
                    tx.chars.push_back(move(ch));
                    continue;
                }
                if (TRACE) {
                    fprintf(stderr, "[trace] mesh glyph=%d\n", it.glyph);
                    fflush(stderr);
                }
                float org_x = 0, org_y = 0;
                if (!build_glyph_mesh(e->fe, it.glyph, depth, ch.mesh, org_x, org_y)) {
                    tx.chars.push_back(move(ch));
                    continue;
                }
                // base_position: position + R*((center - adv/2 + origin) * size)
                Vec3 local{(cx_local - it.advance_em * 0.5f + org_x) * size,
                           (cy_local + org_y) * size, 0};
                ch.base_position = tx.position + local;
                if (tx.has_outline) {
                    float ow = mat_outline_width(spec);
                    ch.hull = smooth_hull(ch.mesh, ow);
                }
                tx.chars.push_back(move(ch));
            }
            tx.hw = clamp(tx.total_width_em * tx.size * 0.5f, 1.0f, 9.0f);
            e->world.texts.push_back(move(tx));
        }
        // sparks need hw — rebuild after texts
        {
            uint32_t r = 0;
            for (size_t bi = 0; bi < e->world.texts.size(); bi++) {
                auto& tx = e->world.texts[bi];
                float cue = INFINITY;
                for (const auto& a : tx.anims->items()) {
                    const json& aj = a.value();
                    if (aj.value("type", "") == "fade") cue = aj.value("start_at", 0.0f);
                }
                if (!isfinite(cue)) continue;
                r = (uint32_t)(bi * 131 + 7);
                for (int k = 0; k < 14; k++) {
                    Sparks::P sp;
                    sp.pos = tx.position +
                             Vec3{(mulberry_f(r) * 2 - 1) * tx.hw * 1.05f,
                                  (mulberry_f(r) * 2 - 1) * 0.85f, (mulberry_f(r) - 0.5f) * 0.6f};
                    sp.cue = cue + mulberry_f(r) * 0.15f;
                    for (int c = 0; c < 3; c++) sp.color[c] = tx.color_lin[c];
                    sp.s0 = (float)M_PI * (0.15f + 0.7f * mulberry_f(r));
                    sp.s1 = 0.5f + mulberry_f(r) * 1.6f;
                    sp.s2 = 2.0f + mulberry_f(r) * 4.0f;
                    e->world.sparks.p.push_back(sp);
                }
            }
        }

        e->w = meta.at("width").get<int>();
        e->h = meta.at("height").get<int>();
        e->fps = meta.at("fps").get<unsigned>();
        e->total = meta.at("total_frames").get<unsigned>();
        if (err && err_cap > 0) err[0] = '\0';
        return e;
    } catch (const exception& ex) {
        return fail(ex.what());
    } catch (...) {
        return fail("unknown exception");
    }
}

void engine_destroy(Engine* e) { delete e; }

// Shared per-frame tail for the direct-API paths (render_rgb / still_png):
// render + audio-reactive flash + post chain. The flash formula is the
// video branch's, kept verbatim from render_main. encode_video keeps its
// own inline copy because it interleaves per-stage profiling between the
// stages (CPP_PROF), which this helper must not distort.
static void engine_render_frame(Engine* e, float t, vector<uint8_t>& rgb) {
    static Frame fr;
    e->world.render_frame_into(t, fr);
    auto& fx = e->meta.at("fx");
    float flash = 0;
    if (fx.value("enabled", false)) {
        float bands[64], bass, energy;
        e->world.env.sample(t, bands, bass, energy);
        float hit = hit_envelope(e->world.cues, t, fx.value("hit_duration", 0.35f));
        flash = fx.value("flash", 0.0f) * hit * (0.45f + 0.55f * energy);
    }
    finish(fr, e->meta, flash, rgb);  // fills in place, keeps rgb's capacity
}

void engine_render_rgb(Engine* e, float t, unsigned char* rgb) {
    if (!e || !rgb) return;
    size_t n = (size_t)e->w * (size_t)e->h * 3;
    try {
        static vector<uint8_t> buf;  // reused across frames (capacity retained)
        engine_render_frame(e, t, buf);
        if (n && buf.size() >= n) memcpy(rgb, buf.data(), n);
    } catch (const exception& ex) {
        fprintf(stderr, "error: %s\n", ex.what());
        if (n) memset(rgb, 0, n);
    } catch (...) {
        fprintf(stderr, "error: unknown exception\n");
        if (n) memset(rgb, 0, n);
    }
}

int engine_still_png(Engine* e, float t, const char* png_path) {
    if (!e) return 1;
    string png = png_path ? png_path : "";
    try {
        static vector<uint8_t> rgb;
        engine_render_frame(e, t, rgb);
        // stbi_write_png opens through the ANSI codepage and fails on Chinese
        // paths; rasterize to memory and write through a wide path instead.
        vector<uint8_t> png_bytes;
        auto png_sink = [](void* ctx, void* data, int size) {
            auto& buf = *(vector<uint8_t>*)ctx;
            const uint8_t* d = (const uint8_t*)data;
            buf.insert(buf.end(), d, d + size);
        };
        if (!stbi_write_png_to_func(png_sink, &png_bytes, e->w, e->h, 3, rgb.data(), e->w * 3)) {
            fprintf(stderr, "[cpp] ERROR: cannot encode %s\n", png.c_str());
            return 1;
        }
        {
            // Fresh checkouts have no out/ directory; create the parent so the
            // documented CLI invocations work without a manual mkdir.
            error_code ec;
            filesystem::path op = filesystem::u8path(png);
            if (op.has_parent_path()) filesystem::create_directories(op.parent_path(), ec);
            ofstream f(op, ios::binary);
            if (!f || !f.write((const char*)png_bytes.data(), (streamsize)png_bytes.size())) {
                fprintf(stderr, "[cpp] ERROR: cannot write %s\n", png.c_str());
                return 1;
            }
        }
        return 0;
    } catch (const exception& ex) {
        fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "error: unknown exception\n");
        return 1;
    }
}

int engine_encode_video(Engine* e, unsigned from, unsigned frames, const char* out_path) {
    if (!e) return 1;
    string out = out_path && *out_path ? out_path : "cpp_out.mp4";
    try {
        int w = e->w, h = e->h;
        unsigned fps = e->fps, total = e->total;
        unsigned count = frames ? frames : total - from;
        count = min(count, total - from);

        {
            // ffmpeg cannot create the output directory itself.
            error_code ec;
            filesystem::path op = filesystem::u8path(out);
            if (op.has_parent_path()) filesystem::create_directories(op.parent_path(), ec);
        }
        VideoPipe vpipe;
        if (!vpipe.start(w, h, fps, out)) {
            fprintf(stderr, "cannot start ffmpeg\n");
            return 1;
        }
        auto t0 = chrono::steady_clock::now();
        double prof_render = 0, prof_finish = 0;
        bool prof = getenv("CPP_PROF") != nullptr;
        for (unsigned i = 0; i < count; i++) {
            float t = (from + i) / (float)fps;
            auto tr = chrono::steady_clock::now();
            static Frame fr;
            e->world.render_frame_into(t, fr);
            auto tf = chrono::steady_clock::now();
            auto& fx = e->meta.at("fx");
            float flash = 0;
            if (fx.value("enabled", false)) {
                float bands[64], bass, energy;
                e->world.env.sample(t, bands, bass, energy);
                float hit = hit_envelope(e->world.cues, t, fx.value("hit_duration", 0.35f));
                flash = fx.value("flash", 0.0f) * hit * (0.45f + 0.55f * energy);
            }
            static vector<uint8_t> rgb;
            finish(fr, e->meta, flash, rgb);
            prof_render += chrono::duration<double>(tf - tr).count();
            prof_finish += chrono::duration<double>(chrono::steady_clock::now() - tf).count();
            if (!vpipe.write(rgb.data(), rgb.size())) {
                // A dead ffmpeg turns the pipe into a silent sink — surface it
                // instead of "done"-ing over a missing file.
                fprintf(stderr, "[cpp] ERROR: ffmpeg pipe broke at frame %u\n", i + 1);
                vpipe.finish();
                return 1;
            }
            if ((i + 1) % 60 == 0 || i + 1 == count) {
                double el = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
                fprintf(stderr, "[cpp] frame %u/%u  %.2f fps  elapsed %.1fs  eta %.0fs\n",
                        i + 1, count, (i + 1) / el, el, el / (i + 1) * (count - i - 1));
            }
        }
        int rc = vpipe.finish();
        double el = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
        if (rc != 0) {
            fprintf(stderr, "[cpp] ERROR: ffmpeg exited %d — output may be missing or bad\n", rc);
            return 1;
        }
        fprintf(stderr, "[cpp] done -> %s  %u frames  %.2f fps\n",
                out.c_str(), count, count / el);
        if (prof)
            fprintf(stderr, "[prof] render %.1fms/f  finish %.1fms/f\n",
                    prof_render / count * 1000, prof_finish / count * 1000);
        return 0;
    } catch (const exception& ex) {
        fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "error: unknown exception\n");
        return 1;
    }
}

int engine_width(const Engine* e) { return e ? e->w : 0; }
int engine_height(const Engine* e) { return e ? e->h : 0; }
unsigned engine_fps(const Engine* e) { return e ? e->fps : 0; }
unsigned engine_total_frames(const Engine* e) { return e ? e->total : 0; }

void set_trace(bool on) { TRACE = on; }

bool read_file(const char* path, std::string& out) {
    try {
        out = ::read_file(path ? path : "");
        return true;
    } catch (...) {
        out.clear();
        return false;
    }
}

char** argv_utf8(int argc, char** argv, int* out_argc) {
#ifdef _WIN32
    // One decode per program; a second call would invalidate the first
    // result (the storage is reassigned). Callers keep it for the program's
    // lifetime.
    static vector<string> hold;
    static vector<char*> ptrs;
    hold = ::utf8_argv(argc, argv);
    ptrs.clear();
    for (auto& s : hold) ptrs.push_back(&s[0]);
    ptrs.push_back(nullptr);
    if (out_argc) *out_argc = (int)hold.size();
    return ptrs.data();
#else
    if (out_argc) *out_argc = argc;
    return argv;
#endif
}

// CLI-shaped content tools — the exe is a thin wrapper around these.
int gen_main(int argc, char** argv) { return ::gen_main(argc, argv); }
int audio_main(int argc, char** argv) { return ::audio_main(argc, argv); }

} // namespace chamina

#endif // CHAMINA_IMPLEMENTATION

#endif // CHAMINA_H_INCLUDE
