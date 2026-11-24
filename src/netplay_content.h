#pragma once
// Compatibility checksum, not an authentication or trust signature. Compare the
// effective read-only DATA tree in the same overlay order as the IOP. Soundtracks
// and movies are local presentation; gameplay archives must match byte for byte.
#include "../runtime/core/host_file.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <memory>
#include <string>
#include <stdexcept>
namespace netplay {
enum class ContentSource : uint8_t { Disc = 1, Integrated, Custom };
struct ContentProfile {
    ContentSource source = ContentSource::Disc;
    std::vector<std::string> packages;
    bool more_packages = false;
};
struct ContentFingerprint { std::string value, error; ContentProfile profile; };

inline bool checksum_text(const std::string &s, size_t length) {
    return s.size() == length && s.find_first_not_of("0123456789abcdef") == s.npos;
}

inline bool package_label(const std::string &s) {
    const auto at = s.find('@');
    return at > 0 && at < 65 && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9')) &&
           at + 1 < s.size() && s.size() <= 96 && s.find('@', at + 1) == s.npos &&
           s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-") >= at &&
           s.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.+-", at + 1) == s.npos &&
           s[at + 1] >= '0' && s[at + 1] <= '9' && s.find("..") == s.npos;
}

// Only report package IDs and versions recorded by the verified local package composer.
// Project names, paths, and arbitrary manifest text never cross the wire.
inline ContentProfile describe_overlay(const std::filesystem::path &root) {
    namespace fs = std::filesystem;
    ContentProfile profile{ContentSource::Custom};
    std::error_code ec;
    const auto file = root / "PROFILE.TXT";
    const auto status = fs::symlink_status(file, ec);
    if (!fs::exists(status)) {
        if (fs::is_regular_file(fs::symlink_status(root / "INTEGRATED.MANIFEST", ec)) &&
            fs::is_regular_file(root / "DATA/CAR/PHANTOM.CAR", ec) &&
            fs::is_regular_file(root / "DATA/RES/COMMON.RES", ec)) profile.source = ContentSource::Integrated;
        return profile;
    }
    if (!fs::is_regular_file(status) || fs::file_size(file, ec) > 65536 || ec) return profile;
    std::ifstream input(file);
    std::string line;
    if (!std::getline(input, line) || line.rfind("profile ", 0) != 0 ||
        !std::getline(input, line)) return profile;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("game ", 0) != 0 || !checksum_text(line.substr(5), 64)) return profile;
    std::map<std::string, bool> seen;
    while (std::getline(input, line)) {
        if (line.rfind("package ", 0) != 0) continue;
        std::istringstream in(line.substr(8));
        std::string id, version, marker, hash, extra;
        if (!(in >> id >> version >> marker >> hash) || in >> extra || marker != "manifest" ||
            !checksum_text(hash, 64) || !package_label(id + "@" + version) || !seen.emplace(id, true).second)
            return {ContentSource::Custom};
        if (profile.packages.size() < 3) profile.packages.push_back(id + "@" + version);
        else profile.more_packages = true;
    }
    return profile;
}

inline const char *profile_name(ContentSource profile) {
    switch (profile) {
    case ContentSource::Disc: return "disc content without a gameplay pack";
    case ContentSource::Integrated: return "the integrated content pack";
    default: return "custom gameplay content";
    }
}

inline ContentFingerprint content_fingerprint(const std::string &disc, const std::string &overlay) {
    namespace fs = std::filesystem;
    try {
        std::map<std::string,fs::path> files;
        std::map<std::string,bool> types;
        auto upper = [](std::string s) { for (char &c:s) if(c>='a'&&c<='z') c-=32; return s; };
        bool gameplay_overlay = false;
        auto inventory = [&](const std::string &root, bool required, const char *tree, const std::string &prefix) {
            if(root.empty()) return;
            fs::path data;
            for(const auto &e:fs::directory_iterator(fs::u8path(root)))
                if(upper(e.path().filename().u8string())==upper(tree)) {
                    if(!data.empty()) throw std::runtime_error("Ambiguous DATA folders.");
                    if(e.is_symlink() || !e.is_directory()) throw std::runtime_error("DATA must be a real folder.");
                    data=e.path();
                }
            if(data.empty()) { if(required) throw std::runtime_error("Game DATA folder is missing."); return; }
            std::map<std::string,fs::path> layer;
            std::map<std::string,bool> layer_types;
            for(const auto &e:fs::recursive_directory_iterator(data)) {
                if(e.is_symlink()) throw std::runtime_error("Linked game content cannot be checked.");
                const std::string rel=prefix+upper(e.path().lexically_relative(data).generic_u8string());
                const bool directory=e.is_directory();
                if(!directory && !e.is_regular_file()) throw std::runtime_error("Unsupported game content entry.");
                if(!layer_types.emplace(rel,directory).second) throw std::runtime_error("Ambiguous game content names.");
                auto previous=types.find(rel);
                if(previous!=types.end() && previous->second!=directory) throw std::runtime_error("Game overlay file/folder collision.");
                if(directory) continue;
                const auto ext=upper(e.path().extension().u8string());
                if(rel.compare(0,6,"VIDEO/")==0 || ext==".BNK" || ext==".MP3" || ext==".AAC" || ext==".FLAC" || ext==".WAV") continue;
                if(!layer.emplace(rel,e.path()).second) throw std::runtime_error("Ambiguous game content filenames.");
            }
            if (!required && !layer.empty()) gameplay_overlay = true;
            for(const auto &entry:layer_types) types[entry.first]=entry.second;
            for(const auto &entry:layer) files[entry.first]=entry.second;
        };
        inventory(disc,true,"DATA",""); inventory(overlay,false,"DATA","");
        inventory(overlay,false,"battle-alternates","@BATTLE/");
        if(files.empty()) throw std::runtime_error("No game content could be checked.");
        uint64_t hash=14695981039346656037ULL;
        auto bytes=[&](const void *p,size_t n) { const auto *b=(const unsigned char*)p; for(size_t i=0;i<n;++i) { hash^=b[i]; hash*=1099511628211ULL; } };
        auto number=[&](uint64_t n) { for(int i=0;i<8;++i) { unsigned char b=(unsigned char)(n>>(8*i)); bytes(&b,1); } };
        auto buffer=std::make_unique<std::array<unsigned char,1024*1024>>();
        for(const auto &entry:files) {
            const auto size=fs::file_size(entry.second);
            const auto stamp=fs::last_write_time(entry.second);
            number(entry.first.size()); bytes(entry.first.data(),entry.first.size()); number(size);
            std::unique_ptr<FILE,decltype(&fclose)> f(host_fopen(entry.second.u8string().c_str(),"rb"),fclose);
            if(!f) throw std::runtime_error("A game content file could not be read.");
            uint64_t read=0; size_t n;
            while((n=fread(buffer->data(),1,buffer->size(),f.get()))!=0) { bytes(buffer->data(),n); read+=n; }
            if(ferror(f.get()) || read!=size || fs::file_size(entry.second)!=size || fs::last_write_time(entry.second)!=stamp)
                throw std::runtime_error("Game content changed while checking it. Try again.");
        }
        char text[17]; std::snprintf(text,sizeof text,"%016llx",(unsigned long long)hash);
        return {text,{},gameplay_overlay ? describe_overlay(fs::u8path(overlay)) : ContentProfile{}};
    } catch(const std::exception &e) { return {{},e.what()}; }
}
}
