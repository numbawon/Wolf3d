// tests/test_config.cpp — Phase 5 WS4: config JSON parser/writer, v1
// schema tolerance, file I/O, CLI precedence (headless; no SDL, no game
// data; file tests use a temp dir, never %APPDATA%).
#include "app/config.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace w3d::app;

namespace {

struct Case {
    const char* name;
    void (*fn)();
};
std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}
struct Register {
    Register(const char* name, void (*fn)()) { cases().push_back({name, fn}); }
};

int g_checks = 0;
int g_failures = 0;
const char* g_current = "?";

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__,          \
                        g_current, #cond);                                     \
        }                                                                      \
    } while (0)

bool parse_ok(const std::string& text, JsonValue& out) {
    std::string err;
    return parse_json(text, out, err);
}
bool parse_bad(const std::string& text, JsonValue& out) {
    std::string err;
    return !parse_json(text, out, err);
}

Config full_config() {
    Config c;
    c.version = 1;
    c.video.res_scale = 4;
    c.video.scale_filter = 1;
    c.video.fullscreen_mode = 2;
    c.video.show_fps = false;
    c.audio.master_volume = 30;
    c.audio.music_volume = 50;
    c.audio.sfx_volume = 70;
    c.audio.music_on = false;
    c.audio.device_id = "DISPLAY 0";
    c.controls.mouse_sensitivity = 2.5;
    c.gameplay.difficulty = 2;
    c.gameplay.fog = false;
    return c;
}

// ------------------------------------------------------------ JSON parser
void test_json_parser() {
    JsonValue v;

    CHECK(parse_ok("null", v));
    CHECK(v.type == JsonValue::T::Null);
    CHECK(parse_ok("true", v) && v.b == true);
    CHECK(parse_ok("false", v) && v.b == false);
    CHECK(parse_ok("42", v) && v.num == 42.0);
    CHECK(parse_ok("-1.5e3", v) && v.num == -1500.0);
    CHECK(parse_ok("0.25", v) && v.num == 0.25);
    CHECK(parse_ok("0", v) && v.num == 0.0);
    CHECK(parse_ok("\"abc\"", v) && v.str == "abc");
    CHECK(parse_ok("\"a\\\"b\\\\c\\/d\\n\\t\\u0041\"", v) &&
          v.str == "a\"b\\c/d\n\tA");
    // Surrogate pair -> U+1F600, encoded as 4-byte UTF-8.
    CHECK(parse_ok("\"\\ud83d\\ude00\"", v) &&
          v.str == "\xF0\x9F\x98\x80");

    CHECK(parse_ok("[1, 2.5, \"x\", null, true]", v));
    CHECK(v.type == JsonValue::T::Array && v.arr.size() == 5);
    CHECK(parse_ok("{\"a\": 1, \"b\": {\"c\": []}}", v));
    CHECK(v.type == JsonValue::T::Object && v.find("a") &&
          v.find("a")->num == 1.0 && v.find("b") &&
          v.find("b")->find("c") &&
          v.find("b")->find("c")->type == JsonValue::T::Array);
    CHECK(parse_ok("  { \"a\" : [ ] }  ", v));  // whitespace tolerance

    // Malformed: every one of these must FAIL the strict parse.
    CHECK(parse_bad("", v));
    CHECK(parse_bad("{", v));
    CHECK(parse_bad("{\"a\":}", v));
    CHECK(parse_bad("{\"a\":1,}", v));  // trailing comma
    CHECK(parse_bad("{a: 1}", v));      // unquoted key
    CHECK(parse_bad("{'a': 1}", v));
    CHECK(parse_bad("[1, 2", v));
    CHECK(parse_bad("1 2", v));         // trailing junk
    CHECK(parse_bad("\"unterminated", v));
    CHECK(parse_bad("\"bad \\q escape\"", v));
    CHECK(parse_bad("01", v));          // leading zero
    CHECK(parse_bad("1.", v));          // bare fraction marker
    CHECK(parse_bad(".5", v));          // missing leading digit
    CHECK(parse_bad("1e", v));
    CHECK(parse_bad("-e5", v));
    CHECK(parse_bad("1e999", v));       // overflows to inf
    CHECK(parse_bad("{\"a\": \\u0041}", v));  // unquoted string
    CHECK(parse_bad("tru", v));
    {
        std::string e;
        CHECK(!parse_json("nul", v, e) && !e.empty());  // failure = message
    }
}

// ------------------------------------------------- round-trip + tolerance
void test_roundtrip() {
    const Config def;
    const std::string text = def.to_json();
    JsonValue v;
    CHECK(parse_ok(text, v));
    Config back;
    CHECK(config_from_json(v, back));
    CHECK(back == def);

    const Config full = full_config();
    const std::string t2 = full.to_json();
    CHECK(parse_ok(t2, v));
    CHECK(config_from_json(v, back));
    back = full_config();
    CHECK(config_from_json(v, back) && back == full);
    // Deterministic writer: two serializations are byte-identical.
    CHECK(full.to_json() == t2);
}

