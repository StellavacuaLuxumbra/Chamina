// ---------------------------------------------------------------------------
// gen: field layout (port of js/layout.mjs layoutField) — serpentine world
// positions, per-cue camera stops and per-line exits. All double: the output
// is scene JSON, so it must round-trip exactly like node's, unlike the
// renderer's float pipeline.
// ---------------------------------------------------------------------------

// Serpentine grid (GRID + gridPositions): row 0 runs from the middle column to
// the end so line 0 lands on the world origin where the default camera already
// looks; rows 1.. then snake left and right.
static vector<d3> grid_positions(int n) {
    const int cols = 5;
    const double x_step = 30, y_step = 15;
    double half = (cols - 1) / 2.0;
    vector<d3> out;
    int row = 0, col = (int)half, dir = 1;
    for (int i = 0; i < n; i++) {
        out.push_back({(col - half) * x_step, row * y_step, 0.0});
        if (dir > 0) {
            if (col < cols - 1) col += 1;
            else {
                row += 1;
                dir = -1;
            }
        } else if (col > 0) {
            col -= 1;
        } else {
            row += 1;
            dir = 1;
        }
    }
    return out;
}

// Does the line still fit the frame from this framing? (framingFits) A rolled
// picture rotates the line's screen box; inflate the box by the roll's
// axis-aligned extent and test the four corners.
static bool framing_fits(const d3& P, const d3& target, double radius, double elevation,
                         double yaw, double roll, double hw, double hh, double th,
                         double aspect, double fov) {
    BasisD basis = camera_basis_d(radius, elevation, target, yaw);
    double r = roll != 0 && !isnan(roll) ? roll : 0.0;  // Math.cos(roll || 0)
    double c = fabs(cos(r)), s = fabs(sin(r));
    double ew = hw * c + hh * s;
    double eh = hh * c + hw * s;
    const d3 pts[4] = {{P[0] - ew, P[1] - eh, P[2]},
                       {P[0] + ew, P[1] - eh, P[2]},
                       {P[0] - ew, P[1] + eh, P[2]},
                       {P[0] + ew, P[1] + eh, P[2]}};
    for (const d3& pt : pts) {
        double x, y;
        if (!project_ndc_d(basis, pt, fov, aspect, x, y)) return false;
        // 7% of the frame is kept back for the outline rim and the bloom bleed.
        if (fabs(x) > 0.93 || fabs(y) > 0.94) return false;
    }
    return true;
}

// Is the axis-aligned box around P (half extents hw/hh) on screen?
// (boxOnFrame — corners behind the eye are tolerated, a box entirely behind
// the eye is not.)
static bool box_on_frame(double radius, double elevation, const d3& target, double yaw,
                         double roll, const d3& P, double hw, double hh, double fov,
                         double aspect) {
    BasisD basis = camera_basis_d(radius, elevation, target, yaw);
    double r = roll != 0 && !isnan(roll) ? roll : 0.0;
    double c = fabs(cos(r)), s = fabs(sin(r));
    double ew = hw * c + hh * s;
    double eh = hh * c + hw * s;
    bool behind = false, any = false;
    double min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
    const d3 pts[4] = {{P[0] - ew, P[1] - eh, P[2]},
                       {P[0] + ew, P[1] - eh, P[2]},
                       {P[0] - ew, P[1] + eh, P[2]},
                       {P[0] + ew, P[1] + eh, P[2]}};
    for (const d3& pt : pts) {
        double x, y;
        if (!project_ndc_d(basis, pt, fov, aspect, x, y)) {
            behind = true;
            continue;
        }
        any = true;
        min_x = js_min(min_x, x);
        max_x = js_max(max_x, x);
        min_y = js_min(min_y, y);
        max_y = js_max(max_y, y);
    }
    if (!any) return false;   // every corner behind the eye
    if (behind) return true;  // straddling the eye: count it as seen
    return !(max_x < -1 || min_x > 1 || max_y < -1 || min_y > 1);
}

struct FieldLayout {
    vector<d3> positions;
    vector<double> exits;  // NaN = leave the block's own timing (--keep-open)
    ojson stops = ojson::array();
};

