// ---------------------------------------------------------------------------
// gen: standalone title/subtitle cards (buildTitleBlock)
// ---------------------------------------------------------------------------

struct TitleOpts {
    string text;
    double size = numeric_limits<double>::quiet_NaN();  // clampNum → 3
    double y = numeric_limits<double>::quiet_NaN();     // isfinite ? : 0.6
    double depth = numeric_limits<double>::quiet_NaN(); // clampNum → 0.12
    string color;
    double strength = numeric_limits<double>::quiet_NaN();
    double start_at = numeric_limits<double>::quiet_NaN();
    double out_at = numeric_limits<double>::quiet_NaN();
    double fade_in = numeric_limits<double>::quiet_NaN();  // clampNum → 0.9
    double fade_out = numeric_limits<double>::quiet_NaN(); // clampNum → 0.9
    double stagger = numeric_limits<double>::quiet_NaN();  // clampNum → 0.03
    string entrance;
    double fit_width = numeric_limits<double>::quiet_NaN();
    string outline;
    double outline_width = numeric_limits<double>::quiet_NaN();
};

static ojson build_title_block(const TitleOpts& o) {
    string text = js_trim(o.text);
    if (text.empty()) return ojson(nullptr);

    double size = clamp_num(o.size, 3, 0.2, 12);
    double y = isfinite(o.y) ? o.y : 0.6;
    double depth = clamp_num(o.depth, 0.12, 0.01, 2);
    string color = o.color.empty() ? "#ffffff" : o.color;
    double strength = clamp_num(o.strength, 1.6, 0, 64);
    double start_at = round3(js_max(0.0, isfinite(o.start_at) ? o.start_at : 0.0));
    double fade_in = clamp_num(o.fade_in, 0.9, 0, 10);
    double fade_out = clamp_num(o.fade_out, 0.9, 0, 10);
    double stagger = clamp_num(o.stagger, 0.03, 0, 1);
    bool has_out = isfinite(o.out_at);
    string entrance = o.entrance.empty() ? "none" : o.entrance;
    check_entrance(entrance);

    ojson anims = ojson::array();
    if (entrance == "slide") {
        ojson a = ojson::object();
        a["type"] = "slide";
        a["delta"] = ojson::array({0.0, -1.6, 0.0});
        a["duration"] = 1.2;
        a["stagger"] = stagger;
        a["ease"] = "ease_out_quad";
        a["start_at"] = start_at;
        anims.push_back(move(a));
    } else if (entrance == "fall") {
        ojson a = ojson::object();
        a["type"] = "fall";
        a["duration"] = 1.2;
        a["stagger"] = stagger;
        a["height"] = 1.8;
        a["gravity"] = -3.0;
        a["drift"] = 0.0;
        a["wobble"] = 0.0;
        a["start_at"] = start_at;
        anims.push_back(move(a));
    } else if (entrance == "pop") {
        ojson a = ojson::object();
        a["type"] = "pop";
        a["duration"] = 0.7;
        a["stagger"] = stagger;
        a["start_at"] = start_at;
        a["from"] = 0.0;
        a["ease"] = "back_out";
        anims.push_back(move(a));
    }
    ojson fade = ojson::object();
    fade["type"] = "fade";
    fade["duration"] = fade_in;
    fade["stagger"] = stagger;
    fade["start_at"] = start_at;
    if (has_out && o.out_at > start_at) {
        fade["fade_out_at"] = round3(o.out_at);
        fade["fade_out"] = fade_out;
    }
    anims.push_back(move(fade));

    ojson block = ojson::object();
    block["text"] = text;
    block["size"] = size;
    block["depth"] = depth;
    block["position"] = ojson::array({0.0, y, 0.0});
    ojson mat = ojson::object();
    mat["type"] = "neon";
    mat["color"] = color;
    mat["strength"] = strength;
    block["material"] = move(mat);
    block["alpha"] = 1.0;
    block["animations"] = move(anims);
    if (isfinite(o.fit_width) && o.fit_width > 0) block["fit_width"] = o.fit_width;
    if (!o.outline.empty()) {
        ojson outline = ojson::object();
        outline["color"] = o.outline;
        double w = isfinite(o.outline_width) && o.outline_width != 0 ? o.outline_width : 0.02;
        outline["width"] = w;
        outline["strength"] = 1.0;
        block["material"]["outline"] = move(outline);
    }
    return block;
}

// ---------------------------------------------------------------------------
// gen: named background looks + cuts (buildBackgrounds / parseCut)
// ---------------------------------------------------------------------------

