#include "core/Config.h"
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// ── WASM: persist settings via browser localStorage ─────────────────────────

EM_JS(char*, js_loadSettings, (), {
    var s = localStorage.getItem("baudline_settings");
    if (!s) return 0;
    var len = lengthBytesUTF8(s) + 1;
    var buf = _malloc(len);
    stringToUTF8(s, buf, len);
    return buf;
});

EM_JS(void, js_saveSettings, (const char* data), {
    localStorage.setItem("baudline_settings", UTF8ToString(data));
});

#else
#include <sys/stat.h>
#endif

namespace baudline {

std::string Config::defaultPath() {
#ifdef __EMSCRIPTEN__
    return "";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && xdg[0]) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = home ? std::string(home) + "/.config" : ".";
    }
    return base + "/baudline/settings.ini";
#endif
}

std::string Config::resolvedPath(const std::string& path) const {
    return path.empty() ? defaultPath() : path;
}

bool Config::load(const std::string& path) {
#ifdef __EMSCRIPTEN__
    char* raw = js_loadSettings();
    if (!raw) return false;
    std::string content(raw);
    free(raw);
    std::istringstream f(content);
#else
    std::ifstream f(resolvedPath(path));
    if (!f.is_open()) return false;
#endif

    data_.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Trim whitespace.
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        data_[key] = val;
    }
    return true;
}

#ifndef __EMSCRIPTEN__
static void ensureDir(const std::string& path) {
    // Create parent directories.
    auto lastSlash = path.rfind('/');
    if (lastSlash == std::string::npos) return;
    std::string dir = path.substr(0, lastSlash);
    // Simple recursive mkdir.
    for (size_t i = 1; i < dir.size(); ++i) {
        if (dir[i] == '/') {
            dir[i] = '\0';
            mkdir(dir.c_str(), 0755);
            dir[i] = '/';
        }
    }
    mkdir(dir.c_str(), 0755);
}
#endif

bool Config::save(const std::string& path) const {
#ifdef __EMSCRIPTEN__
    std::ostringstream f;
    f << "# Baudline settings\n";
    for (const auto& [k, v] : data_)
        f << k << " = " << v << "\n";
    js_saveSettings(f.str().c_str());
    return true;
#else
    std::string p = resolvedPath(path);
    ensureDir(p);
    std::ofstream f(p);
    if (!f.is_open()) return false;

    f << "# Baudline settings\n";
    for (const auto& [k, v] : data_)
        f << k << " = " << v << "\n";
    return true;
#endif
}

void Config::setString(const std::string& key, const std::string& value) { data_[key] = value; }
void Config::setInt(const std::string& key, int value) { data_[key] = std::to_string(value); }
void Config::setFloat(const std::string& key, float value) {
    std::ostringstream ss;
    ss << value;
    data_[key] = ss.str();
}
void Config::setBool(const std::string& key, bool value) { data_[key] = value ? "1" : "0"; }

std::string Config::getString(const std::string& key, const std::string& def) const {
    auto it = data_.find(key);
    return it != data_.end() ? it->second : def;
}

int Config::getInt(const std::string& key, int def) const {
    auto it = data_.find(key);
    if (it == data_.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

float Config::getFloat(const std::string& key, float def) const {
    auto it = data_.find(key);
    if (it == data_.end()) return def;
    try { return std::stof(it->second); } catch (...) { return def; }
}

bool Config::getBool(const std::string& key, bool def) const {
    auto it = data_.find(key);
    if (it == data_.end()) return def;
    return it->second == "1" || it->second == "true";
}

} // namespace baudline