void test_tolerant_mapping() {
    Config c;
    JsonValue v;

    // Empty object: every field at its default.
    CHECK(parse_ok("{}", v) && config_from_json(v, c) &&
          c == Config{});

    // Unknown keys at any depth are dropped on round-trip.
    CHECK(parse_ok(
              "{\"unknown\": 1, \"video\": {\"res_scale\": 2, "
              "\"nope\": {\"deep\": true}}, \"audio\": {\"sfx_volume\": 40, "
              "\"ghost\": \"x\"}, \"bogus_section\": []}",
              v));
    c = Config{};
    CHECK(config_from_json(v, c));
    CHECK(c.video.res_scale == 2);
    CHECK(c.video.scale_filter == 0);  // untouched default
    CHECK(c.audio.sfx_volume == 40);
    // A file rewritten from a document that carries unknown keys drops
    // them (the round-trip guarantee).
    const std::string rewritten = c.to_json();
    CHECK(rewritten.find("unknown") == std::string::npos);
    CHECK(rewritten.find("nope") == std::string::npos);
    CHECK(rewritten.find("ghost") == std::string::npos);
    CHECK(rewritten.find("bogus") == std::string::npos);

    // Out-of-range numbers clamp; sane numbers pass.
    auto num = [&](const std::string& doc) {
        std::string e;
        c = Config{};
        return parse_json(doc, v, e);
    };
    CHECK(num("{\"video\": {\"res_scale\": 99}}") &&
          config_from_json(v, c) && c.video.res_scale == kResScaleMax);
    CHECK(num("{\"video\": {\"res_scale\": 0}}") &&
          config_from_json(v, c) && c.video.res_scale == kResScaleMin);
    CHECK(num("{\"video\": {\"res_scale\": -5}}") &&
          config_from_json(v, c) && c.video.res_scale == kResScaleMin);
    CHECK(num("{\"video\": {\"res_scale\": 2.9}}") &&
          config_from_json(v, c) && c.video.res_scale == 2);
    CHECK(num("{\"audio\": {\"master_volume\": 150}}") &&
          config_from_json(v, c) &&
          c.audio.master_volume == kVolumeMax);
    CHECK(num("{\"audio\": {\"sfx_volume\": -3}}") &&
          config_from_json(v, c) && c.audio.sfx_volume == kVolumeMin);
    CHECK(num("{\"gameplay\": {\"difficulty\": 7}}") &&
          config_from_json(v, c) &&
          c.gameplay.difficulty == kDifficultyMax);
    CHECK(num("{\"controls\": {\"mouse_sensitivity\": 99}}") &&
          config_from_json(v, c) &&
          c.controls.mouse_sensitivity == kSensMax);
    CHECK(num("{\"controls\": {\"mouse_sensitivity\": 0.01}}") &&
          config_from_json(v, c) &&
          c.controls.mouse_sensitivity == kSensMin);

    // Enum by name (case-insensitive) and by number; junk -> default.
    c = Config{};
    CHECK(parse_ok("{\"video\": {\"scale_filter\": \"Nearest\"}}", v) &&
          config_from_json(v, c) && c.video.scale_filter == 1);
    c = Config{};
    CHECK(parse_ok("{\"video\": {\"fullscreen_mode\": \"EXCLUSIVE\"}}", v) &&
          config_from_json(v, c) && c.video.fullscreen_mode == 2);
    c = Config{};
    CHECK(parse_ok("{\"video\": {\"scale_filter\": \"bilinear\"}}", v) &&
          config_from_json(v, c) && c.video.scale_filter == 0);
    c = Config{};
    CHECK(parse_ok("{\"video\": {\"fullscreen_mode\": 5}}", v) &&
          config_from_json(v, c) && c.video.fullscreen_mode == 2);  // clamp
    c = Config{};
    CHECK(parse_ok("{\"video\": {\"fullscreen_mode\": -1}}", v) &&
          config_from_json(v, c) && c.video.fullscreen_mode == 0);

    // Wrong types fall back to defaults (never an error).
    c = Config{};
    CHECK(parse_ok(
              "{\"video\": 42, \"audio\": {\"music_on\": \"loud\"}, "
              "\"gameplay\": {\"fog\": 7}}",
              v) && config_from_json(v, c) && c == Config{});
    // Hand-edited tolerance: bool as "true"/"on", numbers as strings.
    c = Config{};
    CHECK(parse_ok(
              "{\"audio\": {\"music_on\": \"off\"}, "
              "\"audio2\": null, "
              "\"controls\": {\"mouse_sensitivity\": \"3.5\"}}",
              v) && config_from_json(v, c) && c.audio.music_on == false &&
          c.controls.mouse_sensitivity == 3.5);

    // The version field is stored verbatim (opaque marker) and missing
    // defaults to 1.
    c = Config{};
    CHECK(parse_ok("{\"version\": 2}", v) && config_from_json(v, c) &&
          c.version == 2 && c.to_json().find("\"version\": 2") !=
                                std::string::npos);
    c = Config{};
    CHECK(parse_ok("{}", v) && config_from_json(v, c) &&
          c.version == kConfigVersion);
    // A non-object root is the ONLY failure mode of config_from_json.
    CHECK(parse_ok("[1]", v) && !config_from_json(v, c));
    CHECK(parse_ok("\"hi\"", v) && !config_from_json(v, c));
}

