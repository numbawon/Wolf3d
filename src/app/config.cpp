// w3d app/config.cpp — JSON writer/parser + v1 schema (Phase 5, WS4).
#include "app/config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace w3d::app {

namespace {

// ---------------------------------------------------------------- name maps
const char* kScaleFilterNames[cfgname::kNameCount] = {"linear", "nearest",
                                                      "integer"};
const char* kFullscreenNames[cfgname::kNameCount] = {"windowed", "borderless",
                                                     "exclusive"};
const char* kDifficultyNames[cfgname::kNameCount] = {"recruit", "regular",
                                                     "hardcore"};

std::string lower_copy(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

int match_name(const char* names[cfgname::kNameCount], const std::string& s) {
    const std::string l = lower_copy(s);
    for (int i = 0; i < cfgname::kNameCount; ++i)
        if (l == names[i])
            return i;
    return -1;
}

// ------------------------------------------------------------ JSON parsing
// Strict JSON (RFC 8259 shape): the TOLERANCE lives in config_from_json,
// not here — a malformed document fails the parse and the caller falls
// back to defaults.
struct JsonParser {
    const std::string& s;
    std::size_t i = 0;
    std::string& err;

    [[nodiscard]] bool eof() const { return i >= s.size(); }
    [[nodiscard]] char peek() const { return s[i]; }
    void skip_ws() {
        while (!eof() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                          s[i] == '\r'))
            ++i;
    }
    [[nodiscard]] std::string where() const {
        return "at offset " + std::to_string(i);
    }

    bool parse_value(JsonValue& v) {
        v = JsonValue();  // the caller may reuse a tree: never append
        skip_ws();
        if (eof())
            return fail("unexpected end of input");
        const char c = peek();
        if (c == '{')
            return parse_object(v);
        if (c == '[')
            return parse_array(v);
        if (c == '"')
            return parse_string_value(v);
        if (c == 't' || c == 'f' || c == 'n')
            return parse_literal(v);
        if (c == '-' || (c >= '0' && c <= '9'))
            return parse_number(v);
        return fail(std::string("unexpected character '") + c + "' " + where());
    }

    bool fail(const std::string& msg) {
        err = msg + " " + where();
        return false;
    }

    bool parse_literal(JsonValue& v) {
        struct {
            const char* lit;
            JsonValue::T t;
            bool b;
        } kws[] = {{"true", JsonValue::T::Bool, true},
                   {"false", JsonValue::T::Bool, false},
                   {"null", JsonValue::T::Null, false}};
        for (const auto& k : kws) {
            const std::size_t n = std::strlen(k.lit);
            if (s.compare(i, n, k.lit) == 0) {
                i += n;
                v.type = k.t;
                v.b = k.b;
                return true;
            }
        }
        return fail("invalid literal");
    }

    bool parse_object(JsonValue& v) {
        v.type = JsonValue::T::Object;
        ++i;  // '{'
        skip_ws();
        if (eof())
            return fail("unterminated object");
        if (peek() == '}') {
            ++i;
            return true;
        }
        for (;;) {
            skip_ws();
            if (eof() || peek() != '"')
                return fail("expected object key string");
            std::string key;
            if (!parse_string_raw(key))
                return false;
            skip_ws();
            if (eof() || peek() != ':')
                return fail("expected ':' after object key");
            ++i;
            JsonValue val;
            if (!parse_value(val))
                return false;
            v.obj.emplace_back(std::move(key), std::move(val));
            skip_ws();
            if (eof())
                return fail("unterminated object");
            if (peek() == ',') {
                ++i;
                continue;
            }
            if (peek() == '}') {
                ++i;
                return true;
            }
            return fail("expected ',' or '}' in object");
        }
    }

    bool parse_array(JsonValue& v) {
        v.type = JsonValue::T::Array;
        ++i;  // '['
        skip_ws();
        if (eof())
            return fail("unterminated array");
        if (peek() == ']') {
            ++i;
            return true;
        }
        for (;;) {
            JsonValue val;
            if (!parse_value(val))
                return false;
            v.arr.push_back(std::move(val));
            skip_ws();
            if (eof())
                return fail("unterminated array");
            if (peek() == ',') {
                ++i;
                continue;
            }
            if (peek() == ']') {
                ++i;
                return true;
            }
            return fail("expected ',' or ']' in array");
        }
    }

    bool parse_string_value(JsonValue& v) {
        if (!parse_string_raw(v.str))
            return false;
        v.type = JsonValue::T::String;
        return true;
    }

    bool parse_string_raw(std::string& out) {
        ++i;  // opening quote
        for (;;) {
            if (eof())
                return fail("unterminated string");
            const char c = s[i++];
            if (c == '"')
                return true;
            if (c == '\\') {
                if (eof())
                    return fail("unterminated escape");
                const char e = s[i++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!parse_hex4(cp))
                            return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // Possible surrogate pair: \uDC00-\uDFFF follows.
                            if (i + 1 < s.size() && s[i] == '\\' &&
                                s[i + 1] == 'u') {
                                const std::size_t save = i;
                                i += 2;
                                unsigned lo = 0;
                                if (parse_hex4(lo) && lo >= 0xDC00 &&
                                    lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) +
                                         (lo - 0xDC00);
                                } else {
                                    i = save;  // lone high surrogate
                                }
                            }
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default:
                        return fail(std::string("invalid escape '\\") + e +
                                    "'");
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return fail("raw control character in string");
            } else {
                out.push_back(c);
            }
        }
    }

