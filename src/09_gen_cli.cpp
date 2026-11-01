// ---------------------------------------------------------------------------
// gen: CLI (parseArgs + main)
// ---------------------------------------------------------------------------

static void gen_usage() {
    fputs(
        "chamina-cpp gen <song.lrc> [options]\n"
        "  LRC → scene JSON, single layout\n"
        "  -o, --out <file>       output scene JSON (default <lrc>.json)\n"
        "  -t, --template <file>  stage to reuse (default examples/gift.json, cwd-relative)\n"
        "  text:   --size --y --depth --color --strength --outline --outline-width\n"
        "  motion: --entrance --jitter --jitter-rot --jitter-freq --jitter-hit\n"
        "          --jitter-hit-dur --wave --wave-freq --wave-spacing --wave-hit\n"
        "          --wave-hit-dur\n"
        "  camera: --dolly --crane --move-at --move-dur --shake --shake-roll\n"
        "          --shake-freq --shake-hit --shake-hit-dur --yaw-drift --radius\n"
        "          --keep-camera\n"
        "  timing: --offset --fade-in --fade-out --stagger --tail --duration\n"
        "          --fit-width --keep-open\n"
        "  cards:  --title --subtitle --title-in --title-out --title-size\n"
        "          --outro-at --outro-text --outro-subtitle --outro-wish --outro-out\n"
        "  stage:  --bg-look --bg-cut --bg-fade --fx --fx-flash --fx-punch\n"
        "          --fx-vignette --fx-chromatic --fx-sat --bloom --fps --palette\n"
        "  field layout (--layout field): --roll-power --force-whip\n"
        "          --energy-file (per-line 0..1 audio energy, js/audio-env.mjs output)\n"
        "  -h, --help\n",
        stderr);
}