// Lay the lines out as a camera path (layoutField). See js/layout.mjs for the
// full narration; formulas are kept line-for-line. Energy loading lives with
// the CLI section (load_energy) — it reads a file, the layout itself is pure.
static FieldLayout layout_field(const vector<LrcLine>& lines, const ojson& camera,
                                double duration, double fov, double aspect, double size,
                                double fit_width, double fade_in, double outro_at,
                                double title_out, bool groups_auto, double whip,
                                const vector<double>* energy,
                                const vector<bool>* whip_force) {
    const int n = (int)lines.size();
    FieldLayout out;
    if (!n) return out;

    vector<double> cues(n);
    for (int i = 0; i < n; i++) cues[i] = lines[i].time;
    out.positions = grid_positions(n);

    // Work from the rig without stops: the stops we emit are built on the
    // orbit + movement, and cameraAnimation never has any yet when this runs.
    ojson rig = camera;
    rig.erase("stops");

    double th = tan(fov / 2);
    auto half_w_at = [&](double r) { return th * aspect * r; };
    auto half_h_at = [&](double r) { return th * r; };

    // The tightest the frame ever gets. The v9 rig stays within ±1.3 of the
    // orbit's own radius at that cue; framingFits vetoes anything past the edge.
    double base_r;
    {
        double r = camera.contains("radius") && camera["radius"].is_number()
                       ? camera["radius"].get<double>()
                       : numeric_limits<double>::quiet_NaN();
        base_r = r != 0 && !isnan(r) ? r : 14.0;  // Number(camera.radius) || 14
    }
    double rmax_cap = js_min(16.0, base_r + 1.3);  // rmax=16 (default)

    // World half-extents of each line, matching how Rust resolves fit_width.
    vector<double> box_w(n), box_h(n), reveal_end(n);
    for (int i = 0; i < n; i++) {
        double em = em_width(lines[i].text);
        double width = em > 0 ? js_min(em * size, fit_width) : 0.0;
        double resolved = em > 0 ? width / em : size;
        box_w[i] = width / 2;
        box_h[i] = resolved * 0.55;  // half the em box + rim/bloom margin
        const vector<double>* ctp = lines[i].has_ct ? &lines[i].ct : nullptr;
        reveal_end[i] = ctp && !ctp->empty()
                            ? js_max_of(*ctp)   // Math.max(...charTimes)
                            : cues[i];
    }

    // Return to the title framing before the outro.
    double tail = isfinite(outro_at) ? outro_at : duration;
    double last_cue = cues[n - 1];
    double back_at = tail - 12;
    if (back_at < last_cue + 0.6) back_at = last_cue + 0.6;
    double back_dur = tail - back_at;

    // Flights in, one per line — computed first, because a line's own phrase
    // may never run into the flight that carries the camera to the next one.
    vector<double> flight_at(n), flight_dur(n);
    for (int i = 0; i < n; i++) {
        double next = i + 1 < n ? cues[i + 1] : duration;
        double gap = js_max(next - cues[i], 0.2);
        double capped = js_min(14.0, gap - 0.4);
        double d = js_max(1.2, js_min(js_min(gap * 0.45, capped), 2.0));  // TRAVEL_MAX
        double start = cues[i] - d;
        // One stop may never begin before the previous cue; the opening card
        // holds still until it is gone.
        if (i == 0 && isfinite(title_out)) start = js_max(start, title_out);
        else if (i > 0) start = js_max(start, cues[i - 1] + 0.8);
        start = js_min(start, cues[i] - 0.4);
        flight_at[i] = start;
        flight_dur[i] = js_max(cues[i] - start, 0.4);
    }

    // Whip rolls on the flights. Only energetic lines earn one, and not all of
    // them; the sign is handed out at emission time so flight whips and word
    // whips share one alternation.
    auto energy_at = [&](int i) -> double {
        double v = energy && i < (int)energy->size() ? (*energy)[i] : 0.5;
        v = clamp_d(v, 0.0, 1.0);
        return isnan(v) ? 0.0 : v;  // clamp(NaN) is NaN, `|| 0` fixes it
    };
    auto force_of = [&](int i) -> bool {
        return whip_force && i < (int)whip_force->size() && (*whip_force)[i];
    };
    vector<double> flight_whip(n, 0.0);
    double whip_sign = 0;
    if (whip > 0) {
        for (int i = 0; i < n; i++) {
            JsRng r(to_uint32((double)i * 2654435761.0 + 101.0));
            double e = energy_at(i);
            bool forced = force_of(i);
            // The r() draw is only consumed when the meter is actually asked
            // (JS && short-circuit) — the stream must stay aligned with node's.
            if ((!forced && e < 0.52) || flight_dur[i] < 1.0 || (!forced && r.next() < 0.22))
                continue;
            flight_whip[i] =
                (M_PI / 2) * (0.8 + 0.2 * r.next()) * js_min(whip, 1.0) * (0.72 + 0.28 * e);
        }
    }

    // Per-word choreography: with groups auto every line is cut into words that
    // land together, each gap between them getting its own camera move.
    vector<vector<WordGroup>> group_lists;
    vector<Phrase> choreo;
    if (groups_auto) {
        for (int i = 0; i < n; i++) {
            vector<WordGroup> g =
                word_groups(lines[i].text, lines[i].has_ct ? lines[i].ct : vector<double>(), i);
            if (g.empty()) g.push_back({0, 1, cues[i], -1});
            group_lists.push_back(move(g));
        }
        vector<int> counts;
        for (const auto& g : group_lists) counts.push_back((int)g.size());
        choreo = assign_choreo(counts);
        // The last glyph of a line now lands with its group, so the exit scan
        // waits for the group rather than for the glyph it used to wait for.
        for (int i = 0; i < n; i++) {
            const WordGroup& g = group_lists[i].back();
            reveal_end[i] = g.t + js_max(0.0, (double)(g.i1 - g.i0 - 1)) * 0.035;
        }
    }

    vector<double> line_ends(n, 0.0);
    for (int i = 0; i < n; i++) {
        const Stage& stage = STAGES[i % STAGE_COUNT];
        double base_radius, base_elevation, base_yaw, base_roll;
        d3 base_target;
        eval_orbit_d(rig, cues[i], base_radius, base_elevation, base_target, base_yaw,
                     base_roll);
        // The zoom budget rides with the orbit: ±1.3 around wherever the long
        // dolly has the camera at this cue.
        double line_base = base_radius;
        double r_lo = js_max(11.6, line_base - 1.3);
        double r_hi = js_max(r_lo + 0.8, js_min(rmax_cap, line_base + 1.3));
        double radius = clamp_d(line_base + stage.r_add, r_lo, r_hi);
        double elevation = clamp_d(base_elevation + stage.el, 0.0, 0.42);
        double fw = half_w_at(radius);
        double fh = half_h_at(radius);
        // Room the frame actually has once the line's half-width and a rim/bloom
        // margin are taken out.
        double room_x = js_max(0.0, 1 - 0.15 - box_w[i] / fw);
        double room_y = js_max(0.0, 1 - box_h[i] / fh - 0.1);
        double sx = clamp_d(stage.sx, -room_x, room_x);
        double sy = clamp_d(stage.sy, -room_y, room_y);

        // Screen basis at the cue with target at the origin — orientation only,
        // which is exactly what an independent slot needs.
        BasisD basis = camera_basis_d(radius, elevation, {0, 0, 0}, base_yaw);
        const d3& P = out.positions[i];
        d3 off = {sx * fw * basis.right[0] + sy * fh * basis.up[0],
                  sx * fw * basis.right[1] + sy * fh * basis.up[1],
                  sx * fw * basis.right[2] + sy * fh * basis.up[2]};
        d3 slot = {P[0] - off[0], P[1] - off[1], P[2] - off[2]};
        double fly_yaw = FLY_YAW[i % FLY_COUNT];
        double fly_roll = FLY_ROLL[i % FLY_COUNT];
        // The slot was solved for the plain orbit; if the banked framing cannot
        // hold the line, fly it level instead of shrinking the shot.
        if (!framing_fits(P, slot, radius, elevation, base_yaw + fly_yaw, fly_roll, box_w[i],
                          box_h[i], th, aspect, fov)) {
            fly_yaw = 0;
            fly_roll = 0;
        }

        // A whipped transition is three stops: bank in while the outgoing
        // framing holds, fly while the bank unwinds past level (back_out
        // overshoots — the whip-crack), then settle level just after the beat.
        double whip_mag = flight_whip[i];
        double flight_settle_end = flight_at[i] + flight_dur[i];
        if (whip_mag != 0) {
            whip_sign = whip_sign == 0 ? 1 : -whip_sign;
            double signed_mag = whip_mag * whip_sign;
            double dA = js_max(0.5, flight_dur[i] * 0.55);
            double dB = js_max(0.45, flight_dur[i] - dA);
            double gap_next = (i + 1 < n ? cues[i + 1] : duration + 10) - cues[i];
            double settle_clamp =
                js_min(js_min(0.55, js_max(0.12, gap_next - 0.88)),
                       back_at - (cues[i] + 0.08) - 0.02);
            {
                ojson s = ojson::object();
                s["at"] = round3(flight_at[i]);
                s["duration"] = round3(dA);
                s["ease"] = "ease_in_quad";
                s["roll"] = round3(signed_mag);
                out.stops.push_back(move(s));
            }
            {
                ojson s = ojson::object();
                s["at"] = round3(flight_at[i] + dA);
                s["duration"] = round3(dB);
                s["ease"] = "back_out";
                s["target"] = ojson::array({round3(slot[0]), round3(slot[1]), round3(slot[2])});
                s["radius"] = round3(radius);
                s["elevation"] = round3(elevation);
                s["yaw"] = round3(fly_yaw);
                s["roll"] = round3(fly_roll);
                out.stops.push_back(move(s));
            }
            if (settle_clamp >= 0.1) {
                ojson s = ojson::object();
                s["at"] = round3(cues[i] + 0.08);
                s["duration"] = round3(settle_clamp);
                s["ease"] = "ease_out_quad";
                s["roll"] = 0;
                out.stops.push_back(move(s));
                flight_settle_end = cues[i] + 0.08 + settle_clamp;
            }
        } else {
            ojson s = ojson::object();
            s["at"] = round3(flight_at[i]);
            s["duration"] = round3(flight_dur[i]);
            s["ease"] = "ease_in_out_cubic";
            s["target"] = ojson::array({round3(slot[0]), round3(slot[1]), round3(slot[2])});
            s["radius"] = round3(radius);
            s["elevation"] = round3(elevation);
            s["yaw"] = round3(fly_yaw);
            s["roll"] = round3(fly_roll);
            out.stops.push_back(move(s));
        }

        double last_end = flight_settle_end;
        if (!groups_auto) {
            line_ends[i] = last_end;
            continue;
        }

        // The phrase: an optional kick on the beat itself, then one move per gap
        // between word groups.
        const vector<WordGroup>& groups_i = group_lists[i];
        double deadline = i + 1 < n ? flight_at[i + 1] : back_at;
        struct PlanItem {
            const ChoreoMove* m;
            double on;
            bool kick;
        };
        vector<PlanItem> plan;
        if (choreo[i].has_kick) plan.push_back({&choreo[i].kick, cues[i], true});
        for (size_t j = 1; j < groups_i.size(); j++)
            plan.push_back({&choreo[i].between[j - 1], groups_i[j].t, false});

        // One in-phrase whip on the widest word gap of a very energetic line —
        // at most one per line, and line 0 is always in (the opening is the
        // pitch). The sign is only spent when the pair is actually placed.
        int whip_gap_idx = -1;
        double whip_gap_scale = 0;
        if (whip > 0 && (energy_at(i) >= 0.78 || i == 0 || force_of(i)) && groups_i.size() > 2) {
            double best = 0;
            for (size_t j = 1; j < groups_i.size(); j++) {
                double gap_len = groups_i[j].t - groups_i[j - 1].t;
                if (gap_len > best) {
                    best = gap_len;
                    whip_gap_idx = (int)j - 1;
                }
            }
            if (best < 1.0) {
                whip_gap_idx = -1;
            } else {
                JsRng r2(to_uint32((double)i * 97.0 + 13.0));
                whip_gap_scale = i == 0 ? (M_PI / 2) * (0.85 + 0.1 * r2.next())
                                        : 0.5 + 0.35 * r2.next();
            }
        }

        double cur_r = radius, cur_e = elevation, cur_yaw = fly_yaw, cur_roll = fly_roll;
        for (int k = 0; k < (int)plan.size(); k++) {
            const ChoreoMove* m = plan[k].m;
            double on = plan[k].on;
            bool kick = plan[k].kick;
            double next_on = k + 1 < (int)plan.size() ? plan[k + 1].on : deadline + 0.35;
            double span = js_max(next_on - on, 0.18);

            // The in-phrase whip: bank in ahead of the word, unwind on it.
            // Falls through to the ordinary move when the gap turns out too
            // tight once the neighbouring stops have claimed their time.
            if (whip_gap_idx >= 0 && !kick && on == groups_i[whip_gap_idx + 1].t) {
                double dA = js_min(0.45, js_max(0.24, span * 0.3));
                double dB = js_min(0.55, js_max(0.28, span * 0.34));
                double at_a = js_max(last_end + 0.02, on - dA - 0.05);
                if (at_a + dA <= on - 0.01 && on + dB <= deadline - 0.02) {
                    whip_sign = whip_sign == 0 ? 1 : -whip_sign;
                    double mag = whip_gap_scale * js_min(whip, 1.0) * whip_sign;
                    {
                        ojson s = ojson::object();
                        s["at"] = round3(at_a);
                        s["duration"] = round3(dA);
                        s["ease"] = "ease_in_quad";
                        s["roll"] = round3(mag);
                        out.stops.push_back(move(s));
                    }
                    {
                        ojson s = ojson::object();
                        s["at"] = round3(on);
                        s["duration"] = round3(dB);
                        s["ease"] = "ease_out_quad";
                        s["roll"] = round3(cur_roll);
                        out.stops.push_back(move(s));
                    }
                    last_end = on + dB;
                    whip_gap_idx = -1;
                    continue;
                }
            }

            double dur = js_max(0.18, js_min(span * m->pace, 1.1));
            double at = kick ? on : on - m->lead - dur;
            at = js_max(at, last_end + 0.02);
            if (at + dur > deadline) dur = deadline - at;
            if (dur < 0.12) continue;

            const MoveSpec& spec = MOVES[m->spec_idx];
            double dx_n = spec.dx * m->mag;
            double dy_n = spec.dy * m->mag;
            // Shrink the move until the line is still comfortably inside the
            // frame at the new framing; six halvings are the budget, a move
            // that still fails is dropped rather than forced.
            double scale = 1.0;
            d3 T = slot;
            double n_r = cur_r, n_e = cur_e, n_y = cur_yaw, n_ro = cur_roll;
            bool ok = false;
            for (int tries = 0; tries < 6; tries++) {
                n_r = clamp_d(cur_r + spec.d_r * m->mag * scale, r_lo, r_hi);
                n_e = clamp_d(cur_e + spec.d_e * m->mag * scale, 0.0, 0.42);
                n_y = clamp_d(cur_yaw + spec.d_yaw * m->mag * scale, -0.26, 0.26);
                n_ro = clamp_d(cur_roll + spec.d_roll * m->mag * scale, -0.24, 0.24);
                double fw2 = half_w_at(n_r);
                double fh2 = half_h_at(n_r);
                // With the eye swung around the line, the pan direction turns too.
                BasisD b2 = camera_basis_d(n_r, n_e, {0, 0, 0}, base_yaw + n_y);
                T = {slot[0] + dx_n * scale * fw2 * b2.right[0] + dy_n * scale * fh2 * b2.up[0],
                     slot[1] + dx_n * scale * fw2 * b2.right[1] + dy_n * scale * fh2 * b2.up[1],
                     slot[2] + dx_n * scale * fw2 * b2.right[2] + dy_n * scale * fh2 * b2.up[2]};
                if (framing_fits(P, T, n_r, n_e, base_yaw + n_y, n_ro, box_w[i], box_h[i], th,
                                 aspect, fov)) {
                    ok = true;
                    break;
                }
                scale *= 0.5;
            }
            if (!ok) continue;
            // A move that ended up nowhere is not worth a stop.
            if (n_r == cur_r && n_e == cur_e && n_y == cur_yaw && n_ro == cur_roll &&
                fabs(T[0] - slot[0]) + fabs(T[1] - slot[1]) + fabs(T[2] - slot[2]) < 1e-3)
                continue;
            {
                ojson s = ojson::object();
                s["at"] = round3(at);
                s["duration"] = round3(dur);
                s["ease"] = m->ease;
                s["target"] = ojson::array({round3(T[0]), round3(T[1]), round3(T[2])});
                s["radius"] = round3(n_r);
                s["elevation"] = round3(n_e);
                s["yaw"] = round3(n_y);
                s["roll"] = round3(n_ro);
                out.stops.push_back(move(s));
            }
            cur_r = n_r;
            cur_e = n_e;
            cur_yaw = n_y;
            cur_roll = n_ro;
            last_end = at + dur;
        }
        line_ends[i] = last_end;
    }

    // Closing return to the title framing; the card is never banked.
    if (back_dur >= 0.4) {
        double br, be, byaw, broll;
        d3 bt;
        eval_orbit_d(rig, js_min(tail, duration), br, be, bt, byaw, broll);
        ojson s = ojson::object();
        s["at"] = round3(back_at);
        s["duration"] = round3(back_dur);
        s["ease"] = "ease_in_out_cubic";
        s["target"] = ojson::array({round3(bt[0]), round3(bt[1]), round3(bt[2])});
        s["radius"] = round3(br);
        s["elevation"] = round3(be);
        s["yaw"] = 0;
        s["roll"] = 0;
        out.stops.push_back(move(s));
    }

    // First moment each line is no longer in the shot: scan the camera path.
    ojson cam = rig;
    cam["stops"] = out.stops;
    for (int i = 0; i < n; i++) {
        const d3& P = out.positions[i];
        double from = js_max(js_max(cues[i] + fade_in + 0.15, reveal_end[i] + 0.15),
                             line_ends[i] + 0.15);
        double exit = numeric_limits<double>::quiet_NaN();
        for (double t = from; t <= duration; t += 0.1) {
            double br, be, byaw, broll;
            d3 bt;
            eval_orbit_d(cam, t, br, be, bt, byaw, broll);
            if (!box_on_frame(br, be, bt, byaw, broll, P, box_w[i], box_h[i], fov, aspect)) {
                exit = round3(t);
                break;
            }
        }
        out.exits.push_back(exit);
    }

    return out;
}