    bool parse_hex4(unsigned& out) {
        if (i + 4 > s.size())
            return fail("truncated \\u escape");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<unsigned>(c - 'A' + 10);
            else
                return fail("bad hex digit in \\u escape");
        }
        out = v;
        return true;
    }

    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            // Unpaired surrogates are passed through as CESU-8 (never
            // crashes; the config mapping only cares about ASCII).
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_number(JsonValue& v) {
        const std::size_t start = i;
        if (peek() == '-')
            ++i;
        if (eof())
            return fail("incomplete number");
        if (peek() == '0') {
            ++i;  // "0" or "0." or "0e1" — never "01"
        } else if (peek() >= '1' && peek() <= '9') {
            while (!eof() && peek() >= '0' && peek() <= '9')
                ++i;
        } else {
            return fail("invalid number (leading zero or missing digit)");
        }
        if (!eof() && peek() == '.') {
            ++i;
            if (eof() || peek() < '0' || peek() > '9')
                return fail("invalid number fraction");
            while (!eof() && peek() >= '0' && peek() <= '9')
                ++i;
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            ++i;
            if (!eof() && (peek() == '+' || peek() == '-'))
                ++i;
            if (eof() || peek() < '0' || peek() > '9')
                return fail("invalid number exponent");
            while (!eof() && peek() >= '0' && peek() <= '9')
                ++i;
        }
        const std::string tok = s.substr(start, i - start);
        char* end = nullptr;
        const double d = std::strtod(tok.c_str(), &end);
        if (end != tok.c_str() + tok.size())
            return fail("malformed number");
        if (!std::isfinite(d))
            return fail("number out of range");
        v.type = JsonValue::T::Number;
        v.num = d;
        return true;
    }
};

// ------------------------------------------------------- tolerant mapping
// Tolerant helpers: wrong type -> default (never an error).
int as_int_clamped(const JsonValue* v, int def, int lo, int hi) {
    if (!v)
        return def;
    if (v->type == JsonValue::T::Number) {
        const int x = static_cast<int>(v->num);
        return std::clamp(x, lo, hi);
    }
    if (v->type == JsonValue::T::String && !v->str.empty()) {
        char* end = nullptr;
        const long x = std::strtol(v->str.c_str(), &end, 10);
        if (end && *end == '\0')
            return std::clamp(static_cast<int>(x), lo, hi);
    }
    return def;
}