static void parse_gen_args(int argc, char** argv, GenArgs& o) {
    auto next = [&](int& i) -> string {
        i++;
        if (i >= argc) throw runtime_error(string(argv[i - 1]) + " needs a value");
        return argv[i];
    };
    // argv[0] is the "gen" token itself (JS parses process.argv.slice(2)).
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        OptNum n = OPT_NULL;
        if (a == "-o" || a == "--out") o.out = next(i);
        else if (a == "-t" || a == "--template") o.templ = next(i);
        else if (a == "--size") { n = optn(parse_num(next(i))); o.size = n; }
        else if (a == "--y") { n = optn(parse_num(next(i))); o.y = n; }
        else if (a == "--depth") { n = optn(parse_num(next(i))); o.depth = n; }
        else if (a == "--color") o.color = next(i);
        else if (a == "--strength") { n = optn(parse_num(next(i))); o.strength = n; }
        else if (a == "--outline") o.outline = next(i);
        else if (a == "--outline-width") { n = optn(parse_num(next(i))); o.outline_width = n; }
        else if (a == "--bloom") { n = optn(parse_num(next(i))); o.bloom = n; }
        else if (a == "--entrance") o.entrance = next(i);
        else if (a == "--layout") o.layout = next(i);
        else if (a == "--groups") o.groups = next(i);
        else if (a == "--jitter") { n = optn(parse_num(next(i))); o.jitter = n; }
        else if (a == "--jitter-rot") { n = optn(parse_num(next(i))); o.jitter_rot = n; }
        else if (a == "--jitter-freq") { n = optn(parse_num(next(i))); o.jitter_freq = n; }
        else if (a == "--jitter-hit") o.jitter_hit = true;
        else if (a == "--jitter-hit-dur") { n = optn(parse_num(next(i))); o.jitter_hit_dur = n; }
        else if (a == "--wave") { n = optn(parse_num(next(i))); o.wave = n; }
        else if (a == "--wave-freq") { n = optn(parse_num(next(i))); o.wave_freq = n; }
        else if (a == "--wave-spacing") { n = optn(parse_num(next(i))); o.wave_spacing = n; }
        else if (a == "--wave-hit") o.wave_hit = true;
        else if (a == "--wave-hit-dur") { n = optn(parse_num(next(i))); o.wave_hit_dur = n; }
        else if (a == "--dolly") { n = optn(parse_num(next(i))); o.dolly = n; }
        else if (a == "--crane") { n = optn(parse_num(next(i))); o.crane = n; }
        else if (a == "--move-at") { n = optn(parse_num(next(i))); o.move_at = n; }
        else if (a == "--move-dur") { n = optn(parse_num(next(i))); o.move_dur = n; }
        else if (a == "--shake") { n = optn(parse_num(next(i))); o.shake = n; }
        else if (a == "--shake-roll") { n = optn(parse_num(next(i))); o.shake_roll = n; }
        else if (a == "--shake-freq") { n = optn(parse_num(next(i))); o.shake_freq = n; }
        else if (a == "--shake-hit") o.shake_hit = true;
        else if (a == "--shake-hit-dur") { n = optn(parse_num(next(i))); o.shake_hit_dur = n; }
        else if (a == "--fit-width") { n = optn(parse_num(next(i))); o.fit_width = n; }
        else if (a == "--offset") { n = optn(parse_num(next(i))); o.offset = n; }
        else if (a == "--fade-in") { n = optn(parse_num(next(i))); o.fade_in = n; }
        else if (a == "--fade-out") { n = optn(parse_num(next(i))); o.fade_out = n; }
        else if (a == "--stagger") { n = optn(parse_num(next(i))); o.stagger = n; }
        else if (a == "--tail") { n = optn(parse_num(next(i))); o.tail = n; }
        else if (a == "--duration") { n = optn(parse_num(next(i))); o.duration = n; }
        else if (a == "--yaw-drift") { n = optn(parse_num(next(i))); o.yaw_drift = n; }
        else if (a == "--radius") { n = optn(parse_num(next(i))); o.radius = n; }
        else if (a == "--keep-camera") o.keep_camera = true;
        else if (a == "--keep-open") o.keep_open = true;
        else if (a == "--outro-at") { n = optn(parse_num(next(i))); o.outro_at = n; }
        else if (a == "--outro-text") { o.outro_text = next(i); o.has_outro_text = true; }
        else if (a == "--outro-subtitle") { o.outro_subtitle = next(i); o.has_outro_subtitle = true; }
        else if (a == "--outro-wish") o.outro_wish = next(i);
        else if (a == "--outro-out") { n = optn(parse_num(next(i))); o.outro_out = n; }
        else if (a == "--bg-look") o.bg_look = next(i);
        else if (a == "--bg-cut") o.bg_cut.push_back(next(i));
        else if (a == "--bg-fade") { n = optn(parse_num(next(i))); o.bg_fade = n; }
        else if (a == "--fx") o.fx = true;
        else if (a == "--fx-flash") { n = optn(parse_num(next(i))); o.fx_flash = n; }
        else if (a == "--fx-punch") { n = optn(parse_num(next(i))); o.fx_punch = n; }
        else if (a == "--fx-vignette") { n = optn(parse_num(next(i))); o.fx_vignette = n; }
        else if (a == "--fx-chromatic") { n = optn(parse_num(next(i))); o.fx_chromatic = n; }
        else if (a == "--fx-sat") { n = optn(parse_num(next(i))); o.fx_sat = n; }
        else if (a == "--palette") o.palette = next(i);
        else if (a == "--roll-power") { n = optn(parse_num(next(i))); o.roll_power = n; }
        else if (a == "--energy-file") o.energy_file = next(i);
        else if (a == "--force-whip") o.force_whip.push_back(next(i));
        else if (a == "--fps") { n = optn(parse_num(next(i))); o.fps = n; }
        else if (a == "--title") o.title = next(i);
        else if (a == "--subtitle") o.subtitle = next(i);
        else if (a == "--title-in") { n = optn(parse_num(next(i))); o.title_in = n; }
        else if (a == "--title-out") { n = optn(parse_num(next(i))); o.title_out = n; }
        else if (a == "--title-size") { n = optn(parse_num(next(i))); o.title_size = n; }
        else if (a == "-h" || a == "--help") o.help = true;
        else if (!a.empty() && a[0] != '-' && o.input.empty()) o.input = a;
        else throw runtime_error("unknown argument " + a);
    }
}

