#pragma once

#include <string>
#include <unordered_map>

namespace baudmine {

// Simple INI-style config: key = value, one per line. Lines starting with # are
// comments.  No sections.  Stored at ~/.config/baudmine/settings.ini.
class Config {
public:
    static std::string defaultPath();

    bool load(const std::string& path = "");
    bool save(const std::string& path = "") const;

    void setString(const std::string& key, const std::string& value);
    void setInt(const std::string& key, int value);
    void setFloat(const std::string& key, float value);
    void setBool(const std::string& key, bool value);

    std::string getString(const std::string& key, const std::string& def = "") const;
    int         getInt(const std::string& key, int def = 0) const;
    float       getFloat(const std::string& key, float def = 0.0f) const;
    bool        getBool(const std::string& key, bool def = false) const;

private:
    std::unordered_map<std::string, std::string> data_;
    std::string resolvedPath(const std::string& path) const;
};

} // namespace baudmine