// -------------------------------------------------------------- file I/O
std::filesystem::path temp_dir() {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // read-only env lookup
#endif
    const char* tmp = std::getenv("TMPDIR") ? std::getenv("TMPDIR")
                                            : std::getenv("TEMP");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    std::filesystem::path base =
        (tmp && *tmp) ? std::filesystem::path(tmp)
                      : std::filesystem::temp_directory_path();
    auto d = base / "w3d_config_tests";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}

void test_file_io() {
    const auto dir = temp_dir();
    const auto path = dir / "w3d_cfg_test" / "nested" / "config.json";
    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);

    // Missing file: defaults + a note.
    {
        LoadResult r = load_config(path);
        CHECK(!r.file_valid);
        CHECK(r.cfg == Config{});
        CHECK(!r.note.empty());
    }
    // Save creates parent directories and round-trips.
    {
        const Config c = full_config();
        std::string err;
        CHECK(save_config(path, c, err));
        CHECK(std::filesystem::exists(path));
        CHECK(!std::filesystem::exists(path.string() + ".tmp"));  // cleaned up
        LoadResult r = load_config(path);
        CHECK(r.file_valid);
        CHECK(r.cfg == c);
    }
    // Empty file: corrupt (defaults).
    {
        const auto p = dir / "empty.json";
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.close();
        LoadResult r = load_config(p);
        CHECK(!r.file_valid && r.cfg == Config{});
        std::filesystem::remove(p, ec);
    }
    // Garbage file: corrupt (defaults), no crash.
    {
        const auto p = dir / "garbage.json";
        {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f << "{\"video\": {\"res_scale\": 2";  // truncated
        }
        LoadResult r = load_config(p);
        CHECK(!r.file_valid && r.cfg == Config{} && !r.note.empty());
        std::filesystem::remove(p, ec);
    }
    // Partial file: present fields load, absent ones default.
    {
        const auto p = dir / "partial.json";
        {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f << "{\"video\": {\"res_scale\": 3}}";
        }
        LoadResult r = load_config(p);
        CHECK(r.file_valid);
        CHECK(r.cfg.video.res_scale == 3);
        CHECK(r.cfg.video.scale_filter == 0);
        CHECK(r.cfg.audio.master_volume == 100);
        std::filesystem::remove(p, ec);
    }
    // Save overwrites an existing target (no stale .tmp left behind).
    {
        const auto c2 = Config{};
        std::string err;
        CHECK(save_config(path, c2, err));
        LoadResult r = load_config(path);
        CHECK(r.file_valid && r.cfg == c2);
    }
    std::filesystem::remove_all(path.parent_path(), ec);
}

// ------------------------------------------------------ CLI precedence
void test_cli_precedence() {
    Config c = full_config();  // res 4, nearest, fog off, ...
    // Full override set.
    CliOverrides cli;
    cli.has_res_scale = true;
    cli.res_scale = 2;
    cli.has_scale_filter = true;
    cli.scale_filter = 2;
    cli.has_fog = true;
    cli.fog = true;
    apply_cli_overrides(c, cli);
    CHECK(c.video.res_scale == 2);
    CHECK(c.video.scale_filter == 2);
    CHECK(c.gameplay.fog == true);
    // Everything else is untouched by the CLI.
    CHECK(c.audio.master_volume == 30);
    CHECK(c.controls.mouse_sensitivity == 2.5);

    // Partial overrides: only what was parsed applies.
    c = full_config();
    CliOverrides some;
    some.has_res_scale = true;
    some.res_scale = 3;
    apply_cli_overrides(c, some);
    CHECK(c.video.res_scale == 3);
    CHECK(c.video.scale_filter == 1);  // config value survives
    CHECK(c.gameplay.fog == false);

    // Out-of-range CLI values are clamped, not passed through raw.
    c = Config{};
    CliOverrides bad;
    bad.has_res_scale = true;
    bad.res_scale = 99;
    apply_cli_overrides(c, bad);
    CHECK(c.video.res_scale == kResScaleMax);
}

}  // namespace

// ------------------------------------------------------------ registration
static Register r_json("json_parser", test_json_parser);
static Register r_rt("roundtrip", test_roundtrip);
static Register r_tol("tolerant_mapping", test_tolerant_mapping);
static Register r_io("file_io", test_file_io);
static Register r_cli("cli_precedence", test_cli_precedence);

int main() {
    for (const auto& c : cases()) {
        g_current = c.name;
        std::printf("[%s]\n", c.name);
        c.fn();
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
