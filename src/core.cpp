#include "live_skin.hpp"
#include "special_forms.hpp"
#include <rapidjson/document.h>
#include <libproc.h>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
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
        !doc["schema"].IsInt() || doc["schema"].GetInt() != 10) return false;
    auto address = [&](const char* key, uint64_t& value) {
        if (!doc.HasMember(key) || !doc[key].IsString()) return false;
        const char* raw = doc[key].GetString();
        char* end = nullptr;
        value = std::strtoull(raw, &end, 0);
        return end && end != raw && *end == '\0';
    };
    uint64_t champion = 0, skin = 0, stack = 0, gear = 0, character_record = 0;
    uint64_t skin_key = 0, skin_field_ror = 0, skin_value_ror = 0;
    uint64_t skin_pre_ror = 0, skin_add = 0;
    uint64_t animation_slot = 0, animation_graph = 0, clip_map = 0;
    uint64_t skin_resource = 0, skin_properties = 0, gear_list = 0;
    if (!address("frame", out.frame) || !address("setup", out.setup) ||
        !address("preload", out.preload) ||
        !address("actor_refresh", out.actor_refresh) ||
        !address("character_update", out.character_update) ||
        !address("character_prepare", out.character_prepare) ||
        !address("actor_global", out.actor_global) ||
        !address("refresh", out.refresh) ||
        !address("operator_new", out.operator_new) ||
        !address("champion_name_offset", champion) ||
        !address("skin_id_offset", skin) || !address("stack_offset", stack) ||
        !address("skin_id_key", skin_key) ||
        !address("skin_id_field_ror", skin_field_ror) ||
        !address("skin_id_value_ror", skin_value_ror) ||
        !address("skin_id_pre_ror", skin_pre_ror) ||
        !address("skin_id_add", skin_add) ||
        !address("gear_offset", gear) ||
        !address("character_record_offset", character_record) ||
        !address("animation_slot", animation_slot) ||
        !address("animation_graph_offset", animation_graph) ||
        !address("clip_map_offset", clip_map) ||
        !address("skin_resource_offset", skin_resource) ||
        !address("skin_properties_offset", skin_properties) ||
        !address("gear_list_offset", gear_list) ||
        skin_resource > 0xfff8 || skin_resource % 8 != 0 ||
        skin_properties > 0xfff8 || skin_properties % 8 != 0 ||
        gear_list > 0xfff8 || gear_list % 8 != 0 ||
        champion > 0xffff || skin > 0xffff ||
        skin_key > 0xffffffff || skin_field_ror > 31 || skin_value_ror > 31 ||
        skin_pre_ror > 31 || skin_add > 0xffffffff ||
        stack > 0xffff || gear > 0xffff || gear != stack + 0x9c ||
        character_record > 0xffff || character_record % 8 != 0 ||
        animation_slot > 0x7ff8 || animation_slot % 8 != 0 ||
        animation_graph > 0xfff8 || animation_graph % 8 != 0 ||
        clip_map > 0xfff8 || clip_map % 8 != 0 ||
        !doc.HasMember("uuid") || !doc["uuid"].IsString()) return false;
    out.champion_name = static_cast<uint32_t>(champion);
    out.skin_id = static_cast<uint32_t>(skin);
    out.skin_key = static_cast<uint32_t>(skin_key);
    out.skin_field_ror = static_cast<uint32_t>(skin_field_ror);
    out.skin_value_ror = static_cast<uint32_t>(skin_value_ror);
    out.skin_pre_ror = static_cast<uint32_t>(skin_pre_ror);
    out.skin_add = static_cast<uint32_t>(skin_add);
    out.stack = static_cast<uint32_t>(stack);
    out.gear = static_cast<uint32_t>(gear);
    out.character_record = static_cast<uint32_t>(character_record);
    out.animation_slot = static_cast<uint32_t>(animation_slot);
    out.animation_graph = static_cast<uint32_t>(animation_graph);
    out.clip_map = static_cast<uint32_t>(clip_map);
    out.skin_resource = static_cast<uint32_t>(skin_resource);
    out.skin_properties = static_cast<uint32_t>(skin_properties);
    out.gear_list = static_cast<uint32_t>(gear_list);
    std::string uuid = doc["uuid"].GetString();
    uuid.erase(std::remove(uuid.begin(), uuid.end(), '-'), uuid.end());
    if (uuid.size() != 32 || uuid.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        return false;
    for (unsigned i = 0; i < 16; ++i) out.uuid[i] = hex_byte(uuid[i * 2], uuid[i * 2 + 1]);
    return out.frame >= 0x100000000ULL && out.setup >= 0x100000000ULL &&
           out.preload >= 0x100000000ULL &&
           out.actor_refresh >= 0x100000000ULL &&
           out.character_update >= 0x100000000ULL &&
           out.character_prepare >= 0x100000000ULL &&
           out.refresh >= 0x100000000ULL &&
           out.operator_new >= 0x100000000ULL &&
           out.actor_global >= 0x100000000ULL && out.champion_name && out.skin_id &&
           out.animation_slot && out.animation_graph && out.clip_map &&
           out.skin_resource && out.skin_properties && out.gear_list;
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
        std::fprintf(stderr, "usage: stallion-core <liveskin|liveform|skinstate> <pid|0> <skinid|0> --champion:NAME [--gear:N] [--offsets:FILE]\n");
        return 2;
    }
    std::string command = argv[1];
    unsigned requested_pid = 0, skin_id = 0;
    try {
        requested_pid = static_cast<unsigned>(std::stoul(argv[2]));
        skin_id = static_cast<unsigned>(std::stoul(argv[3]));
    } catch (...) { std::fprintf(stderr, "invalid PID or skin ID\n"); return 2; }
    std::string champion;
    int selected_gear = -1;
    fs::path offsets_path = fs::canonical(argv[0]).parent_path() / "offsets.json";
    // built next to the sources by `make`: the profile lives in the project folder above
    if (!fs::exists(offsets_path) && fs::exists(offsets_path.parent_path().parent_path() / "offsets.json"))
        offsets_path = offsets_path.parent_path().parent_path() / "offsets.json";
    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--champion:", 0) == 0) champion = arg.substr(11);
        else if (arg.rfind("--offsets:", 0) == 0) offsets_path = arg.substr(10);
        else if (arg.rfind("--gear:", 0) == 0) {
            try {
                size_t used = 0;
                selected_gear = std::stoi(arg.substr(7), &used);
                if (used != arg.size() - 7) throw std::invalid_argument("gear");
            } catch (...) { std::fprintf(stderr, "invalid gear\n"); return 2; }
        }
    }
    if (champion.empty()) { std::fprintf(stderr, "champion is required\n"); return 2; }
    // A switch may only preselect a form of a known form skin. liveform checks
    // the form against the list of the skin the game has loaded.
    if (command == "liveform" ? selected_gear < -1 || selected_gear >= stallion::kMaxForms
                              : !stallion::valid_gear(champion, skin_id, selected_gear)) {
        std::fprintf(stderr, "this skin has no verified form selector\n"); return 2;
    }
    stallion::Offsets offsets;
    if (!load_offsets(offsets_path, offsets)) {
        std::fprintf(stderr, "invalid offsets profile; run tools/scan_offsets.py\n");
        return 1;
    }
    unsigned pid = requested_pid ? requested_pid : find_game_pid();
    if (!pid) { std::fprintf(stderr, "League match is not running\n"); return 1; }
    if (command == "liveskin") {
        if (stallion::apply_live_skin(pid, skin_id, offsets, champion, selected_gear) != 0) return 1;
    } else if (command == "liveform") {
        return stallion::apply_live_form(pid, skin_id, offsets, champion, selected_gear);
    } else if (command == "skinstate") {
        stallion::LiveSkinState state;
        if (stallion::read_live_skin_state(pid, offsets, champion, state) != 0) return 1;
        std::printf("skinstate pid=%u actor=%llu skin=%u stack_skin=%u gear=%d record=0x%llx record_skin=%u stack_record=0x%llx stack_record_skin=%u\n", pid,
                    static_cast<unsigned long long>(state.actor), state.skin_id,
                    state.stack_skin,
                    static_cast<int>(state.gear),
                    static_cast<unsigned long long>(state.character_record),
                    state.character_record_skin,
                    static_cast<unsigned long long>(state.stack_record),
                    state.stack_record_skin);
    } else { std::fprintf(stderr, "unknown command\n"); return 2; }
    return 0;
}
