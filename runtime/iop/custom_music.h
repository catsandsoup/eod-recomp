// custom_music.h - the custom music library: finds the player's songs, decodes them on a
// background thread into a cache, keeps the per-song on/off state, and plays the chosen
// one through the SPU2 mixer in place of a native stream voice's ADPCM (see
// docs/CUSTOM_MUSIC.md). Local only: nothing here touches the EE, the simulation or its
// RNG; the only EE-visible event is the game's own stream queue request (stream.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace custom_music {

enum class State { Pending, Scanning, Ready, Skipped, Protected };

struct Song {
    std::string rel;     // path under the music folder (the key kept in state.json)
    std::string name;    // display name
    State state = State::Pending;
    std::string reason;  // why a file was skipped
    double seconds = 0;
    float gain = 1;
    bool enabled = true;
    uint32_t id = 0;     // virtual id, >= 0x8000
};
struct Progress {
    int total = 0, done = 0;
    bool scanning = false;
};

// root: the music folder (state.json lives in it); cache_dir: decoded songs. Idempotent.
void init(const std::string &root, const std::string &cache_dir);
// ~/Library/Application Support/TDEODRecomp/mods/music (EOD_MUSIC_DIR overrides it).
void init_default();
void shutdown();
void rescan();

std::vector<Song> songs();
Progress progress();
int ready_count();  // decoded and enabled
void set_enabled(const std::string &rel, bool on);
std::string root_dir();

// Stream-queue hook: called for every native music song the game queues. Returns 0 to
// play it as queued, or the virtual id of the custom song that replaces it.
uint32_t pick_for_native();

// Host voices, one per stream channel (= SPU2 core 0 voice 0/1). EE thread only.
void host_preload(int slot, uint32_t id);  // load the song's PCM (background)
void host_start(int slot);                 // begin playing it (when its voice reaches it)
void host_stop(int slot);
bool host_finished(int slot);              // ended, or failed to load (never stalls the queue)
bool host_active(int slot);
void host_begin_block();                   // the mixer, once per block: publish a loaded song
bool host_frame(int slot, int &l, int &r); // the mixer, per frame: one stereo sample

}  // namespace custom_music