bool as_bool(const JsonValue* v, bool def) {
    if (!v)
        return def;
    if (v->type == JsonValue::T::Bool)
        return v->b;
    if (v->type == JsonValue::T::String) {
        const std::string l = lower_copy(v->str);
        if (l == "true" || l == "on" || l == "yes")
            return true;
        if (l == "false" || l == "off" || l == "no")
            return false;
    }
    return def;
}

int as_enum(const JsonValue* v, int def, int (*match)(const std::string&)) {
    if (!v)
        return def;
    if (v->type == JsonValue::T::Number)
        return std::clamp(static_cast<int>(v->num), 0, cfgname::kNameCount - 1);
    if (v->type == JsonValue::T::String) {
        const int m = match(v->str);
        if (m >= 0)
            return m;
    }
    return def;
}

double as_double_clamped(const JsonValue* v, double def, double lo,
                         double hi) {
    if (!v)
        return def;
    if (v->type == JsonValue::T::Number) {
        if (std::isfinite(v->num))
            return std::clamp(v->num, lo, hi);
        return def;
    }
    if (v->type == JsonValue::T::String && !v->str.empty()) {
        char* end = nullptr;
        const double x = std::strtod(v->str.c_str(), &end);
        if (end && *end == '\0' && std::isfinite(x))
            return std::clamp(x, lo, hi);
    }
    return def;
}

}  // namespace

// ------------------------------------------------------------------ public
const char* cfgname::scale_filter(int i) {
    return kScaleFilterNames[std::clamp(i, 0, kNameCount - 1)];
}
const char* cfgname::fullscreen_mode(int i) {
    return kFullscreenNames[std::clamp(i, 0, kNameCount - 1)];
}
const char* cfgname::difficulty(int i) {
    return kDifficultyNames[std::clamp(i, 0, kNameCount - 1)];
}
int cfgname::scale_filter(const std::string& s) {
    return match_name(kScaleFilterNames, s);
}
int cfgname::fullscreen_mode(const std::string& s) {
    return match_name(kFullscreenNames, s);
}

const JsonValue* JsonValue::find(const char* key) const {
    if (type != T::Object)
        return nullptr;
    for (const auto& kv : obj)
        if (kv.first == key)
            return &kv.second;
    return nullptr;
}

bool Config::operator==(const Config& o) const {
    return version == o.version && video.res_scale == o.video.res_scale &&
           video.scale_filter == o.video.scale_filter &&
           video.fullscreen_mode == o.video.fullscreen_mode &&
           video.show_fps == o.video.show_fps &&
           audio.master_volume == o.audio.master_volume &&
           audio.music_volume == o.audio.music_volume &&
           audio.sfx_volume == o.audio.sfx_volume &&
           audio.music_on == o.audio.music_on &&
           audio.device_id == o.audio.device_id &&
           controls.mouse_sensitivity == o.controls.mouse_sensitivity &&
           gameplay.difficulty == o.gameplay.difficulty &&
           gameplay.fog == o.gameplay.fog;
}

bool parse_json(const std::string& text, JsonValue& out, std::string& err) {
    JsonParser p{text, 0, err};
    if (!p.parse_value(out))
        return false;
    p.skip_ws();
    if (!p.eof())
        return p.fail("trailing characters");
    return true;
}

