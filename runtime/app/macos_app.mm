// macos_app.mm - macOS app integration (EOD_SDL3 builds on macOS):
// settings and memory cards in ~/Library/Application Support/TDEODRecomp, a
// first-run dialog that installs the player's disc (an ISO/BIN image or a
// folder; disc_import.cpp) there, and a settings dialog (F1 or Cmd+,).
#import <Cocoa/Cocoa.h>
#include "macos_app.h"
#include "../core/config.h"
#include <filesystem>
#include <cstdlib>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;
static std::string config_path;
static NSString *ns(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()]; }
static void error(const std::string &s) {
    NSAlert *a = [NSAlert new]; a.messageText = @"Eve of Destruction"; a.informativeText = ns(s); [a runModal];
}
std::string mac_app_support() {
    if (const char *p = getenv("EOD_APP_SUPPORT")) return p;
    NSString *p = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
    return std::string(p.UTF8String) + "/TDEODRecomp";
}

bool mac_app_prepare(std::string &data, const std::string &config, bool headless, const DiscId &id) {
    config_path = config;
    const std::string support = mac_app_support();
    std::error_code ec;
    fs::create_directories(support + "/memcard", ec);
    if (ec) { fprintf(stderr, "Application Support: %s\n", ec.message().c_str()); return false; }
    if (g_config.memcard_dir.empty()) g_config.memcard_dir = support + "/memcard";
    if (const char *p = getenv("EOD_DATA")) if (data.empty()) data = p;
    if (!data.empty()) {
        // Validate before main's fatal-error path: platform_init has not yet
        // established whether SDL dialogs are allowed for this launch.
        DiscResult r = disc_check_folder(data, id);
        if (r.ok()) return true;
        if (headless) fprintf(stderr, "%s\n", r.message.c_str());
        else {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp activateIgnoringOtherApps:YES];
            error(r.message);
        }
        return false;
    }
    data = support + "/disc";
    if (disc_check_folder(data, id).ok()) return true;
    if (headless) { fprintf(stderr, "No valid installed disc. Pass --data DIR or EOD_DATA; headless mode never opens dialogs.\n"); return false; }
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    for (;;) {
        NSOpenPanel *picker = [NSOpenPanel openPanel];
        picker.title = @"Choose your Eve of Destruction disc";
        picker.message = @"Choose your North American v1.00 ISO, or the folder containing SLUS_209.10, SYSTEM.CNF and DATA. Your original files stay untouched.";
        picker.canChooseFiles = YES; picker.canChooseDirectories = YES; picker.allowsMultipleSelection = NO;
        if ([picker runModal] != NSModalResponseOK) return false;
        NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,470,110) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        panel.title = @"Installing your disc";
        NSTextField *label = [NSTextField labelWithString:@"Checking game version…"];
        label.frame = NSMakeRect(20,60,430,30); [panel.contentView addSubview:label];
        NSProgressIndicator *bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20,25,430,20)];
        bar.indeterminate = NO; bar.minValue = 0; bar.maxValue = 100; [panel.contentView addSubview:bar];
        [panel center]; [panel makeKeyAndOrderFront:nil];
        DiscResult r = disc_import(picker.URL.path.UTF8String, data, id, [&](uint64_t done, uint64_t total, const std::string &file) {
            bar.doubleValue = total ? 100.0 * done / total : 0;
            label.stringValue = ns("Copying " + file); [panel displayIfNeeded];
            return true;
        });
        [panel orderOut:nil];
        if (r.ok()) break;
        error(r.message);
    }
    g_config.data_dir = data;
    fs::create_directories(fs::path(config).parent_path(), ec);
    if (!config_save(config)) error("Your disc is installed, but settings could not be saved. Check the folder permissions.");
    return true;
}

