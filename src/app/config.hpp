// w3d app/config.hpp — persistent settings (Phase 5, WS4).
//
// Pure C++ (no SDL, no game data, no fixed file location): a minimal
// hand-rolled JSON writer + tolerant parser (no new dependency, per plan
// section 4.4), the v1 schema, and load/save against a CALLER-SUPPLIED
// path. The app owns the file location (%APPDATA%\Wolf3DRemake\config.json,
// default_config_path) and the save-on-change / save-on-clean-exit policy;
// this module never touches %APPDATA% on its own, so the whole schema is
// testable headless against temp paths.
//
// Tolerance model (documented in the phase plan):
//   - missing file / empty file / malformed JSON  -> defaults + one note
//   - missing key or wrong-typed value            -> that field's default
//   - out-of-range number                         -> clamped
//   - unknown key (any depth)                     -> dropped on round-trip
//
// Schema v1 (key order is fixed; the writer is deterministic):
//   version: 1
//   video:    res_scale (1-4), scale_filter (linear|nearest|integer),
//             fullscreen_mode (windowed|borderless|exclusive),
//             show_fps (bool; the per-second console fps line)
//   audio:    master_volume/music_volume/sfx_volume (0-100), music_on,
//             device_id ("" = system default)
//             -- STORED ONLY until the WS5 audio device lands --
//   controls: mouse_sensitivity (0.1-10.0, live: LiveInput multiplier;
//             the bindings editor is WS9, so no bindings field yet)
//   gameplay: difficulty (0 recruit / 1 regular / 2 hardcore; applies in
//             WS7), fog (bool; live, reuses the --no-fog plumbing)
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace w3d::app {

inline constexpr int kConfigVersion = 1;
inline constexpr int kResScaleMin = 1, kResScaleMax = 4;
inline constexpr int kVolumeMin = 0, kVolumeMax = 100;
inline constexpr double kSensMin = 0.1, kSensMax = 10.0;
inline constexpr int kDifficultyMin = 0, kDifficultyMax = 2;

// Value-name tables (shared by the JSON mapping and the options screen).
// Index order = stored integer order.
namespace cfgname {
inline constexpr int kNameCount = 3;
const char* scale_filter(int i);   // 0 linear, 1 nearest, 2 integer
const char* fullscreen_mode(int i);  // 0 windowed, 1 borderless, 2 exclusive
const char* difficulty(int i);      // 0 recruit, 1 regular, 2 hardcore
// Case-insensitive; -1 if unknown (the caller keeps the default).
int scale_filter(const std::string& s);
int fullscreen_mode(const std::string& s);
}  // namespace cfgname

struct VideoSettings {
    int res_scale = 1;          // 1..kResScaleMax
    int scale_filter = 0;       // cfgname::scale_filter indexes
    int fullscreen_mode = 0;    // cfgname::fullscreen_mode indexes
    bool show_fps = true;       // per-second console fps line (WS4: live)
};

// STORED ONLY in WS4 (device + mixer wiring lands in WS5).
struct AudioSettings {
    int master_volume = 100;    // 0..100
    int music_volume = 100;
    int sfx_volume = 100;
    bool music_on = true;
    std::string device_id;      // "" = system default (WS5 populates)
};

// WS4: sensitivity only (bindings come with the WS9 rebind editor).
struct ControlsSettings {
    double mouse_sensitivity = 1.0;  // 0.1..10.0 multiplier on relative x
};

// WS4: fog live; difficulty stored-only (applies in WS7).
struct GameplaySettings {
    int difficulty = 1;  // regular
    bool fog = true;
};

struct Config {
    int version = kConfigVersion;
    VideoSettings video;
    AudioSettings audio;
    ControlsSettings controls;
    GameplaySettings gameplay;

    [[nodiscard]] bool operator==(const Config&) const;
    // Deterministic (fixed key order) pretty-printed JSON.
    [[nodiscard]] std::string to_json() const;
};

// ---- Minimal JSON tree (the parser's intermediate form) ----
struct JsonValue {
    enum class T { Null, Bool, Number, String, Array, Object } type = T::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;  // insertion order
    [[nodiscard]] const JsonValue* find(const char* key) const;
};

// Strict-JSON parse (malformed input -> false + err; the config layer then
// falls back to defaults — "corrupt -> defaults without crash").
bool parse_json(const std::string& text, JsonValue& out, std::string& err);
// Pretty-print (2-space indent).
std::string json_write(const JsonValue& v, int indent = 0);

// Tolerant mapping (see the file header): false only when the document
// root is not an object.
bool config_from_json(const JsonValue& root, Config& out);
// Exact inverse of the schema (unknown keys never appear — that is what
// "dropped on round-trip" means).
JsonValue config_to_json(const Config& c);

// ---- File I/O (caller-supplied path) ----
struct LoadResult {
    Config cfg;            // defaults when file_valid == false
    bool file_valid = false;
    std::string note;      // one log line when the file was missing/corrupt
};
LoadResult load_config(const std::filesystem::path& path);
// Creates parent directories; writes <path>.tmp then renames over the
// target (no torn files on crash).
bool save_config(const std::filesystem::path& path, const Config& cfg,
                 std::string& err);
// %APPDATA%\Wolf3DRemake\config.json (fallback: ./Wolf3DRemake/config.json
// when APPDATA is unset).
[[nodiscard]] std::filesystem::path default_config_path();

// ---- CLI precedence (the app applies this once at startup) ----
// Flags given on the command line override the config for the CURRENT
// RUN and never write the file (the scripted harness must stay
// deterministic). Only flags that were actually parsed are applied.
struct CliOverrides {
    bool has_res_scale = false;
    int res_scale = 1;
    bool has_scale_filter = false;
    int scale_filter = 0;
    bool has_fog = false;
    bool fog = true;
};
void apply_cli_overrides(Config& cfg, const CliOverrides& cli);

}  // namespace w3d::app