std::string json_write(const JsonValue& v, int indent) {
    std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
    std::string pad2(static_cast<std::size_t>(indent + 1) * 2, ' ');
    switch (v.type) {
        case JsonValue::T::Null:
            return "null";
        case JsonValue::T::Bool:
            return v.b ? "true" : "false";
        case JsonValue::T::Number: {
            if (std::isfinite(v.num) && v.num == std::floor(v.num) &&
                std::fabs(v.num) < 9.0e15)
                return std::to_string(static_cast<long long>(v.num));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", v.num);
            return buf;
        }
        case JsonValue::T::String: {
            std::string out = "\"";
            for (const char raw : v.str) {
                const unsigned char c = static_cast<unsigned char>(raw);
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b"; break;
                    case '\f': out += "\\f"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (c < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out += buf;
                        } else {
                            out.push_back(raw);
                        }
                }
            }
            out.push_back('"');
            return out;
        }
        case JsonValue::T::Array: {
            if (v.arr.empty())
                return "[]";
            std::string out = "[\n";
            for (std::size_t k = 0; k < v.arr.size(); ++k) {
                out += pad2;
                out += json_write(v.arr[k], indent + 1);
                if (k + 1 < v.arr.size())
                    out.push_back(',');
                out.push_back('\n');
            }
            out += pad;
            out += "]";
            return out;
        }
        case JsonValue::T::Object: {
            if (v.obj.empty())
                return "{}";
            std::string out = "{\n";
            for (std::size_t k = 0; k < v.obj.size(); ++k) {
                out += pad2;
                out += json_write(JsonValue{JsonValue::T::String, false, 0,
                                            v.obj[k].first},
                                  indent + 1);
                out += ": ";
                out += json_write(v.obj[k].second, indent + 1);
                if (k + 1 < v.obj.size())
                    out.push_back(',');
                out.push_back('\n');
            }
            out += pad;
            out += "}";
            return out;
        }
    }
    return "null";
}

bool config_from_json(const JsonValue& root, Config& c) {
    if (root.type != JsonValue::T::Object)
        return false;
    c = Config{};  // start from defaults; every field below is optional
    if (const JsonValue* v = root.find("version"))
        c.version = as_int_clamped(v, kConfigVersion, 0, 1000000);

    if (const JsonValue* v = root.find("video")) {
        if (v->type == JsonValue::T::Object) {
            c.video.res_scale =
                as_int_clamped(v->find("res_scale"), 1, kResScaleMin,
                               kResScaleMax);
            c.video.scale_filter =
                as_enum(v->find("scale_filter"), 0,
                        [](const std::string& s) {
                            return cfgname::scale_filter(s);
                        });
            c.video.fullscreen_mode =
                as_enum(v->find("fullscreen_mode"), 0,
                        [](const std::string& s) {
                            return cfgname::fullscreen_mode(s);
                        });
            c.video.show_fps =
                as_bool(v->find("show_fps"), VideoSettings{}.show_fps);
        }
    }

    if (const JsonValue* v = root.find("audio")) {
        if (v->type == JsonValue::T::Object) {
            c.audio.master_volume = as_int_clamped(v->find("master_volume"),
                                                   100, kVolumeMin, kVolumeMax);
            c.audio.music_volume = as_int_clamped(v->find("music_volume"),
                                                  100, kVolumeMin, kVolumeMax);
            c.audio.sfx_volume = as_int_clamped(v->find("sfx_volume"), 100,
                                                kVolumeMin, kVolumeMax);
            c.audio.music_on =
                as_bool(v->find("music_on"), AudioSettings{}.music_on);
            if (const JsonValue* d = v->find("device_id"))
                if (d->type == JsonValue::T::String)
                    c.audio.device_id = d->str;
        }
    }

    if (const JsonValue* v = root.find("controls")) {
        if (v->type == JsonValue::T::Object) {
            c.controls.mouse_sensitivity =
                as_double_clamped(v->find("mouse_sensitivity"), 1.0, kSensMin,
                                  kSensMax);
        }
    }

    if (const JsonValue* v = root.find("gameplay")) {
        if (v->type == JsonValue::T::Object) {
            c.gameplay.difficulty = as_int_clamped(
                v->find("difficulty"), 1, kDifficultyMin, kDifficultyMax);
            c.gameplay.fog =
                as_bool(v->find("fog"), GameplaySettings{}.fog);
        }
    }
    return true;
}

