#include <Cocoa/Cocoa.h>
#include <ApplicationServices/ApplicationServices.h>
#include <OpenGL/gl3.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>
#include <pthread/qos.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <unistd.h>
#include <libproc.h>

extern char** environ;

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
struct Skin { unsigned number; std::string name; };
struct Champion { std::string name; std::vector<Skin> skins; };
static std::vector<Champion> load_skins(const fs::path& path);

struct CapturedKey {
    CGEventType type;
    CGKeyCode code;
    CGEventFlags flags;
    std::u16string text;
};
struct CapturedMouse {
    float x, y;
    int button;
    bool down;
    bool moved;
};

// Where the overlay is, as far as the event tap needs to know.
struct OverlayGeometry {
    bool valid = false;
    double target_x = 0, target_y = 0;  // where we asked for the window (global, top-left origin)
    double width = 0, height = 0;
    bool measured = false;              // WindowServer's position is fresh and stable
    double ws_x = 0, ws_y = 0;
};

struct MouseMapping {
    double x, y;
    bool measured;
};

// Cursor position relative to the overlay's top-left corner. Both halves come from
// WindowServer: the cursor from the event itself and the window from the position it
// reports for our window, which stays right if the OS moved the window or converted
// coordinates with a stale screen height. The requested position is only used until
// such a measurement exists. AppKit's [NSEvent mouseLocation] is deliberately not
// used: after a display-mode switch it stopped following the cursor.
static MouseMapping map_mouse(const OverlayGeometry& g, CGPoint global) {
    const double origin_x = g.measured ? g.ws_x : g.target_x;
    const double origin_y = g.measured ? g.ws_y : g.target_y;
    return {global.x - origin_x, global.y - origin_y, g.measured};
}

struct KeyboardCapture {
    std::atomic<bool> text_active{false};
    std::atomic<bool> overlay_visible{false};
    std::atomic<bool> mouse_captured{false};
    std::atomic<bool> mouse_inside{false};
    std::atomic<bool> wake_pending{false};
    std::atomic<pid_t> game_pid{0};
    std::atomic<int> global_mouse_x{0}, global_mouse_y{0};
    std::mutex geometry_mutex;
    OverlayGeometry geometry;
    std::mutex mutex;
    std::vector<CapturedKey> pending;
    std::vector<CapturedMouse> mouse_pending;
    double wheel = 0;
    CFMachPortRef tap = nullptr;
    CFRunLoopSourceRef source = nullptr;
    CFRunLoopRef runloop = nullptr;
};

// The render loop sleeps until something happens; input arrives on the tap thread.
static void wake_main(KeyboardCapture* capture) {
    if (!capture->wake_pending.exchange(true)) glfwPostEmptyEvent();
}

static CGEventRef keyboard_tap(CGEventTapProxy, CGEventType type,
                               CGEventRef event, void* context) {
    auto* capture = static_cast<KeyboardCapture*>(context);
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (capture->tap) CGEventTapEnable(capture->tap, true);
        return event;
    }
    NSRunningApplication* frontmost = [[NSWorkspace sharedWorkspace] frontmostApplication];
    if (!frontmost || frontmost.processIdentifier != capture->game_pid.load() ||
        !capture->overlay_visible.load()) {
        capture->text_active.store(false);
        capture->mouse_captured.store(false);
        capture->mouse_inside.store(false);
        return event;
    }
    if (type == kCGEventMouseMoved || type == kCGEventLeftMouseDragged ||
        type == kCGEventLeftMouseDown || type == kCGEventLeftMouseUp ||
        type == kCGEventScrollWheel) {
        const CGPoint point = CGEventGetLocation(event);
        capture->global_mouse_x.store(static_cast<int>(point.x));
        capture->global_mouse_y.store(static_cast<int>(point.y));
        OverlayGeometry g;
        { std::lock_guard<std::mutex> lock(capture->geometry_mutex); g = capture->geometry; }
        if (!g.valid) return event;
        const MouseMapping mapped = map_mouse(g, point);
        const float x = static_cast<float>(mapped.x);
        const float y = static_cast<float>(mapped.y);
        const bool inside = x >= 0 && y >= 0 && x < g.width && y < g.height;
        const bool dragging = capture->mouse_captured.load();
        if (type == kCGEventScrollWheel) {
            if (!inside) return event;
            const double lines = CGEventGetDoubleValueField(event, kCGScrollWheelEventFixedPtDeltaAxis1);
            { std::lock_guard<std::mutex> lock(capture->mutex);
              capture->mouse_pending.push_back({x, y, 0, false, true});
              capture->wheel += lines; }
            wake_main(capture);
            return nullptr;
        }
        if (type == kCGEventLeftMouseDown && !inside) {
            capture->text_active.store(false);
            capture->mouse_inside.store(false);
            { std::lock_guard<std::mutex> lock(capture->mutex);
              capture->mouse_pending.push_back({x, y, 0, true, false});
              capture->mouse_pending.push_back({x, y, 0, false, false}); }
            wake_main(capture);
            return event;
        }
        if (!inside && !dragging) {
            // The pointer just left: tell ImGui there is no mouse, or the last row keeps
            // its hover highlight and the software cursor stays where it left.
            if (capture->mouse_inside.exchange(false)) {
                { std::lock_guard<std::mutex> lock(capture->mutex);
                  capture->mouse_pending.push_back({-FLT_MAX, -FLT_MAX, 0, false, true}); }
                wake_main(capture);
            }
            return event;
        }
        capture->mouse_inside.store(inside);
        if (type == kCGEventLeftMouseDown) capture->mouse_captured.store(true);
        if (type == kCGEventLeftMouseUp) capture->mouse_captured.store(false);
        { std::lock_guard<std::mutex> lock(capture->mutex);
          capture->mouse_pending.push_back({x, y, 0,
              type == kCGEventLeftMouseDown,
              type == kCGEventMouseMoved || type == kCGEventLeftMouseDragged}); }
        wake_main(capture);
        return nullptr;
    }
    if (!capture->overlay_visible.load() || !capture->text_active.load() ||
        (CGEventGetFlags(event) & kCGEventFlagMaskCommand)) return event;
    if (type != kCGEventKeyDown && type != kCGEventKeyUp &&
        type != kCGEventFlagsChanged) return event;
    CapturedKey key{type,
        static_cast<CGKeyCode>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode)),
        CGEventGetFlags(event), {}};
    if (type == kCGEventKeyDown) {
        UniChar chars[32] = {};
        UniCharCount length = 0;
        CGEventKeyboardGetUnicodeString(event, 32, &length, chars);
        for (UniCharCount i = 0; i < length; ++i) key.text.push_back(chars[i]);
    }
    { std::lock_guard<std::mutex> lock(capture->mutex);
      capture->pending.push_back(std::move(key)); }
    wake_main(capture);
    return nullptr;
}

static ImGuiKey imgui_key(CGKeyCode code) {
    switch (code) {
    case 51: return ImGuiKey_Backspace;
    case 117: return ImGuiKey_Delete;
    case 36: case 76: return ImGuiKey_Enter;
    case 48: return ImGuiKey_Tab;
    case 53: return ImGuiKey_Escape;
    case 123: return ImGuiKey_LeftArrow;
    case 124: return ImGuiKey_RightArrow;
    case 125: return ImGuiKey_DownArrow;
    case 126: return ImGuiKey_UpArrow;
    case 115: return ImGuiKey_Home;
    case 119: return ImGuiKey_End;
    default: return ImGuiKey_None;
    }
}