bool mac_app_settings() {
    [NSApplication sharedApplication];
    NSAlert *a = [NSAlert new];
    a.messageText = @"Settings";
    a.informativeText = @"Window size, fullscreen, smoothing, volume and key bindings apply now; the other settings on "
                         "the next launch. Several keys for one button are separated by commas.";
    [a addButtonWithTitle:@"Save"];
    [a addButtonWithTitle:@"Cancel"];
    enum Kind { Text, Check, Choice };
    struct Field {
        std::string label, value;
        Kind kind;
        std::vector<std::string> choices;
    };
#ifdef EOD_VULKAN
    const std::vector<std::string> renderers = {"vulkan", "software"};
#else
    const std::vector<std::string> renderers = {"software"};
#endif
    std::vector<Field> fields = {
        {"Renderer", g_config.renderer, Choice, renderers},
        {"Internal resolution (1-6)", std::to_string(g_config.render_scale), Text, {}},
        {"Window width (points)", std::to_string(g_config.width), Text, {}},
        {"Window height (points)", std::to_string(g_config.height), Text, {}},
        {"Fullscreen", std::to_string(g_config.fullscreen), Check, {}},
        {"VSync", std::to_string(g_config.vsync), Check, {}},
        {"Aspect ratio", g_config.aspect, Choice, {"auto", "4:3", "16:9", "stretch"}},
        {"Smooth display", std::to_string(g_config.smooth_display), Check, {}},
        {"Frame skip", std::to_string(g_config.frame_skip), Check, {}},
        {"Sound", std::to_string(g_config.audio), Check, {}},
        {"Volume (0-100)", std::to_string(g_config.volume), Text, {}},
        {"Vibration", std::to_string(g_config.vibration), Check, {}},
        {"Memory card folder", g_config.memcard_dir, Text, {}},
        {"Disc folder", g_config.data_dir, Text, {}},
    };
    const size_t first_key = fields.size();
    for (int i = 0; i < KEY_COUNT; i++) fields.push_back({kPadKeyLabels[i], g_config.keys[i], Text, {}});
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 560, 430)];
    scroll.hasVerticalScroller = YES;
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 540, fields.size() * 32)];
    NSMutableArray<NSControl *> *inputs = [NSMutableArray new];
    for (size_t i = 0; i < fields.size(); i++) {
        const Field &f = fields[i];
        CGFloat y = (fields.size() - i - 1) * 32;
        NSTextField *label = [NSTextField labelWithString:ns(f.label)];
        label.frame = NSMakeRect(0, y + 5, 215, 22);
        [view addSubview:label];
        NSControl *input;
        if (f.kind == Check) {
            NSButton *check = [NSButton checkboxWithTitle:@"On" target:nil action:nil];
            check.state = f.value == "1" ? NSControlStateValueOn : NSControlStateValueOff;
            input = check;
        } else if (f.kind == Choice) {
            NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
            for (const std::string &c : f.choices) [popup addItemWithTitle:ns(c)];
            [popup selectItemWithTitle:ns(f.value)];
            input = popup;
        } else {
            NSTextField *text = [[NSTextField alloc] initWithFrame:NSZeroRect];
            text.stringValue = ns(f.value);
            input = text;
        }
        input.frame = NSMakeRect(220, y, 310, 26);
        [view addSubview:input];
        [inputs addObject:input];
    }
    scroll.documentView = view;
    [view scrollPoint:NSMakePoint(0, view.frame.size.height)];
    a.accessoryView = scroll;
    if ([a runModal] != NSAlertFirstButtonReturn) return false;
    auto value = [&](size_t i) -> std::string {
        NSControl *input = inputs[i];
        if ([input isKindOfClass:[NSPopUpButton class]])
            return std::string(((NSPopUpButton *)input).titleOfSelectedItem.UTF8String);
        if ([input isKindOfClass:[NSButton class]]) return ((NSButton *)input).state == NSControlStateValueOn ? "1" : "0";
        return std::string(input.stringValue.UTF8String);
    };
    auto number = [&](size_t i, int lo, int hi) { return std::max(lo, std::min(hi, atoi(value(i).c_str()))); };
    Config c = g_config;
    c.renderer = value(0);
    c.render_scale = number(1, 1, 6);
    c.width = number(2, 320, 16384);
    c.height = number(3, 240, 16384);
    c.fullscreen = number(4, 0, 1);
    c.vsync = number(5, 0, 1);
    c.aspect = value(6);
    c.smooth_display = number(7, 0, 1);
    c.frame_skip = number(8, 0, 1);
    c.audio = number(9, 0, 1);
    c.volume = number(10, 0, 100);
    c.vibration = number(11, 0, 1);
    c.memcard_dir = value(12);
    c.data_dir = value(13);
    for (int i = 0; i < KEY_COUNT; i++) c.keys[i] = value(first_key + i);
    if (!config_save(config_path, c)) {
        error("Could not save the settings. Check the permissions of " + config_path + ".");
        return false;
    }
    g_config = c;
    return true;
}
