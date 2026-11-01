// ---------------------------------------------------------------------------
// gen: field layout (port of js/layout.mjs layoutField) — serpentine world
// positions, per-cue camera stops and per-line exits. All double: the output
// is scene JSON, so it must round-trip exactly like node's, unlike the
// renderer's float pipeline.
// ---------------------------------------------------------------------------

using d3 = std::array<double, 3>;

static double clamp_d(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

// js/anim.mjs easeApply — clamp01 first, then the curve (all double).
static double ease_apply_d(const string& name, double x) {
    x = clamp_d(x, 0.0, 1.0);
    if (name == "linear") return x;
    if (name == "ease_in_quad") return x * x;
    if (name == "ease_out_quad") return 1 - (1 - x) * (1 - x);
    if (name == "ease_in_out_cubic") return x < 0.5 ? 4 * x * x * x : 1 - pow(-2 * x + 2, 3) / 2;
    if (name == "back_out") {
        const double C1 = 1.70158, C3 = C1 + 1;
        double u = x - 1;
        return 1 + C3 * u * u * u + C1 * u * u;
    }
    return x * x * (3 - 2 * x);  // smooth (default)
}

// js/anim.mjs evalOrbit — movement easing + sequential stop easing with the
// exact from* chaining (fr/fe/…), all double.
static void eval_orbit_d(const ojson& o, double t, double& radius, double& elevation,
                         d3& target, double& yaw, double& roll) {
    radius = o.at("radius").get<double>();
    elevation = o.at("elevation").get<double>();
    target = {o.at("target")[0].get<double>(), o.at("target")[1].get<double>(),
              o.at("target")[2].get<double>()};
    double yaw_off = 0, roll_off = 0;
    auto m = o.find("movement");
    if (m != o.end() && m->is_object()) {
        double at = m->contains("at") && (*m)["at"].is_number() ? (*m)["at"].get<double>() : 0.0;
        double dur = m->contains("duration") && (*m)["duration"].is_number()
                         ? (*m)["duration"].get<double>()
                         : 1.0;
        double k = ease_apply_d(m->value("ease", string("ease_in_out_cubic")), (t - at) / max(dur, 1e-6));
        auto re = m->find("radius_end");
        if (re != m->end() && re->is_number()) radius = radius + (re->get<double>() - radius) * k;
        auto ee = m->find("elevation_end");
        if (ee != m->end() && ee->is_number())
            elevation = elevation + (ee->get<double>() - elevation) * k;
        auto te = m->find("target_end");
        if (te != m->end() && te->is_array())
            target = {target[0] + ((*te)[0].get<double>() - target[0]) * k,
                      target[1] + ((*te)[1].get<double>() - target[1]) * k,
                      target[2] + ((*te)[2].get<double>() - target[2]) * k};
    }
    auto stops = o.find("stops");
    if (stops != o.end() && stops->is_array()) {
        double fr = radius, fe = elevation, fyaw = yaw_off, froll = roll_off;
        d3 ft = target;
        for (const auto& s : *stops) {
            double at = s.contains("at") && s["at"].is_number() ? s["at"].get<double>() : 0.0;
            if (t < at) break;
            double dur = s.contains("duration") && s["duration"].is_number()
                             ? s["duration"].get<double>()
                             : 1.0;
            double k = ease_apply_d(s.contains("ease") && s["ease"].is_string()
                                        ? s["ease"].get<string>()
                                        : string("ease_in_out_cubic"),
                                    (t - at) / max(dur, 1e-6));
            auto rad = s.find("radius");
            double tr = rad != s.end() && rad->is_number() ? rad->get<double>() : fr;
            auto elv = s.find("elevation");
            double te2 = elv != s.end() && elv->is_number() ? elv->get<double>() : fe;
            d3 tt = ft;
            auto tg = s.find("target");
            if (tg != s.end() && tg->is_array())
                tt = {(*tg)[0].get<double>(), (*tg)[1].get<double>(), (*tg)[2].get<double>()};
            auto yw = s.find("yaw");
            double tyaw = yw != s.end() && yw->is_number() ? yw->get<double>() : fyaw;
            auto rl = s.find("roll");
            double troll = rl != s.end() && rl->is_number() ? rl->get<double>() : froll;
            radius = fr + (tr - fr) * k;
            elevation = fe + (te2 - fe) * k;
            target = {ft[0] + (tt[0] - ft[0]) * k, ft[1] + (tt[1] - ft[1]) * k,
                      ft[2] + (tt[2] - ft[2]) * k};
            yaw_off = fyaw + (tyaw - fyaw) * k;
            roll_off = froll + (troll - froll) * k;
            fr = tr;
            fe = te2;
            ft = tt;
            fyaw = tyaw;
            froll = troll;
        }
    }
    yaw = (o.contains("start_yaw") && o["start_yaw"].is_number() ? o["start_yaw"].get<double>() : 0.0) +
          (o.contains("speed") && o["speed"].is_number() ? o["speed"].get<double>() : 0.0) * t +
          yaw_off;
    roll = roll_off;
}

struct BasisD {
    d3 eye, fwd, right, up;
};

// js/anim.mjs orbitEye + cameraBasis — pinhole basis built the way
// Object3D.lookAt builds it (world up is +Y).
static BasisD camera_basis_d(double radius, double elevation, const d3& target, double yaw) {
    double ce = cos(elevation), se = sin(elevation);
    d3 eye = {target[0] + radius * ce * cos(yaw), target[1] + radius * se,
              target[2] + radius * ce * sin(yaw)};
    double fx = target[0] - eye[0], fy = target[1] - eye[1], fz = target[2] - eye[2];
    double fl = hypot(hypot(fx, fy), fz);
    if (fl == 0) fl = 1;  // Math.hypot(...) || 1
    fx /= fl;
    fy /= fl;
    fz /= fl;
    // right = normalize(cross(forward, worldUp)); cross((x,y,z),(0,1,0)) = (-z,0,x)
    double rx = -fz, ry = 0.0, rz = fx;
    double rl = hypot(hypot(rx, ry), rz);
    if (rl == 0) rl = 1;
    rx /= rl;
    ry /= rl;
    rz /= rl;
    // up' = cross(right, forward)
    d3 up = {ry * fz - rz * fy, rz * fx - rx * fz, rx * fy - ry * fx};
    return {eye, {fx, fy, fz}, {rx, ry, rz}, up};
}

// js/anim.mjs projectNDC — NDC of p in [-1,1] across the frame; false when it
// sits behind the eye (JS null).
static bool project_ndc_d(const BasisD& b, const d3& p, double fov, double aspect, double& x,
                          double& y) {
    double vx = p[0] - b.eye[0], vy = p[1] - b.eye[1], vz = p[2] - b.eye[2];
    double zc = vx * b.fwd[0] + vy * b.fwd[1] + vz * b.fwd[2];
    if (zc <= 1e-6) return false;
    double xc = vx * b.right[0] + vy * b.right[1] + vz * b.right[2];
    double yc = vx * b.up[0] + vy * b.up[1] + vz * b.up[2];
    double th = tan(fov / 2);
    x = (xc / zc) / (th * aspect);
    y = (yc / zc) / th;
    return true;
}

// js/layout.mjs rng — mulberry-style counter, Math.imul arithmetic.
static uint32_t js_imul(uint32_t a, uint32_t b) { return (uint32_t)((uint64_t)a * b); }
static uint32_t to_uint32(double v) {
    double m = fmod(v, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (uint32_t)m;
}
struct JsRng {
    uint32_t s;
    explicit JsRng(double seed) {
        uint32_t u = to_uint32(seed);
        s = u ? u : 1;
    }
    double next() {
        s += 0x6d2b79f5u;  // (s + k) >>> 0
        uint32_t t = s;
        t = js_imul(t ^ (t >> 15), t | 1);
        t = t ^ (uint32_t)(t + js_imul(t ^ (t >> 7), t | 61));
        return (double)(uint32_t)(t ^ (t >> 14)) / 4294967296.0;
    }
};

struct Stage {
    double r_add, sx, sy, el;
};
// Framing presets, cycled by line index. rAdd dollies out (which is what
// creates room for sx); sx/sy are the line's slot in NDC; el tilts the orbit.
static const Stage STAGES[] = {
    {0.0, 0.0, 0.0, 0.0},    // 0 centre close-up
    {0.6, -0.16, 0.14, 0.03},   // 1 slight wide, upper-left
    {0.25, 0.07, -0.2, -0.02},  // 2 near, lower-right
    {0.8, 0.0, 0.24, 0.04},     // 3 high angle, top third
    {0.45, 0.14, -0.08, -0.01}, // 4 near, right
    {0.15, -0.04, -0.22, 0.02}, // 5 low close, bottom third
    {0.65, 0.1, 0.1, -0.03},    // 6 near diagonal
};
static const int STAGE_COUNT = (int)(sizeof STAGES / sizeof STAGES[0]);

// The bank and swing carried by each flight.
static const double FLY_YAW[] = {0.05, -0.07, 0.03, -0.05, 0.06, -0.03, 0.04};
static const double FLY_ROLL[] = {0.03, -0.04, 0.05, -0.02, 0.04, -0.05, 0.02};
static const int FLY_COUNT = (int)(sizeof FLY_YAW / sizeof FLY_YAW[0]);

// The vocabulary of a move; one nonzero axis each, insertion order = the
// order the rng draws kinds in.
struct MoveSpec {
    const char* name;
    double d_roll, d_yaw, d_r, d_e, dx, dy;
};
static const MoveSpec MOVES[] = {
    {"rollL", -1, 0, 0, 0, 0, 0}, {"rollR", 1, 0, 0, 0, 0, 0},
    {"yawL", 0, -1, 0, 0, 0, 0},  {"yawR", 0, 1, 0, 0, 0, 0},
    {"push", 0, 0, -1, 0, 0, 0},  {"pull", 0, 0, 1, 0, 0, 0},
    {"crane", 0, 0, 0, 1, 0, 0},  {"tilt", 0, 0, 0, -1, 0, 0},
    {"down", 0, 0, 0, 0, 0, -1},  {"up", 0, 0, 0, 0, 0, 1},
    {"right", 0, 0, 0, 0, 1, 0},  {"left", 0, 0, 0, 0, -1, 0},
};
static const int MOVE_COUNT = (int)(sizeof MOVES / sizeof MOVES[0]);
static const char* EASE_NAMES[] = {"ease_in_out_cubic", "ease_out_quad", "ease_in_quad",
                                   "smooth"};

struct ChoreoMove {
    string kind;
    double mag = 0, lead = 0, pace = 0;
    string ease;
    int spec_idx = 0;  // row in MOVES (kind's lookup index)
};

// One line's phrase: an optional kick on the beat itself, then one move per
// gap between word groups (js/layout.mjs rollChoreo).
struct Phrase {
    ChoreoMove kick;
    bool has_kick = false;
    vector<ChoreoMove> between;
};

// Roll one move: which axis, how far, which ease, and whether it leads. The
// magnitude band follows the axis: banks 0.05-0.16, swings 0.07-0.20,
// dolly 0.5-1.5, tilt 0.02-0.07, pans 0.06-0.15.
static ChoreoMove roll_move(JsRng& r) {
    int kidx = (int)floor(r.next() * MOVE_COUNT);
    if (kidx >= MOVE_COUNT) kidx = MOVE_COUNT - 1;
    if (kidx < 0) kidx = 0;
    const MoveSpec& spec = MOVES[kidx];
    ChoreoMove m;
    m.kind = spec.name;
    m.spec_idx = kidx;
    if (spec.d_roll != 0)
        m.mag = 0.05 + r.next() * 0.11;
    else if (spec.d_yaw != 0)
        m.mag = 0.07 + r.next() * 0.13;
    else if (spec.d_r != 0)
        m.mag = 0.5 + r.next() * 1.0;
    else if (spec.d_e != 0)
        m.mag = 0.02 + r.next() * 0.05;
    else
        m.mag = 0.06 + r.next() * 0.09;
    m.ease = EASE_NAMES[min((size_t)floor(r.next() * 4), (size_t)3)];
    // > 0 the camera settles before the word lands, < 0 it trails it.
    m.lead = -0.16 + r.next() * 0.42;
    // Fraction of the gap to the next beat the move is allowed to spend.
    m.pace = 0.5 + r.next() * 0.35;
    return m;
}

// One line's phrase: an optional kick on the beat itself, then one move per
// gap between word groups.
static Phrase roll_choreo(int groups, int seed) {
    JsRng r(seed);
    Phrase p;
    if (r.next() < 0.72) {
        p.kick = roll_move(r);
        p.has_kick = true;
    }
    for (int j = 1; j < groups; j++) p.between.push_back(roll_move(r));
    return p;
}

static string signature_of(const Phrase& ch) {
    string s = ch.has_kick ? ch.kick.kind : "-";
    s += ">";
    for (size_t j = 0; j < ch.between.size(); j++) {
        if (j) s += ",";
        s += ch.between[j].kind;
    }
    return s;
}

// Give every line its own phrase; signatures are unique by construction (a
// collision re-rolls that line with a new salt). Line 0 keeps the hand-written
// opening in front of its roll.
static vector<Phrase> assign_choreo(const vector<int>& group_counts) {
    vector<string> used;
    vector<Phrase> out;
    for (size_t i = 0; i < group_counts.size(); i++) {
        int k = group_counts[i];
        bool is_first = i == 0;
        int need = k - 1 > 0 ? k - 1 : 0;
        ChoreoMove prefix_kick;
        vector<ChoreoMove> prefix_between;
        int kept = 0;
        if (is_first) {
            prefix_kick.kind = "down";
            prefix_kick.spec_idx = 8;  // MOVES: down
            prefix_kick.mag = 0.34;
            prefix_kick.ease = "ease_out_quad";
            prefix_kick.lead = -0.12;
            prefix_kick.pace = 0.85;
            ChoreoMove b0;
            b0.kind = "up";
            b0.spec_idx = 9;
            b0.mag = 0.13;
            b0.ease = "smooth";
            b0.lead = 0.1;
            b0.pace = 0.6;
            ChoreoMove b1;
            b1.kind = "right";
            b1.spec_idx = 10;
            b1.mag = 0.31;
            b1.ease = "ease_in_out_cubic";
            b1.lead = 0.14;
            b1.pace = 0.8;
            prefix_between = {b0, b1};
            kept = (int)min(prefix_between.size(), (size_t)need);
        }
        bool placed = false;
        for (int salt = 0; salt < 80 && !placed; salt++) {
            JsRng r(to_uint32((double)i * 7919.0 + (double)salt * 104729.0 + 17.0));
            Phrase ch;
            if (is_first) {
                ch.kick = prefix_kick;
                ch.has_kick = true;
                for (int j = 0; j < kept; j++) ch.between.push_back(prefix_between[j]);
                for (int j = kept; j < need; j++) ch.between.push_back(roll_move(r));
            } else {
                if (r.next() < 0.72) {
                    ch.kick = roll_move(r);
                    ch.has_kick = true;
                }
                for (int j = 0; j < need; j++) ch.between.push_back(roll_move(r));
            }
            string sig = signature_of(ch);
            bool dup = false;
            for (const auto& u : used)
                if (u == sig) dup = true;
            if (!dup) {
                used.push_back(sig);
                out.push_back(move(ch));
                placed = true;
            }
        }
        if (!placed) out.push_back(roll_choreo(k, (int)i + 1));
    }
    return out;
}