JsonValue config_to_json(const Config& c) {
    auto keyval = [](const char* k, JsonValue v) {
        return std::pair<std::string, JsonValue>(k, std::move(v));
    };
    auto num = [](double x) {
        JsonValue v;
        v.type = JsonValue::T::Number;
        v.num = x;
        return v;
    };
    auto bval = [](bool x) {
        JsonValue v;
        v.type = JsonValue::T::Bool;
        v.b = x;
        return v;
    };
    auto sval = [](const std::string& x) {
        JsonValue v;
        v.type = JsonValue::T::String;
        v.str = x;
        return v;
    };

    JsonValue video;
    video.type = JsonValue::T::Object;
    video.obj.push_back(keyval("res_scale", num(c.video.res_scale)));
    video.obj.push_back(keyval("scale_filter",
                               sval(cfgname::scale_filter(c.video.scale_filter))));
    video.obj.push_back(
        keyval("fullscreen_mode",
               sval(cfgname::fullscreen_mode(c.video.fullscreen_mode))));
    video.obj.push_back(keyval("show_fps", bval(c.video.show_fps)));

    JsonValue audio;
    audio.type = JsonValue::T::Object;
    audio.obj.push_back(keyval("master_volume", num(c.audio.master_volume)));
    audio.obj.push_back(keyval("music_volume", num(c.audio.music_volume)));
    audio.obj.push_back(keyval("sfx_volume", num(c.audio.sfx_volume)));
    audio.obj.push_back(keyval("music_on", bval(c.audio.music_on)));
    audio.obj.push_back(keyval("device_id", sval(c.audio.device_id)));

    JsonValue controls;
    controls.type = JsonValue::T::Object;
    controls.obj.push_back(
        keyval("mouse_sensitivity", num(c.controls.mouse_sensitivity)));

    JsonValue gameplay;
    gameplay.type = JsonValue::T::Object;
    gameplay.obj.push_back(keyval("difficulty", num(c.gameplay.difficulty)));
    gameplay.obj.push_back(keyval("fog", bval(c.gameplay.fog)));

    JsonValue root;
    root.type = JsonValue::T::Object;
    root.obj.push_back(keyval("version", num(c.version)));
    root.obj.push_back(keyval("video", std::move(video)));
    root.obj.push_back(keyval("audio", std::move(audio)));
    root.obj.push_back(keyval("controls", std::move(controls)));
    root.obj.push_back(keyval("gameplay", std::move(gameplay)));
    return root;
}

std::string Config::to_json() const { return json_write(config_to_json(*this)); }

LoadResult load_config(const std::filesystem::path& path) {
    LoadResult r;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        r.note = "config: missing " + path.string() + "; using defaults";
        return r;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) {
        r.note = "config: " + path.string() + " is empty; using defaults";
        return r;
    }
    JsonValue root;
    std::string err;
    if (!parse_json(text, root, err)) {
        r.note = "config: " + path.string() + " is corrupt (" + err +
                 "); using defaults";
        return r;
    }
    if (!config_from_json(root, r.cfg)) {
        r.note = "config: " + path.string() +
                 " root is not an object; using defaults";
        return r;
    }
    r.file_valid = true;
    return r;
}

bool save_config(const std::filesystem::path& path, const Config& cfg,
                 std::string& err) {
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot open " + tmp + " for writing";
            return false;
        }
        const std::string text = cfg.to_json();
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) {
            err = "write to " + tmp + " failed";
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::remove(path, ec);  // rename over an existing target
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        err = "rename " + tmp + " -> " + path.string() + " failed";
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::filesystem::path default_config_path() {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // read-only env lookup; no portable alt
#endif
    const char* appdata = std::getenv("APPDATA");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (appdata && *appdata)
        return std::filesystem::path(appdata) / "Wolf3DRemake" / "config.json";
    return std::filesystem::path("Wolf3DRemake") / "config.json";
}

void apply_cli_overrides(Config& cfg, const CliOverrides& cli) {
    if (cli.has_res_scale)
        cfg.video.res_scale =
            std::clamp(cli.res_scale, kResScaleMin, kResScaleMax);
    if (cli.has_scale_filter)
        cfg.video.scale_filter =
            std::clamp(cli.scale_filter, 0, cfgname::kNameCount - 1);
    if (cli.has_fog)
        cfg.gameplay.fog = cli.fog;
}

}  // namespace w3d::app