struct Look {
    const char* top;
    const char* bottom;
    const char* accent;
    double nebula, stars, drift;
};
static const map<string, Look> LOOKS = {
    {"night", {"#01040f", "#05060f", "#2f6dff", 0.55, 0.75, 1.0}},
    {"void", {"#000000", "#02030a", "#1b2c6b", 0.30, 0.95, 0.6}},
    {"nebula", {"#0a0320", "#12062e", "#a24bff", 0.85, 0.65, 1.3}},
    {"aurora", {"#001a26", "#001014", "#00ffb3", 0.75, 0.50, 0.9}},
    {"ember", {"#1a0503", "#0a0201", "#ff6a1e", 0.70, 0.35, 1.1}},
    {"ice", {"#041224", "#02060e", "#8fe3ff", 0.45, 0.80, 0.8}},
    {"dawn", {"#2a0a3a", "#4a1420", "#ff9a3c", 0.60, 0.30, 1.0}},
    {"bloom", {"#1a0226", "#26043a", "#ff3ea5", 0.80, 0.50, 1.2}},
};

struct Cut {
    double at = 0;
    bool has_name = false;
    string name;
    bool has_palette = false;
    ojson palette;
};

static vector<string> js_split(const string& s, char sep) {
    vector<string> out;
    size_t pos = 0;
    while (true) {
        size_t f = s.find(sep, pos);
        if (f == string::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, f - pos));
        pos = f + 1;
    }
    return out;
}

// <seconds>:<look> or <seconds>:#top/#bottom/#accent[@n,s,d] → one cut.
static Cut parse_cut(const string& raw) {
    // A trailing ` # …` note is allowed; hexes never have whitespace before #.
    string line = raw;
    for (size_t i = 0; i + 1 < line.size(); i++)
        if (js_ws_at(line, i) && line[i + 1] == '#') {
            line = line.substr(0, i);
            break;
        }
    size_t ci = line.rfind(':');
    if (ci == string::npos)
        throw runtime_error("--bg-cut wants <seconds>:<look>, got " + raw);
    double at = parse_num(line.substr(0, ci));
    if (!isfinite(at))
        throw runtime_error("--bg-cut: bad time in " + raw);
    string spec = line.substr(ci + 1);
    Cut c;
    c.at = at;
    if (spec.find('/') == string::npos) {
        c.has_name = true;
        c.name = spec;
        return c;
    }
    auto parts = js_split(spec, '/');
    if (parts.size() != 3)
        throw runtime_error("--bg-cut palette wants top/bottom/accent, got " + raw);
    // The accent carries the sliders: `#4a6cff@0.55,0.85,1.0`. JS destructures
    // split('@'), so extra '@' components are dropped.
    string accent = parts[2], sliders;
    {
        size_t atpos = parts[2].find('@');
        if (atpos != string::npos) {
            accent = parts[2].substr(0, atpos);
            string rest = parts[2].substr(atpos + 1);
            size_t at2 = rest.find('@');
            sliders = at2 == string::npos ? rest : rest.substr(0, at2);
        }
    }
    for (const string* hex : {&parts[0], &parts[1], &accent})
        if (!is_hex6(*hex))
            throw runtime_error("--bg-cut: \"" + *hex + "\" is not #rrggbb in " + raw);
    c.has_palette = true;
    ojson palette = ojson::object();
    palette["top"] = parts[0];
    palette["bottom"] = parts[1];
    palette["accent"] = accent;
    palette["nebula"] = 0.6;
    palette["stars"] = 0.6;
    palette["drift"] = 1.0;
    if (!sliders.empty()) {
        auto sl = js_split(sliders, ',');
        // RATIO(v, lo, hi, fallback): Number("") is 0 — quirk preserved.
        auto ratio = [](double v, double lo, double hi, double fb) {
            if (!isfinite(v)) return fb;
            return min(hi, max(lo, v));
        };
        double n = sl.size() > 0 ? parse_num(sl[0]) : numeric_limits<double>::quiet_NaN();
        double st = sl.size() > 1 ? parse_num(sl[1]) : numeric_limits<double>::quiet_NaN();
        double d = sl.size() > 2 ? parse_num(sl[2]) : numeric_limits<double>::quiet_NaN();
        palette["nebula"] = ratio(n, 0, 1, 0.6);
        palette["stars"] = ratio(st, 0, 1, 0.6);
        palette["drift"] = ratio(d, 0, 4, 1.0);
    }
    c.palette = move(palette);
    return c;
}

