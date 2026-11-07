#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <SDL3/SDL.h>
#include "controller_panel.h"
#include "controller_navigation.h"
#include "../platform/sdl3/sdl_input.h"
#include <algorithm>
#include <memory>
#include <utility>

static NSString*ui_string(const std::string&s){return [NSString stringWithUTF8String:s.c_str()]?:@"";}
static const char*ui_button(SDL_GamepadButtonLabel b){
    switch(b){case SDL_GAMEPAD_BUTTON_LABEL_A:return "A";case SDL_GAMEPAD_BUTTON_LABEL_B:return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS:return "Cross";case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_X:return "X";case SDL_GAMEPAD_BUTTON_LABEL_Y:return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:return "Square";case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:return "Triangle";
        default:return "South/East";}
}
@interface EODControllerPanel:NSObject<NSWindowDelegate>{
@public
    controller_ui::Page page;
    controller_ui::Navigation navigation;
    size_t selection;
    bool modal,polling;
    NSWindow*window;
    NSScrollView*scroll;
    NSTextField*title,*message,*prompts;
    NSMutableArray<NSButton*>*buttons;
    NSTimer*timer;
}
-(void)setPage:(controller_ui::Page)value preserve:(BOOL)preserve;
-(void)tick;
-(void)press:(NSButton*)sender;
@end
static EODControllerPanel*ui_panel;