static void forward_captured_keys(KeyboardCapture& capture) {
    std::vector<CapturedKey> keys;
    std::vector<CapturedMouse> mouse;
    double wheel = 0;
    capture.wake_pending.store(false);
    { std::lock_guard<std::mutex> lock(capture.mutex);
      keys.swap(capture.pending); mouse.swap(capture.mouse_pending);
      wheel = capture.wheel; capture.wheel = 0; }
    ImGuiIO& io = ImGui::GetIO();
    for (const auto& item : mouse) {
        io.AddMousePosEvent(item.x, item.y);
        if (!item.moved) io.AddMouseButtonEvent(item.button, item.down);
    }
    if (wheel != 0) io.AddMouseWheelEvent(0.0f, static_cast<float>(wheel));
    for (const auto& key : keys) {
        io.AddKeyEvent(ImGuiMod_Shift, (key.flags & kCGEventFlagMaskShift) != 0);
        io.AddKeyEvent(ImGuiMod_Ctrl, (key.flags & kCGEventFlagMaskControl) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (key.flags & kCGEventFlagMaskAlternate) != 0);
        const ImGuiKey mapped = imgui_key(key.code);
        if (mapped != ImGuiKey_None)
            io.AddKeyEvent(mapped, key.type != kCGEventKeyUp);
        if (key.type == kCGEventKeyDown)
            for (char16_t character : key.text)
                if (character >= 32) io.AddInputCharacterUTF16(character);
    }
}

static std::string read_file(const fs::path& path) {
    std::ifstream input(path);
    return std::string((std::istreambuf_iterator<char>(input)), {});
}

static bool process_ok(pid_t child) {
    if (child < 0) return false;
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// posix_spawn instead of fork(): fork() copies the page tables of this GL process,
// which stalls the render thread, and update_catalog() starts one child per champion.
static pid_t spawn_process(const char* program, const std::vector<std::string>& args,
                           int output_fd = -1) {
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (output_fd >= 0) {
        posix_spawn_file_actions_adddup2(&actions, output_fd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, output_fd, STDERR_FILENO);
    }
    pid_t child = 0;
    const int error = posix_spawnp(&child, program, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    return error == 0 ? child : -1;
}

static bool update_offsets(const fs::path& directory, const fs::path& game) {
    const fs::path script = directory / "tools" / "scan_offsets.py";
    const fs::path output = directory / "offsets.json";
    return process_ok(spawn_process("python3", {"python3", script.string(), "--game",
                                                game.string(), "--output", output.string()}));
}

static bool fetch_url(const std::string& url, const fs::path& output) {
    return process_ok(spawn_process("/usr/bin/curl", {"curl", "-fLsS", "--connect-timeout", "4",
                                                      "--max-time", "15", url, "-o",
                                                      output.string()}));
}

// "16.19" from the game's content-metadata.json, empty if it cannot be read.
static std::string installed_patch(const fs::path& game) {
    rapidjson::Document meta;
    meta.Parse(read_file(game.parent_path().parent_path().parent_path().parent_path() /
                         "content-metadata.json").c_str());
    if (meta.HasParseError() || !meta.IsObject() || !meta.HasMember("version") ||
        !meta["version"].IsString()) return "";
    const std::string version = meta["version"].GetString();
    const size_t first = version.find('.'), second = version.find('.', first + 1);
    return first == std::string::npos || second == std::string::npos ? "" : version.substr(0, second);
}

static bool update_catalog(const fs::path& directory, const fs::path& game) {
    const fs::path metadata = game.parent_path().parent_path().parent_path().parent_path()
        / "content-metadata.json";
    rapidjson::Document meta;
    const std::string meta_text = read_file(metadata);
    meta.Parse(meta_text.c_str());
    if (meta.HasParseError() || !meta.IsObject() || !meta.HasMember("version") ||
        !meta["version"].IsString()) return false;
    const std::string installed = meta["version"].GetString();
    const size_t first = installed.find('.'), second = installed.find('.', first + 1);
    if (first == std::string::npos || second == std::string::npos) return false;
    const std::string patch = installed.substr(0, second);
    const fs::path output = directory / "skin_ids.json";
    const fs::path version_file = directory / "skin_ids.version";
    const fs::path scratch = directory / "build" / "riot_catalog";
    std::error_code error;
    fs::create_directories(scratch, error);
    if (error) return false;
    const fs::path versions_path = scratch / "versions.json";
    if (!fetch_url("https://ddragon.leagueoflegends.com/api/versions.json", versions_path)) return false;
    rapidjson::Document versions;
    const std::string versions_text = read_file(versions_path);
    versions.Parse(versions_text.c_str());
    if (versions.HasParseError() || !versions.IsArray()) return false;
    std::string version;
    for (const auto& value : versions.GetArray()) {
        if (value.IsString() && std::string(value.GetString()).rfind(patch + ".", 0) == 0) {
            version = value.GetString(); break;
        }
    }
    if (version.empty()) return false;
    const std::string base = "https://ddragon.leagueoflegends.com/cdn/" + version +
        "/data/en_US/";
    const fs::path summary_path = scratch / "champion.json";
    if (!fetch_url(base + "champion.json", summary_path)) return false;
    rapidjson::Document summary;
    const std::string summary_text = read_file(summary_path);
    summary.Parse(summary_text.c_str());
    if (summary.HasParseError() || !summary.IsObject() || !summary.HasMember("data") ||
        !summary["data"].IsObject()) return false;
    std::vector<std::string> ids;
    for (auto it = summary["data"].MemberBegin(); it != summary["data"].MemberEnd(); ++it) {
        std::string id = it->name.GetString();
        if (!id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_';
            })) ids.push_back(std::move(id));
    }
    if (ids.size() < 100) return false;
    std::vector<std::future<bool>> jobs;
    bool downloaded = true;
    for (size_t i = 0; i < ids.size(); ++i) {
        const auto id = ids[i];
        jobs.push_back(std::async(std::launch::async, [base, scratch, id] {
            return fetch_url(base + "champion/" + id + ".json", scratch / (id + ".json"));
        }));
        if (jobs.size() == 12 || i + 1 == ids.size()) {
            for (auto& job : jobs) downloaded = job.get() && downloaded;
            jobs.clear();
        }
    }
    if (!downloaded) return false;
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    unsigned count = 0;
    for (const auto& id : ids) {
        rapidjson::Document doc;
        const std::string contents = read_file(scratch / (id + ".json"));
        doc.Parse(contents.c_str());
        if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("data") ||
            !doc["data"].IsObject() || !doc["data"].HasMember(id.c_str())) return false;
        const auto& champion = doc["data"][id.c_str()];
        if (!champion.IsObject() || !champion.HasMember("skins") ||
            !champion["skins"].IsArray() || !champion.HasMember("name") ||
            !champion["name"].IsString()) return false;
        for (const auto& skin : champion["skins"].GetArray()) {
            if (!skin.IsObject() || !skin.HasMember("id") || !skin.HasMember("name") ||
                !skin.HasMember("num") || !skin["name"].IsString() ||
                !skin["num"].IsUint()) continue;
            std::string full;
            if (skin["id"].IsString()) full = skin["id"].GetString();
            else if (skin["id"].IsUint64()) full = std::to_string(skin["id"].GetUint64());
            if (full.empty()) continue;
            writer.Key(full.c_str());
            writer.String(skin["num"].GetUint() == 0 ? champion["name"].GetString()
                                                    : skin["name"].GetString());
            ++count;
        }
    }
    writer.EndObject();
    if (count < 1000) return false;
    const fs::path temp = directory / "skin_ids.json.tmp";
    { std::ofstream file(temp); file << buffer.GetString(); }
    if (load_skins(temp).size() < 100) return false;
    fs::rename(temp, output, error);
    if (error) return false;
    { std::ofstream file(version_file); file << patch; }
    return read_file(output) == buffer.GetString() && read_file(version_file) == patch;
}

