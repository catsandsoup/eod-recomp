#import <Cocoa/Cocoa.h>
#include "macos_app.h"
#include "../core/config.h"
#include <filesystem>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cmath>

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

bool mac_app_set_mod_profile(const std::string &profile) {
    if (profile == "native") { unsetenv("EOD_MOD_DIR"); return true; }
    if (profile != "phantom" && profile != "battle" && profile != "combined") return false;
    fs::path root = fs::path(mac_app_support()) / "mods" / profile;
    if (!fs::exists(root)) {
        NSString *resources = [NSBundle mainBundle].resourcePath;
        if (resources) root = fs::path(resources.UTF8String) / "Mods" / profile;
    }
    std::vector<std::string> required = {"RES/COMMON.RES"};
    if (profile != "battle") {
        required.insert(required.end(), {"CAR/PHANTOM.CAR", "RES/CT.RES", "RES/UICAR.RES"});
    }
    if (profile != "phantom") required.push_back("TRACKS/WELCH.TRK");
    for (const auto &file : required) {
        std::error_code ec;
        const auto path = root / "DATA" / file;
        if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) == 0 || ec) return false;
    }
    return setenv("EOD_MOD_DIR", root.c_str(), 1) == 0;
}

bool mac_app_select_mods(bool headless) {
    // Automation and an explicit developer overlay never open or override a chooser.
    if (headless || getenv("EOD_MOD_DIR")) return true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    const char *profiles[] = {"native", "phantom", "battle", "combined"};
    NSArray<NSString *> *titles = @[@"Native game", @"Phantom custom car", @"All-map Battle catalog", @"Phantom + all-map Battle"];
    NSAlert *a = [NSAlert new];
    a.messageText = @"Choose your mods";
    a.informativeText = @"Choose a profile for this session. Press F1 during play for chickens, turbo, invincibility, recovery and other settings. F2 shows controls.\n\nBattle profiles keep Career but replace Action with race + Battle, and multiplayer with Battle. Battle supports four cars total. Change resource profiles by restarting the app.";
    [a addButtonWithTitle:@"Play"]; [a addButtonWithTitle:@"Quit"];
    NSPopUpButton *choice = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0,0,360,30) pullsDown:NO];
    [choice addItemsWithTitles:titles];
    NSString *saved = [[NSUserDefaults standardUserDefaults] stringForKey:@"EODModProfile"];
    for (int i=0;i<4;i++) if ([saved isEqualToString:ns(profiles[i])]) [choice selectItemAtIndex:i];
    a.accessoryView = choice;
    while ([a runModal] == NSAlertFirstButtonReturn) {
        const char *profile = profiles[choice.indexOfSelectedItem];
        if (mac_app_set_mod_profile(profile)) {
            [[NSUserDefaults standardUserDefaults] setObject:ns(profile) forKey:@"EODModProfile"];
            fprintf(stderr, "Mod profile: %s\n", profile);
            return true;
        }
        error("This mod profile is missing or incomplete. Install the local mod profiles in Application Support/TDEODRecomp/mods, or choose Native game.");
    }
    return false;
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
    NSAlert *a = [NSAlert new]; a.messageText = @"Settings";
    a.informativeText = @"Press F2 during play for Controls and player 2 help, including current device assignments. Window size, fullscreen, smoothing, volume, key bindings, cheats, multipliers, damage scaling and manual recovery apply now. Aspect ratio, camera projection and other changes apply on the next launch. PS2 key fields below apply only to the Original keyboard preset. Multiple keys may be separated by commas.";
    [a addButtonWithTitle:@"Save"]; [a addButtonWithTitle:@"Cancel"];
    struct Field { std::string label; std::string value; };
    std::vector<Field> fields = {
        {"Renderer",g_config.renderer}, {"Internal resolution (0 auto, 1–6)",std::to_string(g_config.render_scale)},
        {"Window width (points)",std::to_string(g_config.width)}, {"Window height (points)",std::to_string(g_config.height)},
        {"Fullscreen",std::to_string(g_config.fullscreen)}, {"VSync",std::to_string(g_config.vsync)},
        {"Aspect (next launch)",g_config.aspect}, {"Smooth display",std::to_string(g_config.smooth_display)},
        {"Frame skip",std::to_string(g_config.frame_skip)}, {"Audio enabled",std::to_string(g_config.audio)},
        {"Volume (0–100)",std::to_string(g_config.volume)}, {"Keyboard port (-1 auto, 0 off, 1, 2)",std::to_string(g_config.keyboard_port)},
        {"Vibration",std::to_string(g_config.vibration)}, {"Memory card folder",g_config.memcard_dir}, {"Disc folder",g_config.data_dir},
        {"Controller layout",g_config.controller_layout}, {"Keyboard layout",g_config.keyboard_layout}, {"Detail (next launch)",g_config.detail},
        {"Manual recovery only",std::to_string(g_config.manual_recovery)},
        {"Chicken fight (L2 fires chickens)",std::to_string(g_config.cheat_chickens)},
        {"Turbo boost (L2 boosts)",std::to_string(g_config.cheat_turbo)},
        {"Single player battle mode",std::to_string(g_config.sp_battle_mode)},
        {"Unlock all out of box",std::to_string(g_config.unlock_all)},
        {"Battle bots (next event)",std::to_string(g_config.battle_bots)},
        {"Invincibility (all humans)",std::to_string(g_config.cheat_invincible)},
        {"Allow cheats in Career",std::to_string(g_config.career_cheats)},
        {"Per-car damage scaling",std::to_string(g_config.car_durability)},
        {"Turbo multiplier (0.1–100)",std::to_string(g_config.turbo_scale)},
        {"Chicken force (0.1–100)",std::to_string(g_config.chicken_force_scale)},
        {"Chicken radius (0.1–100)",std::to_string(g_config.chicken_radius_scale)}
    };
    const int key_start = static_cast<int>(fields.size());
    for (int i=0;i<KEY_COUNT;i++) fields.push_back({kPadKeyLabels[i],g_config.keys[i]});
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,560,430)]; scroll.hasVerticalScroller = YES;
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0,0,540,fields.size()*32)];
    NSMutableArray<NSControl *> *inputs = [NSMutableArray new];
    for (size_t i=0;i<fields.size();i++) {
        CGFloat y = (fields.size()-i-1)*32;
        NSTextField *label=[NSTextField labelWithString:ns(fields[i].label)]; label.frame=NSMakeRect(0,y+5,215,22); [view addSubview:label];
        NSControl *input;
        if (i==4 || i==5 || i==7 || i==8 || i==9 || i==12 || (i>=18 && i<=26)) {
            NSButton *check=[NSButton checkboxWithTitle:@"Enabled" target:nil action:nil];
            check.state=fields[i].value=="1" ? NSControlStateValueOn : NSControlStateValueOff;
            input=check;
        } else if (i==0 || i==6 || i==15 || i==16 || i==17) {
            NSPopUpButton *popup=[[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
            if (i==17) {
                [popup addItemsWithTitles:@[@"Original",@"High",@"Maximum"]];
                [popup selectItemAtIndex:fields[i].value=="maximum" ? 2 : fields[i].value=="high" ? 1 : 0];
            } else if (i==16) {
                [popup addItemsWithTitles:@[@"Original (PS2 keys)",@"Racing (WASD + arrows)"]];
                [popup selectItemAtIndex:fields[i].value=="racing" ? 1 : 0];
            } else if (i==15) {
                [popup addItemsWithTitles:@[@"Original (PS2)",@"Modern (analog triggers)"]];
                [popup selectItemAtIndex:fields[i].value=="modern" ? 1 : 0];
            } else {
                [popup addItemsWithTitles:i==0 ? @[@"software",@"vulkan"] : @[@"auto",@"4:3",@"16:9",@"stretch"]];
                [popup selectItemWithTitle:ns(fields[i].value)];
            }
            input=popup;
        } else {
            NSTextField *text=[[NSTextField alloc] initWithFrame:NSZeroRect];
            text.stringValue=ns(fields[i].value); input=text;
        }
        if (i==18) input.toolTip=@"Explore without automatic wrong-way, upside-down or off-track countdown resets. SELECT still recovers your car. Applies when you save; AI recovery is unchanged.";
        if (i==19) input.toolTip=@"Press L2 to fire a chicken; release before firing again. Also toggle live with L1+R1+D-Pad Up or L3+R3.";
        if (i==23) input.toolTip=@"Experimental AI opponents with native weapons and respawns; navigation can stall at obstacles. Applies when starting the next event.";
        if (i==20) input.toolTip=@"Enables supercharged turbo boost on holding L2. Also toggle live with L1+R1+D-Pad Right.";
        if (i==24) input.toolTip=@"Protects all human players (P1–P4). AI still take damage. Existing damage remains. Applies when saved.";
        input.frame=NSMakeRect(220,y,310,26); [view addSubview:input]; [inputs addObject:input];
    }
    scroll.documentView=view; [view scrollPoint:NSMakePoint(0,view.frame.size.height)]; a.accessoryView=scroll;
    if ([a runModal]!=NSAlertFirstButtonReturn) return false;
    Config c=g_config;
    auto value=[&](int i)->std::string {
        NSControl *input=inputs[i];
        if (i==17) { NSInteger n=((NSPopUpButton *)input).indexOfSelectedItem; return n==2 ? "maximum" : n==1 ? "high" : "original"; }
        if (i==16) return ((NSPopUpButton *)input).indexOfSelectedItem==1 ? "racing" : "original";
        if (i==15) return ((NSPopUpButton *)input).indexOfSelectedItem==1 ? "modern" : "original";
        if ([input isKindOfClass:[NSPopUpButton class]]) return std::string(((NSPopUpButton *)input).titleOfSelectedItem.UTF8String);
        if ([input isKindOfClass:[NSButton class]]) return ((NSButton *)input).state==NSControlStateValueOn ? "1" : "0";
        return std::string(input.stringValue.UTF8String);
    };
    auto number=[&](int i,int lo,int hi){return std::max(lo,std::min(hi,atoi(value(i).c_str())));};
    c.renderer=value(0); c.render_scale=number(1,0,6); c.width=number(2,320,16384); c.height=number(3,240,16384);
    c.fullscreen=number(4,0,1); c.vsync=number(5,0,1); c.aspect=value(6);
    c.smooth_display=number(7,0,1); c.frame_skip=number(8,0,1); c.audio=number(9,0,1); c.volume=number(10,0,100);
    c.keyboard_port=number(11,-1,2); c.vibration=number(12,0,1); c.memcard_dir=value(13); c.data_dir=value(14);
    c.controller_layout=value(15);c.keyboard_layout=value(16);
    c.detail=value(17);
    c.manual_recovery=number(18,0,1);
    c.cheat_chickens=number(19,0,1);
    c.cheat_turbo=number(20,0,1);
    c.sp_battle_mode=number(21,0,1);
    c.unlock_all=number(22,0,1);
    c.battle_bots=number(23,0,1);
    c.cheat_invincible=number(24,0,1);
    c.career_cheats=number(25,0,1);
    c.car_durability=number(26,0,1);
    float *scales[] = {&c.turbo_scale, &c.chicken_force_scale, &c.chicken_radius_scale};
    for (int i=0;i<3;i++) {
        const auto text = value(27+i);
        char *end = nullptr;
        float n = strtof(text.c_str(), &end);
        if (!std::isfinite(n) || end == text.c_str() || *end || n < 0.1f || n > 100.f) {
            error("Multipliers must be numbers between 0.1 and 100."); return false;
        }
        *scales[i] = n;
    }
    for(int i=0;i<KEY_COUNT;i++) c.keys[i]=value(key_start+i);
    if (c.aspect!="auto" && c.aspect!="4:3" && c.aspect!="16:9" && c.aspect!="stretch") { error("Aspect must be auto (the game's Widescreen option), 4:3, 16:9 or stretch."); return false; }
    if (c.renderer!="software" && c.renderer!="vulkan") { error("Renderer must be software (or vulkan in a build with Vulkan support)."); return false; }
    if (!config_save(config_path,c)) { error("Could not save settings. Check the settings folder permissions."); return false; }
    g_config=c; return true;
}
