#include "live_skin.hpp"
#include <rapidjson/document.h>
#include <libproc.h>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static unsigned char hex_byte(char a, char b) {
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int hi = digit(a), lo = digit(b);
    return hi < 0 || lo < 0 ? 0 : static_cast<unsigned char>(hi * 16 + lo);
}

static bool load_offsets(const fs::path& path, stallion::Offsets& out) {
    std::ifstream input(path);
    std::string json((std::istreambuf_iterator<char>(input)), {});
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("schema") ||
        !doc["schema"].IsInt() || doc["schema"].GetInt() != 1) return false;
    auto address = [&](const char* key, uint64_t& value) {
        if (!doc.HasMember(key) || !doc[key].IsString()) return false;
        const char* raw = doc[key].GetString();
        char* end = nullptr;
        value = std::strtoull(raw, &end, 0);
        return end && end != raw && *end == '\0';
    };
    uint64_t champion = 0, skin = 0;
    if (!address("frame", out.frame) || !address("setup", out.setup) ||
        !address("actor_global", out.actor_global) ||
        !address("champion_name_offset", champion) ||
        !address("skin_id_offset", skin) || champion > 0xffff || skin > 0xffff ||
        !doc.HasMember("uuid") || !doc["uuid"].IsString()) return false;
    out.champion_name = static_cast<uint32_t>(champion);
    out.skin_id = static_cast<uint32_t>(skin);
    std::string uuid = doc["uuid"].GetString();
    uuid.erase(std::remove(uuid.begin(), uuid.end(), '-'), uuid.end());
    if (uuid.size() != 32 || uuid.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        return false;
    for (unsigned i = 0; i < 16; ++i) out.uuid[i] = hex_byte(uuid[i * 2], uuid[i * 2 + 1]);
    return out.frame >= 0x100000000ULL && out.setup >= 0x100000000ULL &&
           out.actor_global >= 0x100000000ULL && out.champion_name && out.skin_id;
}

static unsigned find_game_pid() {
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes <= 0) return 0;
    std::vector<pid_t> pids(bytes / sizeof(pid_t) + 64);
    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), pids.size() * sizeof(pid_t));
    if (bytes <= 0) return 0;
    for (int i = 0; i < bytes / static_cast<int>(sizeof(pid_t)); ++i) {
        if (pids[i] <= 0) continue;
        char path[PROC_PIDPATHINFO_MAXSIZE] = {};
        if (proc_pidpath(pids[i], path, sizeof(path)) <= 0) continue;
        if (fs::path(path).filename() == "LeagueofLegends") return static_cast<unsigned>(pids[i]);
    }
    return 0;
}

static void on_signal(int signal) {
    stallion::emergency_resume_live_skin();
    std::_Exit(128 + signal);
}

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGHUP, on_signal);
    if (argc < 4) {
        std::fprintf(stderr, "usage: stallion-core <liveskin|skinstate> <pid|0> <skinid|0> --champion:NAME [--offsets:FILE]\n");
        return 2;
    }
    std::string command = argv[1];
    unsigned requested_pid = 0, skin_id = 0;
    try {
        requested_pid = static_cast<unsigned>(std::stoul(argv[2]));
        skin_id = static_cast<unsigned>(std::stoul(argv[3]));
    } catch (...) { std::fprintf(stderr, "invalid PID or skin ID\n"); return 2; }
    std::string champion;
    fs::path offsets_path = fs::canonical(argv[0]).parent_path() / "offsets.json";
    // built next to the sources by `make`: the profile lives in the project folder above
    if (!fs::exists(offsets_path) && fs::exists(offsets_path.parent_path().parent_path() / "offsets.json"))
        offsets_path = offsets_path.parent_path().parent_path() / "offsets.json";
    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--champion:", 0) == 0) champion = arg.substr(11);
        else if (arg.rfind("--offsets:", 0) == 0) offsets_path = arg.substr(10);
    }
    if (champion.empty()) { std::fprintf(stderr, "champion is required\n"); return 2; }
    stallion::Offsets offsets;
    if (!load_offsets(offsets_path, offsets)) {
        std::fprintf(stderr, "invalid offsets profile; run tools/scan_offsets.py\n");
        return 1;
    }
    unsigned pid = requested_pid ? requested_pid : find_game_pid();
    if (!pid) { std::fprintf(stderr, "League match is not running\n"); return 1; }
    if (command == "liveskin") {
        if (stallion::apply_live_skin(pid, skin_id, offsets, champion) != 0) return 1;
    } else if (command == "skinstate") {
        stallion::LiveSkinState state;
        if (stallion::read_live_skin_state(pid, offsets, champion, state) != 0) return 1;
        std::printf("skinstate pid=%u actor=%llu skin=%u\n", pid,
                    static_cast<unsigned long long>(state.actor), state.skin_id);
    } else { std::fprintf(stderr, "unknown command\n"); return 2; }
    return 0;
}