// Turn --bg-look / --bg-cut into the background timeline the renderer
// crossfades through; null when neither flag was given.
static ojson build_backgrounds(const vector<string>& bg_cut, const string& bg_look,
                               double bg_fade) {
    struct Entry {
        double at;
        bool has_name;
        string name;
        bool has_palette;
        ojson palette;
    };
    vector<Entry> cuts;
    if (!bg_look.empty()) cuts.push_back({0.0, true, bg_look, false, {}});
    for (const string& raw : bg_cut) {
        Cut c = parse_cut(raw);
        if (c.has_palette)
            cuts.push_back({c.at, false, "", true, move(c.palette)});
        else
            cuts.push_back({c.at, true, c.name, false, {}});
    }
    if (cuts.empty()) return ojson(nullptr);
    // A first cut with no opener still needs something to cut *from*.
    if (cuts[0].at > 0) cuts.insert(cuts.begin(), {0.0, true, string("night"), false, {}});
    for (const Entry& c : cuts)
        if (!c.has_palette && !LOOKS.count(c.name))
            throw runtime_error("unknown look \"" + c.name +
                                "\" (have: night, void, nebula, aurora, ember, ice, dawn, bloom)");
    stable_sort(cuts.begin(), cuts.end(),
                [](const Entry& a, const Entry& b) { return a.at < b.at; });
    double fade = isfinite(bg_fade) ? min(60.0, max(0.0, bg_fade)) : 1.5;
    ojson arr = ojson::array();
    for (size_t i = 0; i < cuts.size(); i++) {
        const Entry& c = cuts[i];
        ojson e = ojson::object();
        e["at"] = i == 0 ? 0.0 : c.at;
        e["fade"] = i == 0 ? 0.0 : fade;
        if (c.has_palette) {
            for (auto& kv : c.palette.items()) e[kv.key()] = kv.value();
        } else {
            const Look& L = LOOKS.at(c.name);
            e["top"] = L.top;
            e["bottom"] = L.bottom;
            e["accent"] = L.accent;
            e["nebula"] = L.nebula;
            e["stars"] = L.stars;
            e["drift"] = L.drift;
        }
        arr.push_back(move(e));
    }
    return arr;
}

// The grade/distortion rack, or nothing so the scene renders untouched.
static ojson build_fx(const OptNum& fx_chromatic, const OptNum& fx_vignette,
                      const OptNum& fx_sat, const OptNum& fx_flash, const OptNum& fx_punch) {
    ojson fx = ojson::object();
    fx["enabled"] = true;
    fx["chromatic"] = opt_clamp(fx_chromatic, 0, 0.05, 0.0016);
    fx["vignette"] = opt_clamp(fx_vignette, 0, 1, 0.55);
    fx["saturation"] = opt_clamp(fx_sat, 0, 4, 1.12);
    fx["flash"] = opt_clamp(fx_flash, 0, 1, 0.22);
    fx["punch"] = opt_clamp(fx_punch, 0, 5, 0.35);
    fx["hit_duration"] = 0.35;
    return fx;
}

// ---------------------------------------------------------------------------
// gen: camera (tuneOrbit / applyCameraMotion)
// ---------------------------------------------------------------------------

// Swap the template's full-turn orbit for a slow symmetric sway about head-on:
// start yawDrift/2 short of straight-on, cover exactly yawDrift over the scene.
static ojson tune_orbit(const ojson& tpl, double duration, const GenArgs& args) {
    const ojson* base =
        tpl.contains("camera_animation") ? &tpl["camera_animation"] : nullptr;
    bool orbit = base && base->is_object() && base->contains("type") &&
                 base->at("type").is_string() && base->at("type").get<string>() == "orbit";
    ojson anim;
    if (orbit) {
        anim = *base;
    } else {
        anim = ojson::object();
        anim["type"] = "orbit";
        anim["radius"] = 14.0;
        anim["elevation"] = 0.09;
        anim["target"] = ojson::array({0.0, 1.0, 0.0});
    }
    if (opt_finite(args.radius) && args.radius.v > 0) anim["radius"] = args.radius.v;
    double drift = opt_finite(args.yaw_drift) ? args.yaw_drift.v : 0.6;
    anim["start_yaw"] = M_PI / 2 - drift / 2;
    anim["speed"] = drift / js_max(duration, 1e-3);
    return anim;
}

