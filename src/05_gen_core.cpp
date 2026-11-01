// ---------------------------------------------------------------------------
// gen: LRC → scene JSON (port of js/lrc.js + js/lrc2scene.mjs)
//
//   chamina-cpp gen song.lrc -o song.json [flags like node js/lrc2scene.mjs]
//
// Covers the single-layout default path: LRC parsing (incl. {e:...} line tags
// and <mm:ss.xx> word-timed stamps), entrance/fade/jitter/wave text blocks,
// title & outro cards, orbit retune, camera move/shake, background looks and
// cuts, the fx rack and the colour palette. Also ported: --layout field /
// --groups auto (js/layout.mjs — serpentine world positions, per-cue camera
// stops, per-line exits, word-group choreography and audio-energy whips).
//
// Output aims at byte parity with node's JSON.stringify(scene, null, 2):
// numbers print through js_dtoa (ECMAScript Number→String rules), keys keep
// JS insertion order (ordered_json), and template floats like "1.0" print
// as "1" the way JS sees them. Known deviations: Math.tan/strtod may differ
// from V8 by 1 ulp (fit_width), and localeCompare ties fall back to codepoint
// order.
// ---------------------------------------------------------------------------

using ojson = nlohmann::ordered_json;

// JS Math.round: half rounds toward +Infinity (floor(x+0.5)); std::round
// rounds half away from zero and would disagree on negatives.
static double js_round(double x) { return floor(x + 0.5); }
static double round3(double v) { return js_round(v * 1000.0) / 1000.0; }

static double js_max(double a, double b) {
    if (isnan(a) || isnan(b)) return numeric_limits<double>::quiet_NaN();
    return a > b ? a : b;
}
static double js_min(double a, double b) {
    if (isnan(a) || isnan(b)) return numeric_limits<double>::quiet_NaN();
    return a < b ? a : b;
}
// Math.max(...arr) / Math.min(...arr): a NaN anywhere poisons the whole call
// (unlike std::max_element, which skips comparators against NaN). Anywhere the
// JS folds a possibly-NaN array through the spread, use these.
static double js_max_of(const vector<double>& a) {
    double acc = -numeric_limits<double>::infinity();
    for (double v : a) acc = js_max(acc, v);
    return acc;
}
static double js_min_of(const vector<double>& a) {
    double acc = numeric_limits<double>::infinity();
    for (double v : a) acc = js_min(acc, v);
    return acc;
}

// One UTF-8 codepoint at i (invalid bytes pass through as themselves).
static uint32_t utf8_cp(const string& s, size_t i, size_t& len) {
    unsigned char b = (unsigned char)s[i];
    len = 1;
    if (b < 0x80) return b;
    int extra = 0;
    uint32_t cp = 0;
    if ((b & 0xE0) == 0xC0) { extra = 1; cp = b & 0x1F; }
    else if ((b & 0xF0) == 0xE0) { extra = 2; cp = b & 0x0F; }
    else if ((b & 0xF8) == 0xF0) { extra = 3; cp = b & 0x07; }
    else return b;
    if (i + extra >= s.size()) return b;
    for (int k = 1; k <= extra; k++) {
        unsigned char c = (unsigned char)s[i + k];
        if ((c & 0xC0) != 0x80) { len = 1; return b; }
        cp = (cp << 6) | (c & 0x3F);
    }
    len = (size_t)extra + 1;
    return cp;
}

// ECMAScript WhiteSpace + LineTerminator (what String.trim strips).
static bool js_is_ws(uint32_t cp) {
    switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
    case 0xA0: case 0x1680: case 0x2028: case 0x2029:
    case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return true;
    }
    return cp >= 0x2000 && cp <= 0x200A;
}

static string js_trim(const string& s) {
    size_t b = 0, e = s.size();
    while (b < e) {
        size_t l;
        if (js_is_ws(utf8_cp(s, b, l))) b += l;
        else break;
    }
    size_t end = b, i = b;
    while (i < e) {
        size_t l;
        uint32_t cp = utf8_cp(s, i, l);
        if (!js_is_ws(cp)) end = i + l;
        i += l;
    }
    return s.substr(b, end - b);
}

// Regex-level \s (single codepoint) used by the tag matchers.
static size_t js_ws_at(const string& s, size_t i) {
    size_t l;
    uint32_t cp = utf8_cp(s, i, l);
    return js_is_ws(cp) ? l : 0;
}

static bool js_alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool js_word(unsigned char c) {  // \w
    return js_alpha(c) || (c >= '0' && c <= '9') || c == '_';
}