// ---------------------------------------------------------------------------
// Game tracking. All process and WindowServer queries live on a helper thread:
// each one can block for tens of milliseconds while the game saturates the GPU,
// and on the render thread that showed up as overlay freezes.
// ---------------------------------------------------------------------------

static pid_t find_game_pid(const fs::path& selected_game, std::string* matched_path) {
    const std::string name = selected_game.filename().string();
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes <= 0) return 0;
    std::vector<pid_t> pids(bytes / sizeof(pid_t) + 64);
    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), pids.size() * sizeof(pid_t));
    for (int i = 0; i < bytes / static_cast<int>(sizeof(pid_t)); ++i) {
        if (pids[i] <= 0) continue;
        char path[PROC_PIDPATHINFO_MAXSIZE] = {};
        if (proc_pidpath(pids[i], path, sizeof(path)) <= 0) continue;
        const char* slash = std::strrchr(path, '/');
        if (!slash || name != slash + 1) continue;  // cheap filter before any stat()
        std::error_code error;
        if (fs::equivalent(fs::path(path), selected_game, error) && !error) {
            *matched_path = path;
            return pids[i];
        }
    }
    return 0;
}

static bool pid_still_game(pid_t pid, const std::string& matched_path) {
    char path[PROC_PIDPATHINFO_MAXSIZE] = {};
    return proc_pidpath(pid, path, sizeof(path)) > 0 && matched_path == path;
}

static CFArrayRef describe_window(CGWindowID id) {
    const void* values[] = {reinterpret_cast<const void*>(static_cast<uintptr_t>(id))};
    CFArrayRef ids = CFArrayCreate(kCFAllocatorDefault, values, 1, nullptr);
    CFArrayRef info = CGWindowListCreateDescriptionFromArray(ids);
    CFRelease(ids);
    return info;
}

static bool window_int(CFDictionaryRef info, CFStringRef key, int* value) {
    CFNumberRef number = static_cast<CFNumberRef>(CFDictionaryGetValue(info, key));
    return number && CFNumberGetValue(number, kCFNumberIntType, value);
}

static bool window_bounds(CFDictionaryRef info, CGRect* bounds) {
    CFDictionaryRef rectangle = static_cast<CFDictionaryRef>(
        CFDictionaryGetValue(info, kCGWindowBounds));
    return rectangle && CGRectMakeWithDictionaryRepresentation(rectangle, bounds);
}

static bool window_on_screen(CFDictionaryRef info) {
    CFBooleanRef on = static_cast<CFBooleanRef>(CFDictionaryGetValue(info, kCGWindowIsOnscreen));
    return on && CFBooleanGetValue(on);
}

static bool usable_game_window(CFDictionaryRef info, pid_t pid, CGRect* bounds) {
    int owner = 0;
    CGRect rectangle = CGRectZero;
    if (!window_int(info, kCGWindowOwnerPID, &owner) || owner != pid ||
        !window_bounds(info, &rectangle) ||
        rectangle.size.width < 600 || rectangle.size.height < 400) return false;
    *bounds = rectangle;
    return true;
}

// Bounds (global display coordinates, origin top-left) of the game's window.
static bool find_game_window(pid_t pid, CGWindowID* cached, CGRect* bounds) {
    if (*cached) {
        bool ok = false;
        if (CFArrayRef list = describe_window(*cached)) {
            if (CFArrayGetCount(list) == 1) {
                CFDictionaryRef info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, 0));
                ok = window_on_screen(info) && usable_game_window(info, pid, bounds);
            }
            CFRelease(list);
        }
        if (ok) return true;
        *cached = kCGNullWindowID;
    }
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!list) return false;
    bool found = false;
    for (CFIndex i = 0; i < CFArrayGetCount(list) && !found; ++i) {
        CFDictionaryRef info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
        int number = 0;
        if (usable_game_window(info, pid, bounds) && window_int(info, kCGWindowNumber, &number)) {
            *cached = static_cast<CGWindowID>(number);
            found = true;
        }
    }
    CFRelease(list);
    return found;
}

// Where WindowServer says a window really is, in the same coordinates as the game's.
static bool measure_window(CGWindowID id, CGRect* bounds) {
    bool ok = false;
    if (CFArrayRef list = describe_window(id)) {
        if (CFArrayGetCount(list) == 1)
            ok = window_bounds(static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, 0)), bounds);
        CFRelease(list);
    }
    return ok;
}

// The part of the game window that is actually on a display.
static CGRect visible_game_area(CGRect game, CGRect* display_bounds) {
    const CGPoint center = CGPointMake(CGRectGetMidX(game), CGRectGetMidY(game));
    CGDirectDisplayID display = 0;
    uint32_t count = 0;
    *display_bounds = CGDisplayBounds(CGMainDisplayID());
    if (CGGetDisplaysWithPoint(center, 1, &display, &count) == kCGErrorSuccess && count == 1) {
        *display_bounds = CGDisplayBounds(display);
        const CGRect clipped = CGRectIntersection(game, *display_bounds);
        if (!CGRectIsNull(clipped) && clipped.size.width >= 300 && clipped.size.height >= 200)
            return clipped;
    }
    return game;
}

struct GameSnapshot {
    pid_t pid = 0;
    bool foreground = false;   // the game is the active app and has a real window
    bool stable = false;       // its size and the display mode have not changed recently
    CGRect rect = CGRectZero;  // usable game area, global display coordinates
    bool overlay_valid = false;
    CGRect overlay = CGRectZero;  // where WindowServer put our window
    Clock::time_point overlay_time{};
};

struct GameWatcher {
    fs::path game;
    std::atomic<CGWindowID> overlay_id{0};
    std::atomic<bool> overlay_shown{false};
    std::atomic<uint32_t> display_epoch{0};
    std::atomic<bool> stop{false};
    std::mutex mutex;
    GameSnapshot snapshot;
    std::thread thread;
    GameSnapshot get() { std::lock_guard<std::mutex> lock(mutex); return snapshot; }
};

static void display_reconfigured(CGDirectDisplayID, CGDisplayChangeSummaryFlags, void* context) {
    static_cast<GameWatcher*>(context)->display_epoch.fetch_add(1);
    glfwPostEmptyEvent();
}