struct LyricOpts {
    double size = numeric_limits<double>::quiet_NaN();  // clampNum → 1.5
    double y = numeric_limits<double>::quiet_NaN();     // isfinite ? : -0.4
    double depth = numeric_limits<double>::quiet_NaN(); // clampNum → 0.09
    string color;
    double strength = numeric_limits<double>::quiet_NaN();
    double offset = numeric_limits<double>::quiet_NaN(); // isfinite ? : 0
    double fade_in = numeric_limits<double>::quiet_NaN();
    double fade_out = numeric_limits<double>::quiet_NaN();
    double stagger = numeric_limits<double>::quiet_NaN();
    double total = numeric_limits<double>::quiet_NaN();
    string entrance;
    double fit_width = numeric_limits<double>::quiet_NaN();
    bool groups_auto = false;  // --groups auto: word stamps compress onto groups
    const ojson* jitter = nullptr;
    const ojson* wave = nullptr;
};

static void push_entrance(ojson& anims, const string& entrance, double t, double stagger) {
    // Entrance layers land before the fade in the array (JS unshifts); both
    // share start_at so the line slides/drops in while it fades up.
    if (entrance == "slide") {
        ojson a = ojson::object();
        a["type"] = "slide";
        a["delta"] = ojson::array({-6.0, -2.5, 0.0});
        a["duration"] = 0.7;
        a["stagger"] = stagger * 1.5;
        a["ease"] = "ease_out_quad";
        a["start_at"] = t;
        anims.push_back(move(a));
    } else if (entrance == "fall") {
        ojson a = ojson::object();
        a["type"] = "fall";
        a["duration"] = 0.7;
        a["stagger"] = stagger * 1.5;
        a["height"] = 1.4;
        a["gravity"] = -3.0;
        a["drift"] = 0.0;
        a["wobble"] = 0.0;
        a["start_at"] = t;
        anims.push_back(move(a));
    } else if (entrance == "pop") {
        ojson a = ojson::object();
        a["type"] = "pop";
        a["duration"] = 0.5;
        a["stagger"] = stagger * 1.5;
        a["start_at"] = t;
        a["from"] = 0.0;
        a["ease"] = "back_out";
        anims.push_back(move(a));
    } else if (entrance == "overshoot") {
        // Two layers on purpose: slide contributes position, pop scale.
        // back_out overshoots, so each lands past its rest pose.
        ojson a = ojson::object();
        a["type"] = "slide";
        a["delta"] = ojson::array({-7.0, -3.0, 0.0});
        a["duration"] = 0.75;
        a["stagger"] = stagger * 1.5;
        a["ease"] = "back_out";
        a["start_at"] = t;
        anims.push_back(move(a));
        ojson b = ojson::object();
        b["type"] = "pop";
        b["duration"] = 0.6;
        b["stagger"] = stagger * 1.5;
        b["start_at"] = t;
        b["from"] = 0.0;
        b["ease"] = "back_out";
        anims.push_back(move(b));
    }
}