// Layer an eased camera move and/or a hand-held shake onto the orbit; with no
// flags the orbit is left untouched, so regenerated scenes stay identical.
static void apply_camera_motion(ojson& anim, double duration, const GenArgs& args) {
    if (!anim.is_object() || !anim.contains("type") || !anim["type"].is_string() ||
        anim["type"].get<string>() != "orbit")
        return;
    bool has_move = opt_finite(args.dolly) || opt_finite(args.crane);
    if (has_move) {
        ojson move = ojson::object();
        move["ease"] = "ease_in_out_cubic";
        if (opt_finite(args.dolly) && args.dolly.v > 0) move["radius_end"] = args.dolly.v;
        if (opt_finite(args.crane)) move["elevation_end"] = args.crane.v;
        move["at"] = opt_finite(args.move_at) ? args.move_at.v : duration * 0.12;
        move["duration"] = opt_finite(args.move_dur) ? args.move_dur.v : duration * 0.76;
        anim["movement"] = move;
    }
    if (opt_finite(args.shake) && args.shake.v > 0) {
        ojson shake = ojson::object();
        shake["amplitude"] = args.shake.v;
        shake["frequency"] = opt_finite(args.shake_freq) ? args.shake_freq.v : 3.5;
        shake["roll"] = opt_finite(args.shake_roll) ? args.shake_roll.v : 0.008;
        shake["at"] = 0.0;
        shake["until"] = nullptr;
        shake["ramp"] = 0.8;
        // Only written when asked for, so scenes without the flag stay identical.
        if (args.shake_hit) {
            shake["hit"] = true;
            shake["hit_duration"] = opt_finite(args.shake_hit_dur) ? args.shake_hit_dur.v : 0.55;
        }
        anim["shake"] = move(shake);
    }
}

// ---------------------------------------------------------------------------
// gen: JS-flavoured JSON serialisation
// ---------------------------------------------------------------------------

// ECMAScript Number → String, the rules JSON.stringify uses for doubles:
// shortest decimal that round-trips, exponent form only outside 1e-7…1e21.
static void js_dtoa(double v, string& out) {
    if (isnan(v) || isinf(v)) {
        out += "null";  // JSON.stringify coerces nonfinite to null
        return;
    }
    if (v == 0) {
        out += "0";  // also -0 → "0"
        return;
    }
    if (v < 0) {
        out += '-';
        v = -v;
    }
    double a = v;
    char buf[64];
    int p;
    for (p = 1; p <= 17; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, a);
        if (strtod(buf, nullptr) == a) break;
    }
    if (p > 17) snprintf(buf, sizeof buf, "%.16e", a);
    // Split "d[.ddd]e±XX" into digits and n, where value = 0.digits · 10^n.
    string mant = buf;
    size_t epos = mant.find('e');
    if (epos == string::npos) epos = mant.find('E');
    long E = epos == string::npos ? 0 : strtol(mant.c_str() + epos + 1, nullptr, 10);
    string digits;
    for (size_t i = 0; i < (epos == string::npos ? mant.size() : epos); i++)
        if (buf[i] >= '0' && buf[i] <= '9') digits += buf[i];
    long n = E + 1;
    size_t k = digits.size();
    auto put_zeros = [&](long count) {
        for (long z = 0; z < count; z++) out += '0';
    };
    if (k <= (size_t)max(n, 0L) && n <= 21) {
        out += digits;
        put_zeros((long)n - (long)k);
    } else if (0 < n && n <= 21) {
        out += digits.substr(0, (size_t)n);
        out += '.';
        out += digits.substr((size_t)n);
    } else if (-6 < n && n <= 0) {
        out += "0.";
        put_zeros(-n);
        out += digits;
    } else {
        out += digits.substr(0, 1);
        if (k > 1) {
            out += '.';
            out += digits.substr(1);
        }
        out += 'e';
        long ex = n - 1;
        out += ex < 0 ? '-' : '+';
        char eb[16];
        snprintf(eb, sizeof eb, "%ld", ex < 0 ? -ex : ex);
        out += eb;
    }
}

static string js_num_str(double v) {
    string o;
    js_dtoa(v, o);
    return o;
}

static void js_json_str(const string& s, string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\f': out += "\\f"; break;
        case '\r': out += "\\r"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof b, "\\u%04x", c);
                out += b;
            } else {
                out += (char)c;
            }
        }
    }
    out += '"';
}