static int gen_main(int argc, char** argv) {
    GenArgs a;
    try {
        parse_gen_args(argc, argv, a);
    } catch (const exception& e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    if (a.help || a.input.empty()) {
        gen_usage();
        return a.help ? 0 : 1;
    }
    try {
        if (a.layout != "single" && a.layout != "field")
            throw runtime_error("--layout must be single or field (got \"" + a.layout + "\")");
        if (a.groups != "off" && a.groups != "auto")
            throw runtime_error("--groups must be off or auto (got \"" + a.groups + "\")");

        LrcParsed parsed = parse_lrc(read_file(a.input));
        // The [offset:] tag and --offset both nudge the whole song; the tag
        // runs first so the CLI flag stays an explicit authoring correction.
        double off = opt_num0(a.offset);
        vector<LrcLine> lines;
        for (const auto& l : apply_offset(parsed, false, 0.0)) {
            LrcLine o;
            o.time = l.time + off;
            o.text = l.text;
            o.has_tags = l.has_tags;
            o.tags = l.tags;
            if (l.has_ct) {
                o.has_ct = true;
                for (double v : l.ct) o.ct.push_back(v + off);
            }
            lines.push_back(move(o));
        }
        if (lines.empty())
            throw runtime_error("no timestamped lines in " + a.input);

        ojson tpl = ojson::parse(read_file(a.templ));
        double duration = opt_finite(a.duration)
                              ? a.duration.v
                              : suggest_duration(lines, a.tail.v);

        ojson camera_animation = ojson(nullptr);
        bool cam_absent = false;
        if (a.keep_camera) {
            if (tpl.contains("camera_animation"))
                camera_animation = tpl["camera_animation"];
            else
                cam_absent = true;
        } else {
            camera_animation = tune_orbit(tpl, duration, a);
        }
        if (!cam_absent) apply_camera_motion(camera_animation, duration, a);

        // Keep a margin: outline rim, bloom bleed and DOF softness all push
        // the line outward beyond its advance width.
        double vw = visible_width(tpl, cam_absent ? nullptr : &camera_animation);
        double fit_width = a.fit_width.is_null ? vw * 0.85 : a.fit_width.v;

        // Card times, derived before the layout runs.
        double outro_at = opt_finite(a.outro_at) && a.outro_at.v > 0
                              ? a.outro_at.v
                              : numeric_limits<double>::quiet_NaN();
        double outro_out;
        if (opt_finite(a.outro_out) && a.outro_out.v > 0)
            outro_out = a.outro_out.v;
        else
            outro_out = js_max((isfinite(outro_at) ? outro_at : 0.0) + 3.0, duration - 1.5);
        double first_cue = !lines.empty() ? lines[0].time : duration;
        double title_out;
        if (opt_finite(a.title_out) && a.title_out.v >= 0)
            title_out = a.title_out.v;
        else
            title_out = js_max(a.title_in.v + 2.0, first_cue - 1.2);

        // Only lines that actually render take part in the export.
        vector<LrcLine> lyric_lines;
        for (const auto& l : lines)
            if (!l.text.empty()) lyric_lines.push_back(l);

        // Where the music is loud. Read before the layout so the roll budget is
        // spent by the track itself; absent the flag no line is ever
        // "energetic" and the camera path is exactly the one v9 laid out.
        // (loadEnergy validates the envelope the same way single layout does.)
        vector<double> energy;
        if (!a.energy_file.empty()) energy = load_energy(a.energy_file, lyric_lines);
        // Lines the author named for the whip treatment no matter the meter.
        vector<bool> whip_force;
        if (!a.force_whip.empty()) {
            for (const auto& l : lyric_lines) {
                bool f = false;
                for (const auto& txt : a.force_whip)
                    if (l.text.find(txt) != string::npos) {
                        f = true;
                        break;
                    }
                whip_force.push_back(f);
            }
        }
        FieldLayout layout;
        if (a.layout == "field") {
            // Same clamping buildLyricTexts applies, so the frame budget is
            // computed against the size the glyphs really get.
            double size_c = clamp_num(a.size.v, 1.5, 0.2, 12);
            double fade_in_c = clamp_num(a.fade_in.v, 0.3, 0, 10);
            double fov = 0.8726646;
            if (tpl.contains("camera") && tpl["camera"].is_object() &&
                tpl["camera"].contains("fov") && tpl["camera"]["fov"].is_number()) {
                double f = tpl["camera"]["fov"].get<double>();
                if (f != 0 && !isnan(f)) fov = f;
            }
            double tw = tpl.contains("width") && tpl["width"].is_number()
                            ? tpl["width"].get<double>()
                            : 0.0;
            if (tw == 0) tw = 1920;
            double th_ = tpl.contains("height") && tpl["height"].is_number()
                             ? tpl["height"].get<double>()
                             : 0.0;
            if (th_ == 0) th_ = 1080;
            double aspect = tw / th_;
            double whip = opt_finite(a.roll_power) ? clamp_d(a.roll_power.v, 0.0, 1.0) : 0.0;
            layout = layout_field(
                lyric_lines, camera_animation, duration, fov, aspect, size_c, fit_width,
                fade_in_c, isfinite(outro_at) ? outro_at : numeric_limits<double>::quiet_NaN(),
                title_out, a.groups == "auto", whip,
                a.energy_file.empty() ? nullptr : &energy,
                a.force_whip.empty() ? nullptr : &whip_force);
            camera_animation["stops"] = layout.stops;
        }

        ojson jitter_obj = ojson(nullptr);
        if (opt_finite(a.jitter) && a.jitter.v > 0) {
            jitter_obj = ojson::object();
            jitter_obj["amplitude"] = a.jitter.v;
            jitter_obj["rotation"] = a.jitter_rot.v;
            jitter_obj["frequency"] = a.jitter_freq.v;
            jitter_obj["hit"] = a.jitter_hit;
            jitter_obj["hit_duration"] = a.jitter_hit_dur.v;
        }
        ojson wave_obj = ojson(nullptr);
        if (opt_finite(a.wave) && a.wave.v > 0) {
            wave_obj = ojson::object();
            wave_obj["amplitude"] = a.wave.v;
            wave_obj["frequency"] = a.wave_freq.v;
            wave_obj["spacing"] = a.wave_spacing.v;
            wave_obj["hit"] = a.wave_hit;
            wave_obj["hit_duration"] = a.wave_hit_dur.v;
        }

        LyricOpts lo;
        lo.size = a.size.v;
        lo.y = a.y.v;
        lo.depth = a.depth.v;
        lo.color = a.color;
        lo.strength = a.strength.v;
        lo.fade_in = a.fade_in.v;
        lo.fade_out = a.fade_out.v;
        lo.stagger = a.stagger.v;
        lo.entrance = a.entrance;
        lo.total = duration;
        lo.fit_width = fit_width;
        lo.jitter = jitter_obj.is_null() ? nullptr : &jitter_obj;
        lo.wave = wave_obj.is_null() ? nullptr : &wave_obj;
        lo.groups_auto = a.groups == "auto";
        vector<ojson> texts = build_lyric_texts(lyric_lines, lo);

        if (a.layout == "field") {
            // The blocks are all in the single slot buildLyricTexts put them in;
            // the layout owns their world position and the moment each one has
            // left frame.
            for (size_t i = 0; i < texts.size(); i++) {
                texts[i]["position"] = ojson::array({layout.positions[i][0],
                                                     layout.positions[i][1],
                                                     layout.positions[i][2]});
                ojson* fade = find_fade(texts[i]);
                if (fade && isfinite(layout.exits[i]))
                    (*fade)["fade_out_at"] = layout.exits[i];
            }
        }

        if (!a.outline.empty()) {
            for (auto& t : texts) {
                ojson outline = ojson::object();
                outline["color"] = a.outline;
                outline["width"] = a.outline_width.v;
                outline["strength"] = 1.0;
                t["material"]["outline"] = move(outline);
            }
        }
        // Per-line colour cycle; runs after the rim exists so the rim follows
        // each line's own tint. Cards are built later and keep --color.
        if (!a.palette.empty()) apply_palette(texts, a.palette);

        if (a.keep_open && !texts.empty()) {
            ojson* fade = find_fade(texts.back());
            if (fade) {
                fade->erase("fade_out_at");
                fade->erase("fade_out");
            }
        }

        // Outro card. The last cue lands far before the track ends, so the
        // held last line hands the frame over at outro-at - 0.6.
        if (isfinite(outro_at) && !texts.empty()) {
            ojson* fade = find_fade(texts.back());
            if (fade && fade->contains("start_at") && (*fade)["start_at"].is_number()) {
                double hand_off = round3(
                    js_max((*fade)["start_at"].get<double>() + 2.0, outro_at - 0.6));
                double foa = fade->contains("fade_out_at") && (*fade)["fade_out_at"].is_number()
                                 ? (*fade)["fade_out_at"].get<double>()
                                 : numeric_limits<double>::quiet_NaN();
                if (!isfinite(foa) || foa > hand_off) {
                    (*fade)["fade_out"] = a.fade_out.v;
                    (*fade)["fade_out_at"] = hand_off;
                }
            }
        }

        // Opening card; the outro card(s) close the video.
        double title_size = opt_clamp(a.title_size, 0.2, 12, 3);
        string card_entrance = a.entrance == "none" ? "slide" : a.entrance;
        TitleOpts common;
        common.color = a.color;
        common.strength = a.strength.v;
        common.outline = a.outline;
        common.outline_width = a.outline_width.v;
        common.start_at = a.title_in.v;
        common.out_at = title_out;
        // A card with `none` still reads better as a slide.
        common.entrance = card_entrance;
        common.stagger = 0.05;
        vector<ojson> cards;
        auto add_card = [&](const TitleOpts& t) {
            ojson b = build_title_block(t);
            if (!b.is_null()) cards.push_back(move(b));
        };
        {
            TitleOpts t = common;
            t.text = a.title;
            t.size = title_size;
            t.y = 1.2;
            t.fit_width = fit_width * 0.72;
            add_card(t);
        }
        {
            TitleOpts t = common;
            t.text = a.subtitle;
            t.size = title_size / 3.0;
            t.y = -0.6;
            t.strength = a.strength.v * 0.7;
            t.fit_width = fit_width * 0.5;
            t.stagger = 0.03;
            add_card(t);
        }
        if (isfinite(outro_at)) {
            {
                TitleOpts t = common;
                t.text = a.has_outro_text ? a.outro_text : a.title;
                t.size = title_size;
                t.y = 1.2;
                t.fit_width = fit_width * 0.72;
                t.start_at = outro_at;
                t.out_at = outro_out;
                t.fade_out = 1.4;
                add_card(t);
            }
            {
                TitleOpts t = common;
                t.text = a.has_outro_subtitle ? a.outro_subtitle : a.subtitle;
                t.size = title_size / 3.0;
                t.y = -0.6;
                t.strength = a.strength.v * 0.7;
                t.fit_width = fit_width * 0.5;
                t.stagger = 0.03;
                t.start_at = round3(outro_at + 0.35);
                t.out_at = outro_out;
                t.fade_out = 1.4;
                add_card(t);
            }
            if (!a.outro_wish.empty()) {
                TitleOpts t = common;
                t.text = a.outro_wish;
                t.size = title_size / 4.0;
                t.y = -1.7;
                t.strength = a.strength.v * 0.6;
                t.fit_width = fit_width * 0.45;
                t.stagger = 0.05;
                t.start_at = round3(outro_at + 15.0);
                t.out_at = outro_out;
                t.fade_out = 1.4;
                add_card(t);
            }
        }

        ojson scene = tpl;
        scene["duration"] = duration;
        // Sample the same timeline more sparsely; written only when asked for.
        if (opt_finite(a.fps) && a.fps.v > 0) scene["fps"] = a.fps.v;
        {
            bool wd = tpl.contains("work_dir") && js_truthy(tpl["work_dir"]);
            scene["work_dir"] = wd ? tpl["work_dir"] : ojson("target/chamina");
        }
        if (!cam_absent) scene["camera_animation"] = camera_animation;
        ojson bgs = build_backgrounds(a.bg_cut, a.bg_look, a.bg_fade.v);
        if (!bgs.is_null()) scene["backgrounds"] = move(bgs);
        if (a.fx)
            scene["fx"] = build_fx(a.fx_chromatic, a.fx_vignette, a.fx_sat, a.fx_flash,
                                   a.fx_punch);
        if (tpl.contains("bloom") && js_truthy(tpl["bloom"])) {
            ojson bloom = tpl["bloom"];
            // A full lyric line is far more emissive area than the gift card's
            // three glyphs — drop the preset intensity.
            bloom["intensity"] = a.bloom.v;
            scene["bloom"] = move(bloom);
        }
        if (tpl.contains("dof") && js_truthy(tpl["dof"])) {
            ojson dof = tpl["dof"];
            // DOF has to focus on the plane the text sits on; a --radius
            // override would otherwise leave the line permanently defocused.
            if (opt_gt0(a.radius)) dof["focal_distance"] = a.radius.v;
            scene["dof"] = move(dof);
        }
        ojson texts_all = ojson::array();
        for (auto& c : cards) texts_all.push_back(move(c));
        for (auto& t : texts) texts_all.push_back(move(t));
        scene["texts"] = move(texts_all);

        string out_path = a.out;
        if (out_path.empty()) {
            // JS: lrcPath.replace(/\.lrc$/i, '') + '.json'
            out_path = a.input;
            if (out_path.size() >= 4) {
                string tail4 = to_lower_ascii(out_path.substr(out_path.size() - 4));
                if (tail4 == ".lrc") out_path = out_path.substr(0, out_path.size() - 4);
            }
            out_path += ".json";
        }
        {
            error_code ec;
            filesystem::path op = filesystem::u8path(out_path);
            if (op.has_parent_path()) filesystem::create_directories(op.parent_path(), ec);
            string body;
            js_json(scene, body, 0);
            body += '\n';
            ofstream f(op, ios::binary);
            if (!f) throw runtime_error("cannot write " + out_path);
            f.write(body.data(), (streamsize)body.size());
            f.close();
            if (!f) throw runtime_error("cannot write " + out_path);
        }

        printf("%s\n", out_path.c_str());
        double fps_eff = 60.0;
        if (scene.contains("fps") && scene["fps"].is_number()) {
            double fv = scene["fps"].get<double>();
            if (fv != 0) fps_eff = fv;
        }
        double width = 1920, height = 1080;
        if (scene.contains("width") && scene["width"].is_number())
            width = scene["width"].get<double>();
        if (scene.contains("height") && scene["height"].is_number())
            height = scene["height"].get<double>();
        printf("  %d lines  → %ss  → %.0f frames  → %dx%d", (int)texts.size(),
               js_num_str(duration).c_str(), duration * fps_eff, (int)width, (int)height);
        if (!cards.empty())
            printf("  → title card %ss→%ss", js_num_str(a.title_in.v).c_str(),
                   js_num_str(title_out).c_str());
        printf("\n");
        string ti = parsed.meta.count("ti") ? parsed.meta.at("ti") : "";
        string ar = parsed.meta.count("ar") ? parsed.meta.at("ar") : "";
        if (!ti.empty() || !ar.empty())
            printf("  %s%s\n", ti.c_str(), ar.empty() ? "" : (" — " + ar).c_str());
        return 0;
    } catch (const exception& e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

static string find_ffmpeg() {
    // cmd.exe (which popen shells out to) rejects forward slashes in the
    // command position and mangles non-ASCII absolute paths, so the override
    // is normalised to backslashes. Otherwise rely on PATH.
    auto norm = [](string p) {
        for (auto& c : p)
            if (c == '/') c = '\\';
        return p;
    };
#ifdef _WIN32
    // getenv() hands back an ANSI string, but callers treat paths as UTF-8
    // (and CreateProcessW needs Unicode); read the wide environment directly.
    wchar_t buf[32767];
    DWORD n = GetEnvironmentVariableW(L"CHAMINA_FFMPEG", buf, 32767);
    if (n > 0 && n < 32767) {
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
        string s((size_t)len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, &s[0], len, nullptr, nullptr);
        return norm(s);
    }
#else
    if (const char* p = getenv("CHAMINA_FFMPEG")) return norm(p);
#endif
    return "ffmpeg";
}