static string to_lower_ascii(string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

// JS Number(str): surrounding whitespace ignored, "" → 0, full-string float
// syntax otherwise, else NaN. (strtod also accepts "inf"; JS only "Infinity"
// — divergence confined to garbage input.)
static double parse_num(const string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    if (b == e) return 0.0;
    string t = s.substr(b, e - b);
    const char* c = t.c_str();
    char* end = nullptr;
    double v = strtod(c, &end);
    if (end != c + t.size()) return numeric_limits<double>::quiet_NaN();
    return v;
}

// JS optionals default to `null`; Number.isFinite(null) is false while
// clamp(Number(null)) clamps 0 — both behaviours needed, so model the two
// states instead of one sentinel.
struct OptNum {
    bool is_null;
    double v;
};
static OptNum optn(double v) { return {false, v}; }
static const OptNum OPT_NULL{true, 0.0};
static bool opt_finite(const OptNum& o) { return !o.is_null && isfinite(o.v); }
static bool opt_gt0(const OptNum& o) { return !o.is_null && o.v == o.v && o.v > 0; }
// (args.x || 0)
static double opt_num0(const OptNum& o) {
    return (o.is_null || o.v != o.v || o.v == 0) ? 0.0 : o.v;
}
// JS clamp(): Number(v) — null → 0 (finite!), NaN → fallback.
static double opt_clamp(const OptNum& o, double lo, double hi, double fb) {
    if (o.is_null) return min(hi, max(lo, 0.0));
    if (o.v != o.v) return fb;
    return min(hi, max(lo, o.v));
}
// JS clampNum() in lrc.js has the same Number() semantics.
static double clamp_num(double v, double fb, double lo, double hi) {
    if (v != v) return fb;
    return min(hi, max(lo, v));
}

// CLI options for `gen` — every JS-default `null` number is modelled as
// OPT_NULL so Number.isFinite / clamp(Number(v)) behave exactly like node.
struct GenArgs {
    string input, out;
    string templ = "examples/gift.json";  // cwd-relative stage template
    OptNum size = optn(1.4), y = optn(-0.4), depth = optn(0.09);
    string color = "#ffffff";
    OptNum strength = optn(1.6);
    string outline;
    OptNum outline_width = optn(0.02);
    OptNum bloom = optn(0.06);
    string entrance = "slide";
    string layout = "single", groups = "off";
    OptNum jitter = OPT_NULL;
    OptNum jitter_rot = optn(0), jitter_freq = optn(5);
    bool jitter_hit = false;
    OptNum jitter_hit_dur = optn(0.55);
    OptNum wave = OPT_NULL;
    OptNum wave_freq = optn(1.6), wave_spacing = optn(0.55);
    bool wave_hit = false;
    OptNum wave_hit_dur = optn(2.0);
    OptNum dolly = OPT_NULL, crane = OPT_NULL, move_at = OPT_NULL, move_dur = OPT_NULL;
    OptNum shake = OPT_NULL;
    OptNum shake_roll = optn(0.008), shake_freq = optn(3.5);
    bool shake_hit = false;
    OptNum shake_hit_dur = optn(0.55);
    OptNum fit_width = OPT_NULL;
    OptNum offset = optn(0), fade_in = optn(0.3), fade_out = optn(0.45);
    OptNum stagger = optn(0.04), tail = optn(3);
    OptNum duration = OPT_NULL;
    OptNum yaw_drift = optn(0.6), radius = OPT_NULL;
    bool keep_camera = false, keep_open = false;
    OptNum outro_at = OPT_NULL, outro_out = OPT_NULL;
    string outro_text, outro_subtitle, outro_wish;
    bool has_outro_text = false, has_outro_subtitle = false;
    string bg_look;
    vector<string> bg_cut;
    OptNum bg_fade = optn(1.5);
    bool fx = false;
    OptNum fx_flash = OPT_NULL, fx_punch = OPT_NULL, fx_vignette = OPT_NULL,
           fx_chromatic = OPT_NULL, fx_sat = OPT_NULL;
    string palette;
    OptNum roll_power = optn(0);
    string energy_file;
    vector<string> force_whip;
    OptNum fps = OPT_NULL;
    string title, subtitle;
    OptNum title_in = optn(1.2), title_out = OPT_NULL, title_size = optn(3);
    bool help = false;
};

static bool is_hex6(const string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (size_t i = 1; i < 7; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

// The clock regex shared by line tags and glyph stamps:
//   [< (1..3 digits) ':' (1..2 digits) [':'|'.' (1..3 digits)] >]
// matched with real regex backtracking: a bounded digit run that overshoots
// the terminator fails the whole match rather than truncating ("[00:16.1234]"
// is not a time tag).
static size_t digit_run(const string& s, size_t i) {
    size_t n = 0;
    while (i + n < s.size() && s[i + n] >= '0' && s[i + n] <= '9') n++;
    return n;
}
static double digits_val(const string& s, size_t pos, size_t n) {
    long long v = 0;
    for (size_t i = 0; i < n; i++) v = v * 10 + (s[pos + i] - '0');
    return (double)v;
}
static bool match_clock(const string& s, size_t pos, char open, char term,
                        double& sec, size_t& len) {
    if (pos >= s.size() || s[pos] != open) return false;
    size_t run = digit_run(s, pos + 1);
    if (!run) return false;
    size_t mlen = 0;
    for (size_t k = min(run, (size_t)3); k >= 1; k--)
        if (pos + 1 + k < s.size() && s[pos + 1 + k] == ':') { mlen = k; break; }
    if (!mlen) return false;
    double minutes = digits_val(s, pos + 1, mlen);
    size_t i = pos + 1 + mlen + 1;  // past ':'
    size_t run2 = digit_run(s, i);
    if (!run2) return false;
    for (size_t k = min(run2, (size_t)2); k >= 1; k--) {
        size_t j = i + k;
        if (j >= s.size()) continue;
        char c = s[j];
        if (c == term) {
            sec = minutes * 60.0 + digits_val(s, i, k);
            len = j + 1 - pos;
            return true;
        }
        if (c == '.' || c == ':') {
            // The fraction can only be the full digit run after the separator;
            // any shorter prefix ends on a digit, never the terminator.
            size_t fr = digit_run(s, j + 1);
            if (fr >= 1 && fr <= 3 && j + 1 + fr < s.size() && s[j + 1 + fr] == term) {
                double frac = digits_val(s, j + 1, fr) / pow(10.0, (double)fr);
                sec = minutes * 60.0 + digits_val(s, i, k) + frac;
                len = j + 1 + fr + 1 - pos;
                return true;
            }
        }
    }
    return false;
}

struct LrcLine {
    double time = 0;
    string text;
    bool has_tags = false;
    map<string, string> tags;  // keys lowercased at parse, never serialized
    bool has_ct = false;
    vector<double> ct;         // one absolute time per codepoint
};
struct LrcParsed {
    map<string, string> meta;
    double offset_ms = 0;
    vector<LrcLine> lines;
};

static bool match_meta(const string& s, string& key, string& value) {
    // META_RE = /^\[([A-Za-z_][\w#-]*):(.*)\]\s*$/
    if (s.size() < 3 || s[0] != '[') return false;
    if (!(js_alpha((unsigned char)s[1]) || s[1] == '_')) return false;
    size_t i = 2;
    while (i < s.size() && (js_word((unsigned char)s[i]) || s[i] == '#' || s[i] == '-')) i++;
    if (i >= s.size() || s[i] != ':') return false;
    size_t close = s.rfind(']');
    if (close == string::npos || close <= i) return false;
    for (size_t k = close + 1; k < s.size(); k++)
        if (!js_ws_at(s, k)) return false;
    key = s.substr(1, i - 1);
    value = s.substr(i + 1, close - i - 1);
    return true;
}

// TAG_RE — leading {key:value} overrides, key lowercased, value trimmed.
static bool match_tag(const string& s, string& key, string& val, size_t& len) {
    if (s.empty() || s[0] != '{') return false;
    size_t i = 1;
    if (i >= s.size() || !(js_alpha((unsigned char)s[i]) || s[i] == '_')) return false;
    i++;
    while (i < s.size() && (js_word((unsigned char)s[i]) || s[i] == '-')) i++;
    size_t j = i;
    while (j < s.size()) {
        size_t l = js_ws_at(s, j);
        if (!l) break;
        j += l;
    }
    if (j >= s.size() || (s[j] != ':' && s[j] != '=')) return false;
    j++;
    while (j < s.size()) {
        size_t l = js_ws_at(s, j);
        if (!l) break;
        j += l;
    }
    size_t vs = j;
    while (j < s.size() && s[j] != '{' && s[j] != '}') j++;
    if (j >= s.size() || s[j] != '}') return false;
    key = to_lower_ascii(s.substr(1, i - 1));
    val = js_trim(s.substr(vs, j - vs));
    len = j + 1;
    return true;
}

static pair<map<string, string>, string> split_tags(const string& text) {
    map<string, string> tags;
    string rest = text;
    string key, val;
    size_t len;
    while (match_tag(rest, key, val, len)) {
        tags[key] = val;
        rest = rest.substr(len);
    }
    return {move(tags), js_trim(rest)};
}

// splitGlyphTimes — word-timed LRC: <mm:ss.xx> stamps in front of a glyph or a
// run of glyphs; characters inherit the last stamp, untagged text falls back
// to the line cue.
static void split_glyph_times(const string& src, double line_time, string& text,
                              vector<double>& ct, bool& saw) {
    size_t i = 0;
    bool have_at = false;
    double at = 0;
    while (i < src.size()) {
        double stamp;
        size_t slen;
        if (src[i] == '<' && match_clock(src, i, '<', '>', stamp, slen)) {
            at = stamp;
            have_at = true;
            saw = true;
            i += slen;
            continue;
        }
        double when = have_at ? at : line_time;
        if (src[i] == '<') {  // '.' matches the lone '<' as one unit
            text += '<';
            ct.push_back(when);
            i += 1;
            continue;
        }
        while (i < src.size() && src[i] != '<') {
            size_t l;
            utf8_cp(src, i, l);  // one entry per codepoint
            text += src.substr(i, l);
            ct.push_back(when);
            i += l;
        }
    }
}

static LrcParsed parse_lrc(const string& src) {
    LrcParsed p;
    size_t pos = 0;
    while (pos <= src.size()) {
        size_t nl = src.find('\n', pos);
        string raw = src.substr(pos, (nl == string::npos ? src.size() : nl) - pos);
        pos = nl == string::npos ? src.size() + 1 : nl + 1;
        string line = js_trim(raw);
        if (line.empty()) continue;

        // Collect every leading timestamp before deciding what this row is.
        vector<double> times;
        size_t rest_i = 0;
        for (;;) {
            double sec;
            size_t len;
            if (!match_clock(line, rest_i, '[', ']', sec, len)) break;
            times.push_back(sec);
            rest_i += len;
        }
        if (!times.empty()) {
            // Strip a leading run of whitespace/colons (JS /^[\s:]+/), then the
            // {…} overrides.
            string body = line.substr(rest_i);
            size_t lb = 0;
            while (lb < body.size()) {
                size_t l = js_ws_at(body, lb);
                if (!l && body[lb] != ':') break;
                lb += l ? l : 1;
            }
            body = js_trim(body.substr(lb));
            auto [tags, text0] = split_tags(body);
            string text;
            vector<double> ct;
            bool saw = false;
            split_glyph_times(text0, times[0], text, ct, saw);
            for (double t : times) {
                LrcLine e;
                e.time = t;
                e.text = text;
                if (!tags.empty()) {
                    e.has_tags = true;
                    e.tags = tags;
                }
                if (saw) {
                    // A repeated stamp replays the same run on this copy's cue.
                    e.has_ct = true;
                    e.ct = ct;
                    double shift = t - times[0];
                    if (shift != 0)
                        for (auto& v : e.ct) v += shift;
                }
                p.lines.push_back(move(e));
            }
            continue;
        }

        string mk, mv;
        if (match_meta(line, mk, mv)) {
            string key = to_lower_ascii(mk);
            string value = js_trim(mv);
            p.meta[key] = value;
            if (key == "offset") {
                double n = parse_num(value);
                if (isfinite(n)) p.offset_ms = n;
            }
        }
    }

    // JS sorts with `a.time - b.time || a.text.localeCompare(b.text)`; the
    // collation is locale-dependent, so exact ties (same timestamp) fall back
    // to codepoint order here.
    stable_sort(p.lines.begin(), p.lines.end(), [](const LrcLine& a, const LrcLine& b) {
        if (a.time != b.time) return a.time < b.time;
        return a.text < b.text;
    });
    return p;
}

static vector<LrcLine> apply_offset(const LrcParsed& p, bool has_given, double given) {
    // applyOffset: shift = (Number(offsetMs) || 0) / 1000 — a positive
    // [offset:+500] tag displays the lyrics 500 ms earlier.
    double ms = has_given ? given : p.offset_ms;
    double shift = (ms == ms && ms != 0 ? ms : 0.0) / 1000.0;
    vector<LrcLine> out;
    for (const auto& l : p.lines) {
        LrcLine o;
        o.time = l.time - shift;
        o.text = l.text;
        o.has_tags = l.has_tags;
        o.tags = l.tags;
        if (l.has_ct) {
            o.has_ct = true;
            for (double v : l.ct) o.ct.push_back(v - shift);
        }
        out.push_back(move(o));
    }
    return out;
}

static double suggest_duration(const vector<LrcLine>& lines, double tail) {
    double last = lines.empty() ? 0.0 : lines.back().time;
    return js_max(4.0, ceil(last + tail));
}

// ---------------------------------------------------------------------------
// gen: entrances
// ---------------------------------------------------------------------------

static void check_entrance(const string& mode) {
    static const char* ENTRANCES[] = {"none", "slide", "fall", "pop", "overshoot"};
    for (const char* e : ENTRANCES)
        if (mode == e) return;
    throw runtime_error("unknown entrance \"" + mode + "\" (have: none, slide, fall, pop, overshoot)");
}

// The entrance for one line: its `{e:fall}` tag if present, else the
// scene-wide default. A typo'd mode throws — a whole PV line would be off.
static string line_entrance(const LrcLine& line, const string& fallback) {
    if (!line.has_tags) return fallback;
    auto it = line.tags.find("e");
    if (it == line.tags.end() || it->second.empty()) return fallback;
    string mode = to_lower_ascii(js_trim(it->second));
    check_entrance(mode);
    return mode;
}

// ---------------------------------------------------------------------------
// gen: per-line colour palette (hexHsl / hslHex / applyPalette)
// ---------------------------------------------------------------------------

struct Hsl {
    double h, s, l;
};

// #rrggbb → {h, s, l} in 0..1. Plain enough for tinting rims.
static Hsl hex_hsl(const string& hex) {
    string h = hex.empty() ? "#ffffff" : hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    long long n = 0;
    for (size_t i = 0; i < min(h.size(), (size_t)6); i++) {
        char c = h[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;  // parseInt stops at the first invalid character
        n = n * 16 + d;
    }
    double r = ((n >> 16) & 255) / 255.0;
    double g = ((n >> 8) & 255) / 255.0;
    double b = (n & 255) / 255.0;
    double mx = max(r, max(g, b)), mn = min(r, min(g, b));
    double l = (mx + mn) / 2;
    if (mx == mn) return {0, 0, l};
    double d = mx - mn;
    double s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    double hh;
    if (mx == r) hh = ((g - b) / d + (g < b ? 6 : 0)) / 6;
    else if (mx == g) hh = ((b - r) / d + 2) / 6;
    else hh = ((r - g) / d + 4) / 6;
    return {hh, s, l};
}

static double hcl(double x) { return js_round(min(255.0, max(0.0, x)) * 255.0); }

static double hue2rgb(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 1.0 / 2) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

static string rgb_hex6(int r, int g, int b) {
    char buf[16];
    snprintf(buf, sizeof buf, "#%06x", (r << 16) | (g << 8) | b);
    return buf;
}

// {h, s, l} in 0..1 → #rrggbb.
static string hsl_hex(double h, double s, double l) {
    double H = fmod(fmod(h, 1.0) + 1.0, 1.0);
    double S = min(1.0, max(0.0, s == s ? s : 0.0));
    double L = min(1.0, max(0.0, l == l ? l : 0.0));
    if (S == 0) {
        int v = (int)hcl(L);
        return rgb_hex6(v, v, v);
    }
    double q = L < 0.5 ? L * (1 + S) : L + S - L * S;
    double p = 2 * L - q;
    int r = (int)hcl(hue2rgb(p, q, H + 1.0 / 3.0));
    int g = (int)hcl(hue2rgb(p, q, H));
    int b = (int)hcl(hue2rgb(p, q, H - 1.0 / 3.0));
    return rgb_hex6(r, g, b);
}

// Cycle a romantic palette across the lyric blocks; the rim follows each
// line's tint, hue-shifted a little and pushed darker and more saturated.
static void apply_palette(vector<ojson>& texts, const string& hexes) {
    vector<string> pal;
    size_t pos = 0;
    while (pos <= hexes.size()) {
        size_t c = hexes.find(',', pos);
        string part = js_trim(hexes.substr(pos, (c == string::npos ? hexes.size() : c) - pos));
        if (is_hex6(part)) pal.push_back(part);
        if (c == string::npos) break;
        pos = c + 1;
    }
    if (pal.empty())
        throw runtime_error("--palette wants comma-separated #rrggbb values");
    for (size_t i = 0; i < texts.size(); i++) {
        const string& c = pal[i % pal.size()];
        texts[i]["material"]["color"] = to_lower_ascii(c);
        if (texts[i]["material"].contains("outline")) {
            Hsl hs = hex_hsl(c);
            texts[i]["material"]["outline"]["color"] =
                hsl_hex(hs.h + 0.055, min(1.0, hs.s * 1.6 + 0.18), hs.l * 0.6);
        }
    }
}

// ---------------------------------------------------------------------------
// gen: frame width budget (visibleWidth)
// ---------------------------------------------------------------------------

// Full width of the text plane the camera can see, in world units — the
// budget for fit_width so a line can never run off frame.
static double visible_width(const ojson& scene, const ojson* cam_anim) {
    static const ojson EMPTY_OBJ = ojson::object();
    const ojson& cam = scene.contains("camera") ? scene["camera"] : EMPTY_OBJ;
    const ojson& anim = cam_anim ? *cam_anim : EMPTY_OBJ;
    double radius = 14.0;
    if (anim.contains("radius") && anim["radius"].is_number()) {
        double r = anim["radius"].get<double>();
        radius = r != 0 ? r : 14.0;  // Number(anim.radius) || 14
    }
    if (anim.contains("movement") && anim["movement"].is_object()) {
        const ojson& mv = anim["movement"];
        if (mv.contains("radius_end") && mv["radius_end"].is_number()) {
            double re = mv["radius_end"].get<double>();
            if (isfinite(re)) radius = min(radius, re);
        }
    }
    double fov = 0.8726646;
    if (cam.contains("fov") && cam["fov"].is_number()) {
        double f = cam["fov"].get<double>();
        fov = f != 0 ? f : 0.8726646;
    }
    double w = 1920, h = 1080;
    if (scene.contains("width") && scene["width"].is_number()) {
        double v = scene["width"].get<double>();
        w = v != 0 ? v : 1920;
    }
    if (scene.contains("height") && scene["height"].is_number()) {
        double v = scene["height"].get<double>();
        h = v != 0 ? v : 1080;
    }
    double aspect = w / h;
    return 2.0 * tan(fov / 2.0) * radius * aspect;
}

// ---------------------------------------------------------------------------
// gen: idle wobble (buildJitter) and karaoke bounce (buildWave)
// ---------------------------------------------------------------------------

static double num_or(const ojson& j, const char* key, double d) {
    double v = j.value(key, d);
    return isfinite(v) ? v : d;
}

// `jitter: 0.03` → one `jitter` animation for line i, phased so neighbouring
// lines never wobble in lockstep. `delay` holds it back until the entrance
// has settled. Returns null when absent or amplitude is zero.
static ojson build_jitter(const ojson* jitter, int i, double t) {
    if (!jitter) return ojson(nullptr);
    const ojson& j = *jitter;
    double a = num_or(j, "amplitude", 0.03);
    double amp[3] = {a, a * 0.8, 0.0};
    if (amp[0] == 0 && amp[1] == 0 && amp[2] == 0) return ojson(nullptr);
    ojson out = ojson::object();
    out["type"] = "jitter";
    out["amplitude"] = ojson::array({round3(amp[0]), round3(amp[1]), round3(amp[2])});
    out["frequency"] = round3(num_or(j, "frequency", 5.0));
    out["rotation"] = round3(num_or(j, "rotation", 0.0));
    out["phase"] = round3(i * 1.7);
    out["start_at"] = round3(t + num_or(j, "delay", 0.4));
    out["stagger"] = round3(num_or(j, "stagger", 0.0));
    out["ramp"] = round3(num_or(j, "ramp", 0.5));
    // Written only when asked for, so scenes without the flag stay identical.
    if (j.value("hit", false)) {
        out["hit"] = true;
        out["hit_duration"] = round3(num_or(j, "hit_duration", 0.55));
    }
    return out;
}

// `wave: 0.2` → the karaoke bounce: each character trails the one before it
// by `spacing` radians. Returns null when absent or amplitude is zero.
static ojson build_wave(const ojson* wave, int i, double t) {
    if (!wave) return ojson(nullptr);
    const ojson& w = *wave;
    double amp = num_or(w, "amplitude", 0.2);
    if (amp == 0 || amp != amp) return ojson(nullptr);
    (void)i;
    ojson out = ojson::object();
    out["type"] = "wave";
    out["amplitude"] = round3(amp);
    out["frequency"] = round3(num_or(w, "frequency", 1.6));
    out["spacing"] = round3(num_or(w, "spacing", 0.55));
    out["start_at"] = round3(t + num_or(w, "delay", 0.0));
    out["ramp"] = round3(num_or(w, "ramp", 0.4));
    if (w.value("hit", false)) {
        out["hit"] = true;
        out["hit_duration"] = round3(num_or(w, "hit_duration", 2.0));
    }
    return out;
}

// ---------------------------------------------------------------------------
// gen: word grouping (port of js/lrc.js wordGroups / groupCharTimes) — the
// `--groups auto` path. A group always breaks at a space (the space rides
// with the word before it) and each phrase between spaces is cut by a rhythm
// picked from the line/phrase index; the fixed lyric corpus is hand-worded
// instead of guessed.
// ---------------------------------------------------------------------------

static const double SPACE_EM = 0.2959;  // advance of a space in the layout font

// Advance of a line in em, mirroring `text::layout` in Rust. Everything that
// is neither CJK nor a space is counted a whole em — an overestimate only
// ever makes the frame budget more conservative. (The CJK branch adds 1 too,
// so only the space is special.)
static double em_width(const string& text) {
    double em = 0;
    for (size_t i = 0; i < text.size();) {
        size_t l;
        uint32_t cp = utf8_cp(text, i, l);
        i += l;
        em += cp == ' ' ? SPACE_EM : 1.0;
    }
    return em;
}

// js/lrc.js thirds / chunks — the two helpers PHRASE_CUTS composes with.
static vector<int> thirds_cut(int n) {
    int a = (int)js_round(n / 3.0);
    int b = (int)js_round((n - a) / 2.0);
    return {a, b, n - a - b};
}
static vector<int> chunks_cut(int n, int k) {
    vector<int> out;
    int left = n;
    for (int i = k; i > 0; i--) {
        int take = (int)js_max(1.0, js_round(left / (double)i));
        out.push_back((int)js_min((double)take, (double)(left - (i - 1))));
        left -= out.back();
    }
    vector<int> nz;
    for (int x : out)
        if (x > 0) nz.push_back(x);
    return nz;
}

// phrase cut table (js/lrc.js PHRASE_CUTS — fifteen rhythms, index = kind)
typedef vector<int> (*CutFn)(int);
static vector<int> cut_all(int n) { return {n}; }
static vector<int> cut_1_rest(int n) { return n >= 2 ? vector<int>{1, n - 1} : vector<int>{n}; }
static vector<int> cut_2_rest(int n) { return n >= 3 ? vector<int>{2, n - 2} : vector<int>{n}; }
static vector<int> cut_11_rest(int n) { return n >= 4 ? vector<int>{1, 1, n - 2} : vector<int>{n}; }
static vector<int> cut_rest_1(int n) { return n >= 2 ? vector<int>{n - 1, 1} : vector<int>{n}; }
static vector<int> cut_half(int n) {
    // Math.ceil(n/2), Math.floor(n/2)
    return n >= 2 ? vector<int>{(n + 1) / 2, n / 2} : vector<int>{n};
}
static vector<int> cut_thirds(int n) { return n >= 3 ? thirds_cut(n) : vector<int>{n}; }
static vector<int> cut_1_rest_1(int n) { return n >= 4 ? vector<int>{1, n - 2, 1} : vector<int>{n}; }
static vector<int> cut_chunks2(int n) { return chunks_cut(n, 2); }
static vector<int> cut_chunks3(int n) { return chunks_cut(n, 3); }
static vector<int> cut_12_rest(int n) { return n >= 5 ? vector<int>{1, 2, n - 3} : vector<int>{n}; }
static vector<int> cut_rest_21(int n) { return n >= 5 ? vector<int>{n - 3, 2, 1} : vector<int>{n}; }
static vector<int> cut_22_rest(int n) { return n >= 6 ? vector<int>{2, 2, n - 4} : vector<int>{n}; }
static vector<int> cut_31_rest(int n) { return n >= 6 ? vector<int>{3, 1, n - 4} : vector<int>{n}; }
static vector<int> cut_1_mid_rest(int n) {
    if (n >= 3) {
        int mid = (n - 1 + 1) / 2;  // Math.ceil((n-1)/2)
        return {1, mid, n - 1 - mid};
    }
    return {n};
}
static const CutFn PHRASE_CUTS[] = {
    cut_all,     cut_1_rest,  cut_2_rest,   cut_11_rest, cut_rest_1,
    cut_half,    cut_thirds,  cut_1_rest_1, cut_chunks2, cut_chunks3,
    cut_12_rest, cut_rest_21, cut_22_rest,  cut_31_rest, cut_1_mid_rest,
};
static const int PHRASE_CUT_COUNT = (int)(sizeof PHRASE_CUTS / sizeof PHRASE_CUTS[0]);

// Hand-worded segmentation for the fixed lyric corpus (js/lrc.js
// WORD_SPLIT_SPECS): line text → '|'-separated words, the trailing space of a
// gap riding with the word before it so the pieces tile the whole string.
struct SplitSpec {
    const char* text;
    const char* spec;
};
static const SplitSpec WORD_SPLIT_SPECS[] = {
    {"远离那些繁华悲伤 我们从此告别了过往", "远离|那些|繁华|悲伤 |我们|从此|告别了|过往"},
    {"飞鸟张开时光翅膀 经历了无数风雨阻挡", "飞鸟|张开|时光|翅膀 |经历了|无数|风雨|阻挡"},
    {"来到她身旁 谁的目光 停留在那个梦里的夜晚",
     "来到|她|身旁 |谁的|目光 |停留|在|那个|梦里|的|夜晚"},
    {"我们追随风的方向 遥远飞行地老天荒 别彷徨",
     "我们|追随|风的|方向 |遥远|飞行|地老天荒 |别|彷徨"},
    {"在初遇的那地方 我们曾眷恋不已 梦想", "在|初遇|的|那地方 |我们|曾|眷恋|不已 |梦想"},
    {"面对迷惘 也不要放弃了希望", "面对|迷惘 |也|不要|放弃了|希望"},
    {"就像是盛开的花 对阳光不变的那信仰", "就像是|盛开|的|花 |对|阳光|不变的|那|信仰"},
    {"天空里早已留下了 永恒的神话", "天空里|早已|留下了 |永恒的|神话"},
    {"在夏天的操场 静静地眺望 在远方", "在|夏天|的|操场 |静静地|眺望 |在|远方"},
    {"连绵不断 是回忆之间的海洋", "连绵|不断 |是|回忆|之间|的|海洋"},
    {"穿过云端 是谁的天堂 在召唤", "穿过|云端 |是|谁的|天堂 |在|召唤"},
    {"她在寻访 那沉睡千年的答案", "她|在|寻访 |那|沉睡|千年|的|答案"},
    {"在告别的那地方 我们都无法放下的伤感",
     "在|告别|的|那地方 |我们|都|无法|放下|的|伤感"},
    {"挥一挥手留下的是美丽愿望", "挥一挥手|留下的|是|美丽|愿望"},
    {"就像是逆风的帆 对海浪不屈的那坚强", "就像是|逆风|的|帆 |对|海浪|不屈|的|那|坚强"},
    {"秋天的夜晚 静静地回想 在远方", "秋天|的|夜晚 |静静地|回想 |在|远方"},
    {"挥去不散 我们怀念着的故乡", "挥去|不散 |我们|怀念着|的|故乡"},
    {"在我们身旁", "在|我们|身旁"},
    {"她的目光 飞翔在每个梦里的夜晚", "她的|目光 |飞翔|在|每个|梦里|的|夜晚"},
    {"我们追随风的方向 遥远飞行地久天长 不返航",
     "我们|追随|风的|方向 |遥远|飞行|地久天长 |不|返航"},
};
// PHRASE_CUT_OVERRIDES（js/lrc.js）—— `lineIndex:phraseIndex` → 表项：第 0 行
// 的两个短语由手写节拍替代公式节拍，在 word_groups 内联生效（0:0 → 表[3]，
// 0:1 → 表[12]）。

struct WordGroup {
    int i0, i1;   // half-open codepoint range
    double t;     // the time the whole group lands on (its first glyph's stamp)
    int kind;     // phrase-cut index (fallback groups carry -1)
};

static vector<WordGroup> word_groups(const string& text, const vector<double>& char_times,
                                     int seed) {
    if (char_times.empty()) return {};
    // one byte offset per codepoint (spaces break phrases at codepoint level)
    vector<size_t> off;
    for (size_t i = 0; i < text.size();) {
        size_t l;
        utf8_cp(text, i, l);
        off.push_back(i);
        i += l;
    }
    size_t n = min(off.size(), char_times.size());
    if (n == 0) return {};
    vector<WordGroup> out;
    for (const auto& sp : WORD_SPLIT_SPECS) {
        if (text != sp.text) continue;
        const string specs = sp.spec;
        size_t i = 0, pos = 0;
        while (true) {
            size_t bar = specs.find('|', pos);
            string piece = specs.substr(pos, bar == string::npos ? string::npos : bar - pos);
            if (i < n) {
                size_t len = 0;
                for (size_t j = 0; j < piece.size();) {
                    size_t l;
                    utf8_cp(piece, j, l);
                    j += l;
                    len++;
                }
                size_t end = min(i + len, n);
                out.push_back({(int)i, (int)end, char_times[i], seed % PHRASE_CUT_COUNT});
                i = end;
            }
            if (bar == string::npos) break;
            pos = bar + 1;
        }
        // A corpus line that grew past its table entry keeps the remainder on
        // the last group rather than dropping glyphs on the floor.
        if (i < n) out.push_back({(int)i, (int)n, char_times[i], seed % PHRASE_CUT_COUNT});
        return out;
    }
    size_t phrase_start = 0;
    int phrase_index = 0;
    auto flush = [&](size_t phrase_end) {
        size_t len = phrase_end - phrase_start;
        if (len == 0) return;
        int kind = (seed * 5 + phrase_index * 3) % PHRASE_CUT_COUNT;
        // PHRASE_CUT_OVERRIDES — "0:0" → PHRASE_CUTS[3], "0:1" → PHRASE_CUTS[12]
        CutFn cut = PHRASE_CUTS[kind];
        if (seed == 0 && phrase_index == 0) cut = PHRASE_CUTS[3];
        else if (seed == 0 && phrase_index == 1) cut = PHRASE_CUTS[12];
        size_t i = phrase_start;
        for (int piece : cut((int)len)) {
            if (piece <= 0) continue;
            // charTimes[i] can be undefined: the piece sum may run past the
            // line when chunks(...) filters to a longer run. undefined joins
            // later arithmetic as NaN (same as storing NaN directly).
            double t0 = i < char_times.size()
                            ? char_times[i]
                            : numeric_limits<double>::quiet_NaN();
            out.push_back({(int)i, (int)(i + piece), t0, kind});
            i += piece;
        }
        phrase_index += 1;
    };
    // Cut *after* the space, so the gap rides with the word it follows and
    // the groups tile the whole line with no holes.
    for (size_t i = 0; i < n; i++) {
        size_t l;
        uint32_t cp = utf8_cp(text, off[i], l);
        if (cp == ' ') {
            flush(i + 1);
            phrase_start = i + 1;
        }
    }
    flush(n);
    return out;
}

// Compress per-glyph stamps onto their group, then sweep the glyphs inside a
// group 35 ms apart (js/lrc.js groupCharTimes).
static vector<double> group_char_times(const string& text, const vector<double>& ct, int seed) {
    auto gs = word_groups(text, ct, seed);
    if (gs.empty()) return ct;
    vector<double> out = ct;
    for (const auto& g : gs)
        for (int i = g.i0; i < g.i1; i++) {
            // A JS array grows in place on out[i] = v when i == length: piece
            // sums can run past the source stamps, extending the sweep by one
            // entry per group tail (a hole would read undefined → NaN, so fill
            // the vector with NaN the same way).
            if ((size_t)i >= out.size())
                out.resize((size_t)i + 1, numeric_limits<double>::quiet_NaN());
            out[i] = g.t + (i - g.i0) * 0.035;
        }
    return out;
}