@implementation EODControllerPanel
-(instancetype)init{
    if(!(self=[super init]))return nil;
    selection=0;modal=false;polling=false;
    window=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,900,690)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    window.title=@"Eve of Destruction";window.delegate=self;window.releasedWhenClosed=NO;
    window.collectionBehavior=NSWindowCollectionBehaviorFullScreenAuxiliary;
    window.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    [window center];
    NSView*content=window.contentView;
    title=[NSTextField labelWithString:@""];title.font=[NSFont boldSystemFontOfSize:28];
    title.frame=NSMakeRect(28,623,844,38);title.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin;[content addSubview:title];
    message=[NSTextField wrappingLabelWithString:@""];message.font=[NSFont systemFontOfSize:15];
    message.frame=NSMakeRect(28,525,844,88);message.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin;[content addSubview:message];
    scroll=[[NSScrollView alloc]initWithFrame:NSMakeRect(28,80,844,435)];
    scroll.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;scroll.hasVerticalScroller=YES;scroll.drawsBackground=NO;
    [content addSubview:scroll];
    prompts=[NSTextField wrappingLabelWithString:@""];prompts.font=[NSFont boldSystemFontOfSize:14];
    prompts.frame=NSMakeRect(28,18,844,52);prompts.autoresizingMask=NSViewWidthSizable|NSViewMaxYMargin;[content addSubview:prompts];
    buttons=[NSMutableArray new];return self;
}
-(BOOL)windowShouldClose:(NSWindow*)sender{
    (void)sender;const auto back=page.back;if(back)back();return NO;
}
-(void)setPage:(controller_ui::Page)value preserve:(BOOL)preserve{
    page=std::move(value);if(!preserve)selection=0;
    title.stringValue=ui_string(page.title);message.stringValue=ui_string(page.message);
    NSView*document=[NSView new];[buttons removeAllObjects];
    const int columns=std::max(1,page.columns);const CGFloat width=scroll.contentSize.width;
    const CGFloat height=std::max(scroll.contentSize.height,CGFloat((page.rows.size()+columns-1)/columns)*54);
    document.frame=NSMakeRect(0,0,width,height);
    for(size_t i=0;i<page.rows.size();++i){
        const auto&row=page.rows[i];NSButton*button=[NSButton buttonWithTitle:ui_string(row.label+(row.value.empty()?"":"   "+row.value))target:self action:@selector(press:)];
        button.tag=NSInteger(i);button.enabled=row.enabled;button.bezelStyle=NSBezelStyleRegularSquare;
        button.font=[NSFont systemFontOfSize:columns>1?19:21 weight:NSFontWeightSemibold];button.wantsLayer=YES;
        button.frame=NSMakeRect(CGFloat(i%columns)*(width/columns)+4,height-CGFloat(i/columns+1)*54+5,width/columns-8,44);
        [document addSubview:button];[buttons addObject:button];
    }
    scroll.documentView=document;
    if(selection>=page.rows.size())selection=0;
    if(!page.rows.empty()&&!page.rows[selection].enabled){
        std::vector<bool>enabled;for(const auto&r:page.rows)enabled.push_back(r.enabled);
        selection=controller_ui::move_selection(selection,1,enabled);
    }
    [self highlight];
}
-(void)highlight{
    for(NSButton*button in buttons){
        const bool selected=size_t(button.tag)==selection;
        button.layer.borderWidth=selected?3:0;
        button.layer.cornerRadius=5;
        button.layer.borderColor=(selected?[NSColor systemOrangeColor]:[NSColor clearColor]).CGColor;
    }
    if(selection<buttons.count)[buttons[selection] scrollRectToVisible:buttons[selection].bounds];
}
-(void)press:(NSButton*)sender{
    const size_t index=size_t(sender.tag);if(index>=page.rows.size()||!page.rows[index].enabled)return;
    selection=index;[self highlight];const auto action=page.rows[index].activate;if(action)action();
}
-(void)tick{
    if(polling)return;polling=true;
    if(modal){
        SDL_PumpEvents();SDL_Event event;
        while(SDL_PollEvent(&event)){
            if(event.type==SDL_EVENT_GAMEPAD_ADDED)sdl_input::on_added(event.gdevice.which);
            else if(event.type==SDL_EVENT_GAMEPAD_REMOVED)sdl_input::on_removed(event.gdevice.which);
        }
    }
    SDL_UpdateGamepads();sdl_input::assign_waiting();
    SDL_Gamepad*pad=sdl_input::port_gamepad(0);
    if(pad){
        const uint32_t device=SDL_GetJoystickID(SDL_GetGamepadJoystick(pad));navigation.set_owner(device);
        HostPadState state;sdl_input::read_menu_gamepad(pad,&state,true);
        const auto event=navigation.poll(SDL_GetTicks(),device,state,[NSApp isActive]&&window.isKeyWindow);
        const char*accept=ui_button(SDL_GetGamepadButtonLabel(pad,SDL_GAMEPAD_BUTTON_SOUTH));
        const char*back=ui_button(SDL_GetGamepadButtonLabel(pad,SDL_GAMEPAD_BUTTON_EAST));
        std::string text=std::string(accept)+" Confirm   "+back+" Back   D-pad / stick Navigate";
        if(page.columns>1)text+="   LB Delete   RB Done";
        if(!navigation.armed())text+="\nRelease buttons to continue.";
        prompts.stringValue=ui_string(text);
        std::vector<bool>enabled;for(const auto&r:page.rows)enabled.push_back(r.enabled);
        int move=event.vertical*std::max(1,page.columns)+(page.columns>1?event.horizontal:0);
        while(move){selection=controller_ui::move_selection(selection,move>0?1:-1,enabled);move+=move>0?-1:1;}
        if(move||event.vertical||event.horizontal)[self highlight];
        std::function<void()>action;
        if(event.back)action=page.back;
        else if(event.left_shoulder)action=page.left_shoulder;
        else if(event.right_shoulder)action=page.right_shoulder;
        else if(selection<page.rows.size()&&page.rows[selection].enabled){
            const auto row=page.rows[selection];
            if(event.accept)action=row.activate;
            else if(page.columns==1&&event.horizontal&&row.adjust)action=[row,d=event.horizontal]{row.adjust(d);};
        }
        if(action){polling=false;action();return;} // Page may replace its own callbacks.

    }else{
        navigation.set_owner(0);prompts.stringValue=@"Connect a controller. The first controller owns these menus; player 2 keeps their assignment.";
    }
    const auto callback=page.tick;if(callback)callback();polling=false;
}
@end
namespace controller_ui{
static bool input_ready(){
    sdl_input::set_hints();sdl_input::set_keyboard_port(-2);
    const auto need=(SDL_INIT_GAMEPAD|SDL_INIT_EVENTS)&~SDL_WasInit(0);
    return !need||SDL_InitSubSystem(need);
}
void open_panel(Page page){
    if(const char*h=getenv("EOD_HEADLESS");h&&h[0]=='1')return;
    [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    if(!ui_panel)ui_panel=[EODControllerPanel new];
    [ui_panel setPage:std::move(page) preserve:NO];[ui_panel->window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
}
int run_panel(Page page){
    if(const char*h=getenv("EOD_HEADLESS");h&&h[0]=='1')return -1;
    if(!input_ready())return -1;
    open_panel(std::move(page));ui_panel->modal=true;
    __weak EODControllerPanel*weak=ui_panel;
    ui_panel->timer=[NSTimer scheduledTimerWithTimeInterval:1.0/60 repeats:YES block:^(NSTimer*){[weak tick];}];
    const auto result=[NSApp runModalForWindow:ui_panel->window];
    [ui_panel->timer invalidate];ui_panel->timer=nil;ui_panel->modal=false;return int(result);
}
void replace_page(Page page,bool preserve){if(ui_panel)[ui_panel setPage:std::move(page) preserve:preserve];}
void close_panel(int result){
    if(!ui_panel)return;
    [ui_panel->window orderOut:nil];if(ui_panel->modal)[NSApp stopModalWithCode:result];
    ui_panel->page=Page{};
}
void panel_tick(){if(ui_panel&&ui_panel->window.visible&&!ui_panel->modal)[ui_panel tick];}
bool panel_visible(){return ui_panel&&ui_panel->window.visible;}
void text_entry(std::string caption,std::string initial,size_t capacity,std::function<void(std::string)>accepted,std::function<void()>cancelled){
    auto edit=std::make_shared<TextEntry>(std::move(initial),capacity);
    auto draw=std::make_shared<std::function<void()>>();std::weak_ptr<std::function<void()>>redraw=draw;
    *draw=[caption=std::move(caption),edit,accepted=std::move(accepted),cancelled=std::move(cancelled),redraw]{
        Page page;page.title=caption;page.message=edit->value();page.columns=10;page.back=cancelled;
        for(char c:TextEntry::characters())page.rows.push_back({c==' '?"Space":std::string(1,c),"",true,[edit,c,redraw]{edit->append(c);if(auto f=redraw.lock())(*f)();},{}});
        page.left_shoulder=[edit,redraw]{edit->erase();if(auto f=redraw.lock())(*f)();};
        page.right_shoulder=[edit,accepted]{if(accepted)accepted(edit->value());};
        // Keep redraw state alive with this page, without a self-owning cycle.
        page.tick=[keep=redraw.lock()]{};
        replace_page(std::move(page),true);
    };
    (*draw)();
}
}
#endif
