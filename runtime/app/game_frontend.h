#pragma once
#include "disc_import.h"
#include "../platform/platform.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace game_frontend {
struct Player {unsigned slot=0;std::string name;bool ready=false,muted=false,deafened=false,speaking=false;uint64_t identity=0;};
struct Option {std::string id,label,value;bool editable=false;std::string help;};   // lobby rows (host edits, guests read)
struct LanGame {std::string name,address;unsigned players=0;};                       // games found on the same network / friends hosting (address = friend id)
struct Snapshot {
    enum class Phase {Offline,Hosting,Connecting,CheckingCompatibility,Lobby,Loading,WaitingForStart,Racing,Results,Disconnected,Error};
    Phase phase=Phase::Offline;
    bool direct_connect=false,internet_invites=false,native_race_sync=false,voice=false,microphone_capture=false;
    bool retryable_error=false;uint64_t generation=0;
    enum class Microphone{Unavailable,NotRequested,Requesting,Granted,Denied,DeviceLost};
    Microphone microphone=Microphone::Unavailable;
    bool can_cancel=false,can_start=false,can_rematch=false,can_rejoin_lobby=false;
    bool is_host=false,ready=false,muted=false,deafened=false,microphone_enabled=false;
    std::string status,error,content_problem,invite,address;
    std::vector<Player>players;
    std::vector<Option>options;
    std::vector<LanGame>lan;
    std::string strip;   // short text shown over the game while players choose ("Waiting for ...")
    std::string track_pic;      // lobby: the track whose picture to show (display name)
    uint32_t car_pic=0;         // lobby: the vehicle number whose portrait to show
    std::string code;    // host: the Garage Code to share (Cmd+C copies it)
    std::string route;   // plain-words state of the internet link ("Internet play ready", ...)
    std::vector<LanGame>crew;   // remembered friends who are hosting right now (one press to join)
};
struct Command {
    enum class Kind {Host,JoinAddress,JoinCode,Ready,Start,EndEvent,Cancel,Disconnect,Rematch,RejoinLobby,
        MicrophoneEnabled,Muted,Deafened,VoiceVolume,Option,JoinCrew,Nickname,Kick};
    Kind kind;std::string text;uint16_t port=0;bool enabled=false;float value=0;
};
class Network {
public:
    virtual ~Network()=default;
    virtual Snapshot snapshot()const=0; // Thread-safe copy, no blocking UI/network waits.
    // The ONLINE tab is on screen this frame (snapshot() alone, e.g. from SETTINGS, starts nothing):
    // the network side may look for remembered friends who are hosting while it stays on screen.
    virtual void viewing_online(){}
    virtual void submit(Command command)=0;
};
void set_network(std::shared_ptr<Network>network);
void set_settings_applied(std::function<void()>callback);
// All-default launch/recovery; no startup mod chooser. Headless never opens UI.
bool prepare(std::string&data,const std::string&config,bool headless,const DiscId&id,PlatformConfig&display);
void show_menu();
void tick();
bool captures_input();
bool paused();
bool controller_first();
}