static void watch_game(GameWatcher* watcher) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    struct Shape {
        pid_t pid; CGSize size; CGRect display; uint32_t epoch;
        bool operator==(const Shape& o) const {
            return pid == o.pid && CGSizeEqualToSize(size, o.size) &&
                   CGRectEqualToRect(display, o.display) && epoch == o.epoch;
        }
    };
    pid_t pid = 0;
    std::string matched_path;
    CGWindowID game_window = kCGNullWindowID;
    Clock::time_point next_scan{}, last_foreground{};
    Clock::time_point stable_since = Clock::now();
    Shape last_shape{};
    GameSnapshot published;
    while (!watcher->stop.load()) {
        std::chrono::milliseconds delay = 500ms;
        @autoreleasepool {
            const auto now = Clock::now();
            if (pid && !pid_still_game(pid, matched_path)) { pid = 0; game_window = kCGNullWindowID; }
            if (!pid && now >= next_scan) {
                pid = find_game_pid(watcher->game, &matched_path);
                next_scan = now + 500ms;
            }
            GameSnapshot next;
            next.pid = pid;
            next.rect = published.rect;
            CGRect display = CGDisplayBounds(CGMainDisplayID());
            bool active = false;
            if (pid) {
                NSRunningApplication* app = [[NSWorkspace sharedWorkspace] frontmostApplication];
                CGRect bounds = CGRectZero;
                active = app && app.processIdentifier == pid &&
                         find_game_window(pid, &game_window, &bounds);
                if (active) next.rect = visible_game_area(bounds, &display);
                delay = active ? 50ms : 150ms;
            }
            if (active) last_foreground = now;
            // Ride out one-poll flickers while the game changes display mode.
            next.foreground = active || (published.foreground && now - last_foreground < 120ms);
            const Shape shape{pid, next.rect.size, display, watcher->display_epoch.load()};
            if (!(shape == last_shape)) { last_shape = shape; stable_since = now; }
            next.stable = now - stable_since >= 350ms;
            const CGWindowID overlay_id = watcher->overlay_id.load();
            if (overlay_id && watcher->overlay_shown.load() &&
                measure_window(overlay_id, &next.overlay)) {
                next.overlay_valid = true;
                next.overlay_time = Clock::now();
            }
            bool changed;
            { std::lock_guard<std::mutex> lock(watcher->mutex);
              changed = published.pid != next.pid || published.foreground != next.foreground ||
                        published.stable != next.stable ||
                        !CGRectEqualToRect(published.rect, next.rect);
              watcher->snapshot = next; }
            published = next;
            if (changed) glfwPostEmptyEvent();
        }
        std::this_thread::sleep_for(delay);
    }
}

// AppKit's y axis starts at the bottom of the primary screen, WindowServer's and the
// event tap's at the top. Convert with AppKit's own screen height. GLFW uses
// CGDisplayBounds() instead, which changes the instant League switches the display
// mode while AppKit still reports the old height, so a window placed at that moment
// lands hundreds of points off-screen.
static void place_window(NSWindow* window, CGPoint top_left) {
    NSScreen* primary = [[NSScreen screens] firstObject];
    const CGFloat height = primary ? primary.frame.size.height
                                   : CGDisplayBounds(CGMainDisplayID()).size.height;
    [window setFrameOrigin:NSMakePoint(top_left.x, height - top_left.y - window.frame.size.height)];
}

// Shift by a delta measured in WindowServer coordinates; independent of any height.
static void nudge_window(NSWindow* window, CGFloat dx, CGFloat dy) {
    const NSPoint origin = window.frame.origin;
    [window setFrameOrigin:NSMakePoint(origin.x + dx, origin.y - dy)];
}

// ---------------------------------------------------------------------------
// Overlay size and position are remembered between runs (overlay.cfg).
// ---------------------------------------------------------------------------

constexpr int kBaseWidth = 440, kBaseHeight = 540;
constexpr int kMinWidth = 352, kMaxWidth = 1100, kMinHeight = 300, kMaxHeight = 1600;

struct OverlayPrefs {
    int width = kBaseWidth, height = kBaseHeight;
    bool positioned = false;               // moved by the user; else top-right corner
    double gap_right = 40, gap_top = 90;   // distance from the game area's top-right corner
    bool cursor = true;                    // draw a software cursor over the overlay
};

// Text and spacing scale with the overlay's width so the layout never breaks.
static float overlay_scale(int width) {
    return std::clamp(static_cast<float>(width) / kBaseWidth, 0.8f, 2.4f);
}

// Smallest height at which both lists, the button and a two-line message still fit.
static int overlay_min_height(int width) {
    return std::max(kMinHeight, static_cast<int>(std::lround(330 * overlay_scale(width))));
}

static OverlayPrefs load_prefs(const fs::path& path) {
    OverlayPrefs prefs;
    std::istringstream lines(read_file(path));
    std::string line;
    while (std::getline(lines, line)) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        const double value = std::atof(line.c_str() + equals + 1);
        if (key == "width") prefs.width = static_cast<int>(value);
        else if (key == "height") prefs.height = static_cast<int>(value);
        else if (key == "gap_right") { prefs.gap_right = value; prefs.positioned = true; }
        else if (key == "gap_top") { prefs.gap_top = value; prefs.positioned = true; }
        else if (key == "cursor") prefs.cursor = value != 0;
    }
    prefs.width = std::clamp(prefs.width, kMinWidth, kMaxWidth);
    prefs.height = std::clamp(prefs.height, overlay_min_height(prefs.width), kMaxHeight);
    prefs.gap_right = std::clamp(prefs.gap_right, -2000.0, 6000.0);
    prefs.gap_top = std::clamp(prefs.gap_top, -2000.0, 6000.0);
    return prefs;
}

static void save_prefs(const fs::path& path, const OverlayPrefs& prefs) {
    std::ofstream file(path);
    file << "width=" << prefs.width << "\nheight=" << prefs.height << "\n";
    if (prefs.positioned)
        file << "gap_right=" << std::lround(prefs.gap_right)
             << "\ngap_top=" << std::lround(prefs.gap_top) << "\n";
    file << "cursor=" << (prefs.cursor ? 1 : 0) << "\n";
}

// Rasterize the font at the size it is drawn at; scaling a 13 px atlas up blurs it.
static void rebuild_font(int pixels) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig config;
    config.SizePixels = static_cast<float>(pixels);
    config.OversampleH = config.OversampleV = 1;  // same settings as ImGui's own default
    config.PixelSnapH = true;
    io.Fonts->AddFontDefault(&config);
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
}

static std::vector<Champion> load_skins(const fs::path& path) {
    std::ifstream input(path);
    std::string json((std::istreambuf_iterator<char>(input)), {});
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    if (doc.HasParseError() || !doc.IsObject()) return {};
    std::map<unsigned, Champion> by_id;
    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        if (!it->value.IsString()) continue;
        char* end = nullptr;
        unsigned long full = std::strtoul(it->name.GetString(), &end, 10);
        if (!end || *end != '\0') continue;
        unsigned number = full % 1000;
        if (number > 255) continue;
        unsigned id = full / 1000;
        if (number == 0) by_id[id].name = it->value.GetString();
        else by_id[id].skins.push_back({number, it->value.GetString()});
    }
    std::vector<Champion> result;
    for (auto& [id, champ] : by_id) {
        if (champ.name.empty()) continue;
        champ.skins.push_back({0, "Default"});
        std::sort(champ.skins.begin(), champ.skins.end(), [](const Skin& a, const Skin& b) {
            return a.number < b.number;
        });
        result.push_back(std::move(champ));
    }
    std::sort(result.begin(), result.end(), [](const Champion& a, const Champion& b) {
        return a.name < b.name;
    });
    return result;
}

