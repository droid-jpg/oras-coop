// Simple key=value config stored next to the plugin as oras-coop.txt.
#include "Session.hpp"
#include <cstdlib>

using namespace CTRPluginFramework;

namespace coop {

static const char* CONFIG_PATH = "oras-coop.txt";

void Config::Load() {
    File f(CONFIG_PATH, File::READ);
    if (!f.IsOpen()) return;
    u32 size = f.GetSize();
    if (size == 0 || size > 4096) return;
    std::string text(size, '\0');
    if (f.Read(&text[0], size) != 0) return;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "server") server = v;
        else if (k == "port") port = (u16)std::atoi(v.c_str());
        else if (k == "room") room = v;
        else if (k == "name") name = v;
        else if (k == "greeting") greeting = v;
        else if (k == "scale") pxPerUnit = (float)std::atof(v.c_str());
        else if (k == "overlay") overlay = v != "0";
        else if (k == "puppets") puppets = v == "1";
    }
    if (port == 0) port = 7777;
    if (pxPerUnit <= 0.05f || pxPerUnit > 20.0f) pxPerUnit = 1.4f;
}

void Config::Save() const {
    File f(CONFIG_PATH, File::RWC | File::TRUNCATE);
    if (!f.IsOpen()) return;
    char scale[16];
    snprintf(scale, sizeof scale, "%.2f", pxPerUnit);
    std::string text = "server=" + server + "\nport=" + std::to_string(port) +
                       "\nroom=" + room + "\nname=" + name + "\ngreeting=" + greeting +
                       "\nscale=" + scale + "\noverlay=" + (overlay ? "1" : "0") +
                       "\npuppets=" + (puppets ? "1" : "0") + "\n";
    f.Write(text.data(), text.size());
}

} // namespace coop