static vector<ojson> build_lyric_texts(const vector<LrcLine>& lines, const LyricOpts& o) {
    double size = clamp_num(o.size, 1.5, 0.2, 12);
    double y = isfinite(o.y) ? o.y : -0.4;
    double depth = clamp_num(o.depth, 0.09, 0.01, 2);
    string color = o.color.empty() ? "#ffffff" : o.color;
    double strength = clamp_num(o.strength, 1.6, 0, 64);
    double offset = isfinite(o.offset) ? o.offset : 0.0;
    double fade_in = clamp_num(o.fade_in, 0.3, 0, 10);
    double fade_out = clamp_num(o.fade_out, 0.45, 0, 10);
    double stagger = clamp_num(o.stagger, 0.04, 0, 1);
    double total = o.total;  // NaN (JS null) when absent
    string base_entrance = o.entrance.empty() ? "none" : o.entrance;
    check_entrance(base_entrance);
    // World-space width budget; Rust resolves it against real font metrics
    // at export time, so we only say how much of the frame a line may use.
    double fit = isfinite(o.fit_width) && o.fit_width > 0 ? o.fit_width
                                                          : numeric_limits<double>::quiet_NaN();

    vector<ojson> out;
    int lyric_index = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        const LrcLine& line = lines[i];
        if (line.text.empty()) continue;

        string entrance = line_entrance(line, base_entrance);
        double t = round3(line.time + offset);
        // Word-timed lines light up glyph by glyph, so the fade-out waits for
        // the last one. (JS slices charTimes to text.length in UTF-16 units;
        // the array is built one-per-codepoint, so the slice never cuts.)
        vector<double> ct;
        bool has_ct = false;
        if (line.has_ct) {
            has_ct = true;
            for (double v : line.ct) ct.push_back(round3(v + offset));
            // --groups auto: stamps compress onto their word group, then sweep
            // the glyphs inside a group 35 ms apart (groupCharTimes).
            if (o.groups_auto) {
                vector<double> g = group_char_times(line.text, ct, lyric_index);
                ct.clear();
                for (double v : g) ct.push_back(round3(v));
            }
        }
        double last_glyph = has_ct && !ct.empty() ? js_max_of(ct)   // Math.max
                                                  : t;
        double next_raw = i + 1 < lines.size() ? lines[i + 1].time + offset : total;
        double next = isfinite(next_raw) ? next_raw : t + 3.0;
        // Never let a line outlive the next cue; always leave room to fade.
        // (opts.hold is never set by the CLI.)
        double out_at = round3(js_max(t + 0.1, js_min(next, t + 1e9) - fade_out));
        if (has_ct) out_at = round3(js_max(out_at, last_glyph + 0.15));

        ojson anims = ojson::array();
        push_entrance(anims, entrance, t, stagger);
        ojson fade = ojson::object();
        fade["type"] = "fade";
        fade["duration"] = fade_in;
        fade["stagger"] = stagger;
        fade["start_at"] = t;
        fade["fade_out_at"] = out_at;
        fade["fade_out"] = fade_out;
        anims.push_back(move(fade));
        ojson jit = build_jitter(o.jitter, lyric_index, t);
        if (!jit.is_null()) anims.push_back(move(jit));
        ojson wav = build_wave(o.wave, lyric_index, t);
        if (!wav.is_null()) anims.push_back(move(wav));

        ojson block = ojson::object();
        block["text"] = line.text;
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
        if (isfinite(fit)) block["fit_width"] = fit;
        // One absolute start time per glyph; it rides on the block because
        // fade, entrance and wobble all fall on the same rhythm.
        if (has_ct) block["char_times"] = ct;
        out.push_back(move(block));
        lyric_index += 1;
    }
    return out;
}