static std::string run_command(const fs::path& tool, const char* command, pid_t game_pid,
                               const std::string& champion, unsigned skin) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return "Could not create output pipe";
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    const pid_t child = spawn_process(tool.c_str(),
        {tool.string(), command, std::to_string(game_pid), std::to_string(skin),
         "--champion:" + champion}, pipefd[1]);
    close(pipefd[1]);
    if (child < 0) { close(pipefd[0]); return "Could not start mod-tools"; }
    std::string output;
    char buffer[1024];
    ssize_t count;
    while ((count = read(pipefd[0], buffer, sizeof buffer)) > 0) output.append(buffer, count);
    close(pipefd[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return std::strcmp(command, "skinstate") == 0 ? output : "Skin changed in the current match";
    return output.empty() ? "Live switch failed" : output;
}

int main(int argc, char** argv) {
    fs::path executable = fs::canonical(argv[0]);
    fs::path directory = executable.parent_path();
    // `make` puts the binary in build/, but offsets, skins, tools and the core live in the
    // project folder above it.
    if (!fs::exists(directory / "tools" / "scan_offsets.py") &&
        fs::exists(directory.parent_path() / "tools" / "scan_offsets.py"))
        directory = directory.parent_path();
    const fs::path lock_path = directory / ".stallion.lock";
    int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        std::fprintf(stderr, "Stallion77 Skin Changer is already running.\n");
        return 1;
    }
    fs::path selected = argc > 1 ? argv[1] : fs::path("/Applications/League of Legends.app");
    fs::path game = selected;
    if (selected.filename() == "League of Legends.app")
        game = selected / "Contents/LoL/Game/LeagueofLegends.app/Contents/MacOS/LeagueofLegends";
    else if (selected.filename() == "LeagueofLegends.app")
        game = selected / "Contents/MacOS/LeagueofLegends";
    bool profile_ready = update_offsets(directory, game);
    // The skin list is only downloaded when the Refresh button is pressed. It counts as
    // current when it was built for the installed patch.
    std::future<bool> catalog_task;
    const std::string patch = installed_patch(game);
    std::string built_for = read_file(directory / "skin_ids.version");
    while (!built_for.empty() && std::isspace(static_cast<unsigned char>(built_for.back()))) built_for.pop_back();
    bool catalog_ready = !patch.empty() && built_for == patch && fs::exists(directory / "skin_ids.json");
    fs::path data = directory / "skin_ids.json";
    if (!fs::exists(data)) data = directory / "data" / "skin_ids.json";
    auto champions = load_skins(data);
    if (champions.empty()) {
        std::fprintf(stderr, "Cannot load skin catalog: %s\n", data.c_str());
        return 1;
    }
    fs::path tool = directory / "stallion-core";
    if (!fs::exists(tool) && fs::exists(directory / "build" / "stallion-core"))
        tool = directory / "build" / "stallion-core";
    if (!fs::exists(tool)) {
        std::fprintf(stderr, "stallion-core must be next to this executable\n");
        return 1;
    }
    const fs::path prefs_path = directory / "overlay.cfg";
    OverlayPrefs prefs = load_prefs(prefs_path);
    if (!glfwInit()) return 1;
    // This is an overlay that must keep up with a game using every core; do not let
    // App Nap or a background QoS stretch its timers.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    id activity = [[NSProcessInfo processInfo] beginActivityWithOptions:
        (NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical)
        reason:@"Track the game window and draw the overlay"];
    CFRetain((__bridge CFTypeRef)activity);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    // The overlay must never take focus from the game: input reaches it through the
    // event tap, and once the game stops being the active app the overlay hides.
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(prefs.width, prefs.height, "Stallion77 Skin Changer",
                                          nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    NSWindow* native = glfwGetCocoaWindow(window);
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    // League's full-screen window occupies a private high WindowServer layer.
    [native setLevel:2147483630];
    [native setHidesOnDeactivate:NO];
    NSWindowCollectionBehavior behavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorFullScreenAuxiliary |
        NSWindowCollectionBehaviorStationary;
    [native setCollectionBehavior:behavior];
    [native setOpaque:NO];
    [native setBackgroundColor:[NSColor clearColor]];
    [native setHasShadow:NO];
    [native setMovableByWindowBackground:NO];
    [native setAnimationBehavior:NSWindowAnimationBehaviorNone];
    glfwHideWindow(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    // The window never has focus, so the OS cursor is not ours to change.
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    const ImGuiStyle base_style = ImGui::GetStyle();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");
    int font_px = 13;
    // Compile the shaders and upload the font atlas now instead of on the first
    // visible frame.
    ImGui_ImplOpenGL3_NewFrame();
    KeyboardCapture keyboard;
    keyboard.tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
        CGEventMaskBit(kCGEventFlagsChanged) | CGEventMaskBit(kCGEventMouseMoved) |
        CGEventMaskBit(kCGEventLeftMouseDragged) | CGEventMaskBit(kCGEventLeftMouseDown) |
        CGEventMaskBit(kCGEventLeftMouseUp) | CGEventMaskBit(kCGEventScrollWheel),
        keyboard_tap, &keyboard);
    if (keyboard.tap) {
        keyboard.source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, keyboard.tap, 0);
    }
    std::promise<void> tap_ready;
    std::thread tap_thread;
    if (keyboard.source) {
        tap_thread = std::thread([&] {
            // Every mouse and key event in the session waits on this callback.
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            keyboard.runloop = CFRunLoopGetCurrent();
            CFRetain(keyboard.runloop);
            CFRunLoopAddSource(keyboard.runloop, keyboard.source, kCFRunLoopCommonModes);
            CGEventTapEnable(keyboard.tap, true);
            tap_ready.set_value();
            CFRunLoopRun();
            CFRunLoopRemoveSource(keyboard.runloop, keyboard.source, kCFRunLoopCommonModes);
            CFRelease(keyboard.runloop);
        });
        tap_ready.get_future().wait();
        CFRetain(keyboard.runloop);
    }
    const bool input_ready = keyboard.tap && keyboard.source;

    GameWatcher watcher;
    watcher.game = game;
    CGDisplayRegisterReconfigurationCallback(display_reconfigured, &watcher);
    watcher.thread = std::thread(watch_game, &watcher);

    int champion_index = 0;
    int skin_index = 0;
    char filter[96] = {};
    std::future<std::string> pending;
    int want_w = prefs.width, want_h = prefs.height;  // size the user chose
    int full_w = want_w, full_h = want_h;             // size in use (fits the game area)
    bool minimized = false;
    bool user_positioned = prefs.positioned;
    double gap_right = prefs.gap_right, gap_top = prefs.gap_top;
    bool prefs_dirty = false;
    Clock::time_point last_pref_change{};
    bool shown = false;                // overlay window is on screen
    bool initial_verification = false; // invisible and not taking input until it is confirmed in place
    bool verifying = false;            // comparing our position with WindowServer's
    CGPoint target = CGPointMake(-1e9, -1e9);  // global top-left we last asked for
    Clock::time_point placed_at{}, last_render{}, last_repair{};
    Clock::time_point last_sample_time{};  // WindowServer samples of our own window
    CGPoint last_sample_origin = CGPointZero;
    int stable_samples = 0, nudges = 0;
    bool ws_measured = false;
    bool ws_unreliable = false;     // WindowServer's position did not follow our moves
    bool awaiting_response = false; // a correction was made; check that the measurement moved
    CGPoint pre_nudge_origin = CGPointZero;
    double nudge_dx = 0, nudge_dy = 0;
    std::string last_state;
    int frames_left = 0;
    double drag_start_x = 0, drag_start_y = 0;
    int drag_mouse_x = 0, drag_mouse_y = 0;
    int resize_start_w = 0, resize_start_h = 0, resize_mouse_x = 0, resize_mouse_y = 0;
    std::string message = !profile_ready
        ? "Offset scan failed. See terminal; live switch is disabled."
        : !input_ready
            ? "Input capture unavailable. Allow Accessibility/Input Monitoring for Stallion."
            : "Choose your current champion and a skin.";

    auto overlay_size = [&] {
        if (!minimized) return CGSizeMake(full_w, full_h);
        const float scale = overlay_scale(full_w);
        return CGSizeMake(std::lround(240 * scale), std::lround(44 * scale));
    };
    auto clamp_into = [&](const CGRect& area, double x, double y) {
        const CGSize size = overlay_size();
        const double max_x = std::max(area.origin.x, area.origin.x + area.size.width - size.width);
        const double max_y = std::max(area.origin.y, area.origin.y + area.size.height - size.height);
        return CGPointMake(std::round(std::clamp(x, area.origin.x, max_x)),
                           std::round(std::clamp(y, area.origin.y, max_y)));
    };
    auto restart_verification = [&](Clock::time_point now) {
        placed_at = now;
        verifying = true;
        nudges = 0;
        stable_samples = 0;
        last_sample_time = {};
    };
    // Keep the overlay where the user left it relative to the game area's top-right
    // corner, follow the game window, and confirm the result against WindowServer.
    auto sync_position = [&](const GameSnapshot& snap, Clock::time_point now) {
        const CGRect& area = snap.rect;
        // A size chosen in a bigger game window must not spill out of a smaller one.
        const int max_w = static_cast<int>(std::clamp<double>(area.size.width - 16, kMinWidth, kMaxWidth));
        const int max_h = static_cast<int>(std::clamp<double>(area.size.height - 16, kMinHeight, kMaxHeight));
        const int fit_w = std::min(want_w, max_w);
        const int fit_h = std::clamp(want_h, std::min(overlay_min_height(fit_w), max_h), max_h);
        if (fit_w != full_w || fit_h != full_h) {
            full_w = fit_w;
            full_h = fit_h;
            if (!minimized) glfwSetWindowSize(window, full_w, full_h);
        }
        const CGSize size = overlay_size();
        const double right = user_positioned ? gap_right : 40.0;
        const double top = user_positioned
            ? gap_top : std::min(90.0, std::max(0.0, area.size.height - size.height));
        const CGPoint want = clamp_into(area, area.origin.x + area.size.width - size.width - right,
                                        area.origin.y + top);
        if (want.x != target.x || want.y != target.y) {
            target = want;
            place_window(native, target);
            awaiting_response = false;
            restart_verification(now);
        }
        // Track WindowServer's view of our window. A sample is only trusted once two in a
        // row agree and it was taken well after our last move: under load the description
        // lags, and acting on a stale one would move a correctly placed window away.
        const bool sized = snap.overlay_valid &&
            std::abs(snap.overlay.size.width - size.width) < 0.5 &&
            std::abs(snap.overlay.size.height - size.height) < 0.5;
        if (sized && snap.overlay_time > placed_at + 250ms && snap.overlay_time != last_sample_time) {
            const bool same = last_sample_time != Clock::time_point{} &&
                std::abs(snap.overlay.origin.x - last_sample_origin.x) <= 0.5 &&
                std::abs(snap.overlay.origin.y - last_sample_origin.y) <= 0.5;
            stable_samples = same ? stable_samples + 1 : 1;
            last_sample_time = snap.overlay_time;
            last_sample_origin = snap.overlay.origin;
        }
        ws_measured = !ws_unreliable && stable_samples >= 2 && last_sample_time != Clock::time_point{} &&
                      now - last_sample_time < 400ms;
        // A correction must show up in the measurement. If it does not, the measurement
        // is not describing this window: put the window back and stop using it.
        if (ws_measured && awaiting_response) {
            awaiting_response = false;
            const double moved = std::hypot(last_sample_origin.x - pre_nudge_origin.x,
                                            last_sample_origin.y - pre_nudge_origin.y);
            if (moved < 0.5 * std::hypot(nudge_dx, nudge_dy)) {
                ws_unreliable = true;
                ws_measured = false;
                nudge_window(native, -nudge_dx, -nudge_dy);
                verifying = false;
            }
        }
        if (verifying) {
            if (ws_measured) {
                const double dx = target.x - last_sample_origin.x;
                const double dy = target.y - last_sample_origin.y;
                if (std::abs(dx) <= 4 && std::abs(dy) <= 4) {
                    verifying = false;
                } else if (nudges < 3 && std::abs(dx) <= 900 && std::abs(dy) <= 900) {
                    pre_nudge_origin = last_sample_origin;
                    nudge_dx = dx;
                    nudge_dy = dy;
                    awaiting_response = std::hypot(dx, dy) >= 20;
                    nudge_window(native, dx, dy);
                    ++nudges;
                    placed_at = now;
                    stable_samples = 0;
                    last_sample_time = {};
                } else {
                    verifying = false;
                }
            } else if (now - placed_at > 2500ms) {
                verifying = false;
            }
        } else if (ws_measured && now - last_repair > 2s &&
                   (std::abs(target.x - last_sample_origin.x) > 8 ||
                    std::abs(target.y - last_sample_origin.y) > 8)) {
            last_repair = now;
            place_window(native, target);
            restart_verification(now);
        }
        // However verification ended (confirmed, gave up, or the measurement was found
        // useless) the overlay must become visible and start taking input.
        if (!verifying && initial_verification) {
            initial_verification = false;
            [native setAlphaValue:1.0];
            frames_left = std::max(frames_left, 3);
        }
        // What the event tap needs to turn a cursor position into overlay coordinates.
        OverlayGeometry geometry;
        const NSRect frame = [native frame];
        geometry.valid = true;
        geometry.target_x = target.x;
        geometry.target_y = target.y;
        geometry.width = frame.size.width;
        geometry.height = frame.size.height;
        geometry.measured = ws_measured;
        geometry.ws_x = last_sample_origin.x;
        geometry.ws_y = last_sample_origin.y;
        { std::lock_guard<std::mutex> lock(keyboard.geometry_mutex); keyboard.geometry = geometry; }
    };
    // Changing the size keeps the top-left corner where it is.
    auto set_size = [&](int width, int height, const GameSnapshot& snap, Clock::time_point now) {
        const CGRect& area = snap.rect;
        const double left = target.x - area.origin.x;
        const double top = target.y - area.origin.y;
        full_w = want_w = width;
        full_h = want_h = height;
        const CGSize size = overlay_size();
        user_positioned = true;
        gap_top = top;
        gap_right = area.size.width - left - size.width;
        glfwSetWindowSize(window, static_cast<int>(size.width), static_cast<int>(size.height));
        sync_position(snap, now);
        prefs_dirty = true;
        last_pref_change = now;
    };
    auto toggle_minimized = [&](const GameSnapshot& snap, Clock::time_point now) {
        const CGRect& area = snap.rect;
        const double left = target.x - area.origin.x;
        const double top = target.y - area.origin.y;
        minimized = !minimized;
        const CGSize size = overlay_size();
        user_positioned = true;
        gap_top = top;
        gap_right = area.size.width - left - size.width;
        glfwSetWindowSize(window, static_cast<int>(size.width), static_cast<int>(size.height));
        sync_position(snap, now);
        prefs_dirty = true;
        last_pref_change = now;
    };

    while (!glfwWindowShouldClose(window)) {
      @autoreleasepool {
        double timeout = 0.25;  // hidden: just keep watching for the game
        if (shown) timeout = verifying ? 0.02 : keyboard.text_active.load() ? 0.1 : 1.0;
        if (pending.valid() || catalog_task.valid()) timeout = std::min(timeout, 0.1);
        if (frames_left > 0) glfwPollEvents(); else glfwWaitEventsTimeout(timeout);
        Clock::time_point now = Clock::now();
        const GameSnapshot snap = watcher.get();
        keyboard.game_pid.store(snap.pid);
        const bool should_show = snap.foreground && snap.stable;
        if (should_show != shown) {
            if (should_show) {
                watcher.overlay_id.store(static_cast<CGWindowID>([native windowNumber]));
                target = CGPointMake(-1e9, -1e9);
                // Stay invisible until the window is confirmed in place, so a misplacement
                // is corrected without ever being seen.
                initial_verification = true;
                [native setAlphaValue:0.0];
                sync_position(snap, now);
                glfwShowWindow(window);
                [native orderFrontRegardless];
                ImGui::GetIO().AddFocusEvent(true);
                watcher.overlay_shown.store(true);
                frames_left = 3;
            } else {
                keyboard.text_active.store(false);
                keyboard.mouse_captured.store(false);
                keyboard.mouse_inside.store(false);
                { std::lock_guard<std::mutex> lock(keyboard.mutex);
                  keyboard.pending.clear(); keyboard.mouse_pending.clear(); keyboard.wheel = 0; }
                { std::lock_guard<std::mutex> lock(keyboard.geometry_mutex);
                  keyboard.geometry.valid = false; }
                ImGui::GetIO().AddMouseButtonEvent(0, false);
                ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                ImGui::GetIO().AddFocusEvent(false);
                glfwHideWindow(window);
                watcher.overlay_shown.store(false);
                initial_verification = false;
                verifying = false;
                frames_left = 0;
            }
            shown = should_show;
        }
        if (shown) sync_position(snap, now);
        keyboard.overlay_visible.store(shown && !initial_verification);
        if (catalog_task.valid() &&
            catalog_task.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            catalog_ready = catalog_task.get();
            if (!catalog_ready) message = "Skin list refresh failed";
            if (catalog_ready) {
                auto refreshed = load_skins(directory / "skin_ids.json");
                if (!refreshed.empty()) {
                    champions = std::move(refreshed);
                    champion_index = std::min(champion_index, static_cast<int>(champions.size() - 1));
                    skin_index = 0;
                } else catalog_ready = false;
            }
            frames_left = 3;
        }
        if (pending.valid() && pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            message = pending.get();
            frames_left = 3;
        }
        if (prefs_dirty && !keyboard.mouse_captured.load() && !minimized &&
            now - last_pref_change > 600ms) {
            prefs.width = want_w;
            prefs.height = want_h;
            prefs.positioned = user_positioned;
            prefs.gap_right = gap_right;
            prefs.gap_top = gap_top;
            save_prefs(prefs_path, prefs);
            prefs_dirty = false;
        }
        if (!shown) { frames_left = 0; continue; }
        // Draw only when something changed; every present makes WindowServer
        // recomposite the game. One idle frame per second heals a lost surface.
        if (keyboard.wake_pending.load()) frames_left = std::max(frames_left, 3);
        if (frames_left == 0 && now - last_render >= 1s) frames_left = 1;
        if (frames_left == 0 && keyboard.text_active.load() && now - last_render >= 200ms)
            frames_left = 1;  // text caret blink
        if (frames_left == 0) continue;
        if (now - last_render < 16ms) std::this_thread::sleep_until(last_render + 16ms);
        // Everything is laid out in units of one scale factor derived from the width.
        const float scale = overlay_scale(full_w);
        const int wanted_px = std::clamp(static_cast<int>(std::lround(13 * scale)), 10, 34);
        if (wanted_px != font_px) { rebuild_font(wanted_px); font_px = wanted_px; }
        ImGuiStyle& style = ImGui::GetStyle();
        style = base_style;
        style.ScaleAllSizes(scale);
        style.MouseCursorScale = scale;
        // The game hides the system cursor and its own sprite is under the overlay, so
        // draw one where the click will land.
        ImGui::GetIO().MouseDrawCursor = prefs.cursor && keyboard.mouse_inside.load();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        forward_captured_keys(keyboard);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Stallion77 Skin Changer", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);
        const float header_height = 24 * scale;
        const float button_width = 30 * scale;
        ImVec2 drag_origin = ImGui::GetCursorScreenPos();
        const float drag_width = minimized
            ? ImGui::GetContentRegionAvail().x
            : ImGui::GetContentRegionAvail().x - 2 * button_width - 2 * style.ItemSpacing.x;
        ImGui::InvisibleButton("##drag", ImVec2(drag_width, header_height));
        bool restore_from_minimized = false;
        if (ImGui::IsItemActivated()) {
            drag_start_x = target.x;
            drag_start_y = target.y;
            drag_mouse_x = keyboard.global_mouse_x.load();
            drag_mouse_y = keyboard.global_mouse_y.load();
        }
        if (ImGui::IsItemActive() && keyboard.mouse_captured.load()) {
            const CGPoint moved = clamp_into(snap.rect,
                drag_start_x + keyboard.global_mouse_x.load() - drag_mouse_x,
                drag_start_y + keyboard.global_mouse_y.load() - drag_mouse_y);
            if (moved.x != target.x || moved.y != target.y) {
                const CGSize size = overlay_size();
                user_positioned = true;
                gap_right = snap.rect.origin.x + snap.rect.size.width - (moved.x + size.width);
                gap_top = moved.y - snap.rect.origin.y;
                sync_position(snap, now);
                prefs_dirty = true;
                last_pref_change = now;
            }
        }
        if (minimized && ImGui::IsItemDeactivated()) {
            int distance_x = std::abs(keyboard.global_mouse_x.load() - drag_mouse_x);
            int distance_y = std::abs(keyboard.global_mouse_y.load() - drag_mouse_y);
            restore_from_minimized = distance_x <= 4 && distance_y <= 4;
        }
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(drag_origin.x + 6 * scale, drag_origin.y + (header_height - ImGui::GetFontSize()) * 0.5f),
            IM_COL32(245, 245, 245, 255), "Stallion77 Skin Changer");
        bool flip_minimized = restore_from_minimized;
        if (!minimized) {
            ImGui::SameLine();
            if (ImGui::Button("_", ImVec2(button_width, header_height))) flip_minimized = true;
            ImGui::SameLine();
            if (ImGui::Button("X", ImVec2(button_width, header_height)))
                glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        if (flip_minimized) {
            toggle_minimized(snap, now);
            keyboard.text_active.store(false);
        }
        if (!minimized) {
        ImGui::Text("Game: %s | Offsets: %s | Skins: %s",
            snap.foreground ? "READY" : "WAITING FOR A MATCH",
            profile_ready ? "OK" : "ERROR",
            catalog_task.valid() ? "CHECKING" : (catalog_ready ? "OK" : "OUTDATED"));
        {
            const float refresh_w = ImGui::CalcTextSize("Refresh").x + 2 * style.FramePadding.x;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - refresh_w -
                                                                 style.WindowPadding.x));
            if (catalog_task.valid()) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Refresh"))
                catalog_task = std::async(std::launch::async,
                    [directory, game] { return update_catalog(directory, game); });
            if (catalog_task.valid()) ImGui::EndDisabled();
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##search", "Search champion", filter, sizeof filter);
        keyboard.text_active.store(shown && ImGui::IsItemActive() && input_ready);
        // Both lists share the height that is left after the fixed parts, so the layout
        // follows the window size instead of using fixed heights.
        const float spacing = style.ItemSpacing.y;
        const float row = ImGui::GetTextLineHeightWithSpacing();
        const float button_height = 32 * scale;
        const float message_height = ImGui::CalcTextSize(message.c_str(), nullptr, false,
            ImGui::GetContentRegionAvail().x).y;
        const float fixed = ImGui::GetTextLineHeight() + button_height + message_height +
                            5 * spacing + 10 * scale;
        const float lists = std::max(6 * row, ImGui::GetContentRegionAvail().y - fixed);
        const float champion_height = std::max(3 * row, std::floor(lists * 0.55f));
        const float skin_height = std::max(3 * row, lists - champion_height);
        if (ImGui::BeginListBox("##champions", ImVec2(-FLT_MIN, champion_height))) {
            std::string needle = filter;
            std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
            for (size_t i = 0; i < champions.size(); ++i) {
                std::string name = champions[i].name;
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                if (name.find(needle) == std::string::npos) continue;
                if (ImGui::Selectable(champions[i].name.c_str(), champion_index == static_cast<int>(i))) {
                    champion_index = static_cast<int>(i);
                    skin_index = 0;
                }
            }
            ImGui::EndListBox();
        }
        auto& champion = champions[champion_index];
        ImGui::Text("%s", champion.name.c_str());
        if (ImGui::BeginListBox("##skins", ImVec2(-FLT_MIN, skin_height))) {
            for (size_t i = 0; i < champion.skins.size(); ++i) {
                const auto& skin = champion.skins[i];
                std::string label = skin.name + " (" + std::to_string(skin.number) + ")";
                if (ImGui::Selectable(label.c_str(), skin_index == static_cast<int>(i)))
                    skin_index = static_cast<int>(i);
            }
            ImGui::EndListBox();
        }
        bool busy = pending.valid() || !profile_ready || !catalog_ready ||
                    !input_ready || !snap.foreground;
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button("Switch in game", ImVec2(-FLT_MIN, button_height))) {
            std::string name = champion.name;
            unsigned number = champion.skins[skin_index].number;
            message = "Switching...";
            pid_t target_pid = snap.pid;
            pending = std::async(std::launch::async, [tool, target_pid, name, number] {
                return run_command(tool, "liveskin", target_pid, name, number);
            });
        }
        if (busy) ImGui::EndDisabled();
        ImGui::TextWrapped("%s", message.c_str());
        // Resize grip in the bottom-right corner; dragging it resizes the overlay.
        const float grip = 16 * scale;
        const ImVec2 window_size = ImGui::GetWindowSize();
        ImGui::SetCursorPos(ImVec2(window_size.x - grip, window_size.y - grip));
        ImGui::InvisibleButton("##resize", ImVec2(grip, grip));
        const bool grip_active = ImGui::IsItemActive();
        const bool grip_hot = grip_active || ImGui::IsItemHovered();
        if (grip_hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        const ImU32 grip_color = grip_hot ? IM_COL32(230, 230, 230, 255) : IM_COL32(150, 150, 150, 255);
        for (int i = 1; i <= 3; ++i)
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(window_size.x - 3 * scale - i * 4 * scale, window_size.y - 3 * scale),
                ImVec2(window_size.x - 3 * scale, window_size.y - 3 * scale - i * 4 * scale),
                grip_color, std::max(1.0f, scale));
        if (ImGui::IsItemActivated()) {
            resize_start_w = full_w;
            resize_start_h = full_h;
            resize_mouse_x = keyboard.global_mouse_x.load();
            resize_mouse_y = keyboard.global_mouse_y.load();
        }
        if (grip_active && keyboard.mouse_captured.load()) {
            const CGRect& area = snap.rect;
            const int max_w = static_cast<int>(std::clamp<double>(area.size.width - 16, kMinWidth, kMaxWidth));
            int width = std::clamp(resize_start_w + keyboard.global_mouse_x.load() - resize_mouse_x,
                                   kMinWidth, max_w);
            const int max_h = static_cast<int>(std::clamp<double>(area.size.height - 16, kMinHeight, kMaxHeight));
            int height = std::clamp(resize_start_h + keyboard.global_mouse_y.load() - resize_mouse_y,
                                    std::min(overlay_min_height(width), max_h), max_h);
            if (width != full_w || height != full_h) set_size(width, height, snap, now);
        }
        }
        ImGui::End();
        ImGui::Render();
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
        last_render = Clock::now();
        --frames_left;
        // A click or keystroke changes what the next frame shows; make sure it is drawn.
        const std::string state = std::to_string(champion_index) + "/" + std::to_string(skin_index) +
            (minimized ? "/m/" : "/f/") + std::to_string(full_w) + "x" + std::to_string(full_h) + "/" +
            filter + "/" + message + (pending.valid() ? "/p" : "/i");
        if (state != last_state) { last_state = state; frames_left = std::max(frames_left, 2); }
      }
    }
    glfwHideWindow(window);
    if (pending.valid()) pending.wait();
    if (catalog_task.valid()) catalog_task.wait();
    if (prefs_dirty && !minimized) {
        prefs.width = want_w;
        prefs.height = want_h;
        prefs.positioned = user_positioned;
        prefs.gap_right = gap_right;
        prefs.gap_top = gap_top;
        save_prefs(prefs_path, prefs);
    }
    keyboard.text_active.store(false);
    keyboard.overlay_visible.store(false);
    watcher.stop.store(true);
    if (watcher.thread.joinable()) watcher.thread.join();
    CGDisplayRemoveReconfigurationCallback(display_reconfigured, &watcher);
    if (keyboard.runloop) CFRunLoopStop(keyboard.runloop);
    if (tap_thread.joinable()) tap_thread.join();
    if (keyboard.runloop) CFRelease(keyboard.runloop);
    if (keyboard.source) {
        CFRelease(keyboard.source);
    }
    if (keyboard.tap) CFRelease(keyboard.tap);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
