// The Recycle Bin on Linux (convert.h): the freedesktop.org trash in $XDG_DATA_HOME/Trash, the same
// place file managers use. A file moved there gets a .trashinfo note with its original path and
// deletion time, so it can be brought back. The trash must be on the file's own drive, since a
// move across drives would be a copy; otherwise the file is kept where it is, as the Windows build
// does when its bin isn't reachable.
#include "convert.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

std::string TrashDir() {
    if (const char* data = getenv("XDG_DATA_HOME"); data && *data) return std::string(data) + "/Trash";
    const char* home = getenv("HOME");
    return std::string(home ? home : "") + "/.local/share/Trash";
}

// The trashinfo Path is the original absolute path, percent-encoded except for '/'.
std::string Encode(const std::string& path) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string Decode(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit((unsigned char)text[i + 1]) &&
            std::isxdigit((unsigned char)text[i + 2])) {
            out += (char)std::stoi(text.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::string LocalNow() {
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    char text[32];
    strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%S", &local);
    return text;
}

std::string Utf8(const std::wstring& text) { return compat::Narrow(text.c_str()); }

}  // namespace

bool RecycleFile(const std::wstring& path) {
    const std::string from = Utf8(path);
    struct stat source{};
    if (stat(from.c_str(), &source) != 0 || !S_ISREG(source.st_mode)) return false;
    const std::string dir = TrashDir();
    const fs::path files = dir + "/files", info = dir + "/info";
    std::error_code ec;
    fs::create_directories(files, ec);
    fs::create_directories(info, ec);
    struct stat bin{};
    if (ec || stat(files.c_str(), &bin) != 0 || bin.st_dev != source.st_dev) return false;

    const std::string base = fs::path(from).filename().string();
    for (int n = 1; n < 1000; ++n) {
        const std::string name = n == 1 ? base : base + "." + std::to_string(n);
        const fs::path note = info / (name + ".trashinfo");
        // The note is created exclusively, so two files of one name never share a trash entry.
        const int fd = open(note.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return false;
        }
        if (fs::exists(files / name)) {
            close(fd);
            unlink(note.c_str());
            continue;
        }
        const std::string text = "[Trash Info]\nPath=" + Encode(from) + "\nDeletionDate=" + LocalNow() + "\n";
        const bool wrote = write(fd, text.data(), text.size()) == (ssize_t)text.size();
        close(fd);
        if (!wrote || rename(from.c_str(), (files / name).c_str()) != 0) {
            unlink(note.c_str());
            return false;
        }
        return true;
    }
    return false;
}

bool RestoreFromRecycleBin(const std::wstring& wideOriginal) {
    const std::string original = Utf8(wideOriginal);
    const std::string dir = TrashDir();
    const fs::path files = dir + "/files", info = dir + "/info";
    // The most recent deletion of that path, as the Windows bin picks when a path was recycled twice.
    std::string bestName, bestDate;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(info, ec)) {
        if (entry.path().extension() != ".trashinfo") continue;
        std::ifstream in(entry.path());
        std::string line, path, date;
        while (std::getline(in, line)) {
            if (line.rfind("Path=", 0) == 0) path = Decode(line.substr(5));
            else if (line.rfind("DeletionDate=", 0) == 0) date = line.substr(13);
        }
        if (path != original) continue;
        if (bestName.empty() || date > bestDate) {
            bestName = entry.path().stem().string();
            bestDate = date;
        }
    }
    if (bestName.empty() || fs::exists(original, ec)) return false;
    const fs::path parent = fs::path(original).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    if (rename((files / bestName).c_str(), original.c_str()) != 0) return false;
    fs::remove(info / (bestName + ".trashinfo"), ec);
    return true;
}