// JSON.stringify(value, null, 2): 2-space indent, empty arrays/objects collapse,
// non-ASCII stays raw UTF-8, no spaces around brackets.
static void js_json(const ojson& j, string& out, int depth) {
    if (j.is_null()) { out += "null"; return; }
    if (j.is_boolean()) { out += j.get<bool>() ? "true" : "false"; return; }
    if (j.is_number_unsigned()) { out += to_string(j.get<uint64_t>()); return; }
    if (j.is_number_integer()) { out += to_string(j.get<int64_t>()); return; }
    if (j.is_number_float()) { js_dtoa(j.get<double>(), out); return; }
    if (j.is_string()) { js_json_str(j.get<string>(), out); return; }
    if (j.is_array()) {
        if (j.empty()) { out += "[]"; return; }
        out += "[\n";
        for (size_t i = 0; i < j.size(); i++) {
            out.append((size_t)depth + 2, ' ');
            js_json(j[i], out, depth + 2);
            if (i + 1 < j.size()) out += ',';
            out += '\n';
        }
        out.append((size_t)depth, ' ');
        out += ']';
        return;
    }
    if (j.empty()) { out += "{}"; return; }
    out += "{\n";
    size_t i = 0, n = j.size();
    for (auto it = j.begin(); it != j.end(); ++it, ++i) {
        out.append((size_t)depth + 2, ' ');
        js_json_str(it.key(), out);
        out += ": ";
        js_json(it.value(), out, depth + 2);
        if (i + 1 < n) out += ',';
        out += '\n';
    }
    out.append((size_t)depth, ' ');
    out += '}';
}

// JS truthiness for template leaf checks (work_dir || default, bloom?, dof?).
static bool js_truthy(const ojson& v) {
    if (v.is_null()) return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) {
        double d = v.get<double>();
        return d == d && d != 0;
    }
    if (v.is_string()) return !v.get<string>().empty();
    return true;  // arrays/objects are truthy even when empty
}

static ojson* find_fade(ojson& block) {
    if (!block.contains("animations")) return nullptr;
    for (auto& an : block["animations"])
        if (an.is_object() && an.contains("type") && an["type"].is_string() &&
            an["type"].get<string>() == "fade")
            return &an;
    return nullptr;
}

static string read_file(const string& path) {
    // MinGW's ifstream(char*) opens through the ANSI codepage and fails on
    // Chinese paths; going through a filesystem::path built from UTF-8 uses
    // the wide opening path instead.
    filesystem::path p = filesystem::u8path(path);
    ifstream f(p, ios::binary);
    if (!f) throw runtime_error("cannot read " + path);
    f.seekg(0, ios::end);
    size_t n = (size_t)f.tellg();
    f.seekg(0);
    string s(n, '\0');
    f.read(&s[0], (streamsize)n);
    if (f.gcount() >= 0 && (size_t)f.gcount() != n) s.resize((size_t)f.gcount());
    return s;
}

// Per-line audio energy in 0..1 from a precomputed envelope (loadEnergy): the
// mean band energy around each cue (0.8 s before, 1.5 s after), normalised so
// the loudest line is 1 and the quietest 0.
static vector<double> load_energy(const string& path, const vector<LrcLine>& lines) {
    ojson env = ojson::parse(read_file(path));
    if (!env.is_object() || !env.contains("data") || !env["data"].is_array() ||
        env["data"].empty())
        throw runtime_error("--energy-file: " + path + " has no envelope data");
    double fps = 60.0;  // Number(env.fps) || 60
    if (env.contains("fps") && env["fps"].is_number()) {
        double f = env["fps"].get<double>();
        if (f != 0 && !isnan(f)) fps = f;
    }
    double frames = (double)env["data"].size();
    vector<double> raw;
    for (const auto& l : lines) {
        double f0 = js_max(0.0, js_round((l.time - 0.8) * fps));
        double f1 = js_min(frames - 1.0, js_round((l.time + 1.5) * fps));
        double sum = 0;
        double n = 0;
        for (double f = f0; f <= f1; f++) {
            const ojson& row = env["data"][(size_t)f];
            if (!row.is_array() || row.empty()) continue;
            double m = 0;
            for (const auto& v : row) m += v.get<double>();
            sum += m / (double)row.size();
            n += 1;
        }
        raw.push_back(n ? sum / n : 0.0);
    }
    double mn = js_min_of(raw);  // Math.min(...raw)
    double mx = js_max_of(raw);
    double span = mx - mn > 1e-9 ? mx - mn : 1.0;
    vector<double> out;
    for (double v : raw) out.push_back(round3((v - mn) / span));
    return out;
}
