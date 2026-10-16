// eodsave.h - the game's memory card save file (BASLUS-20910DRIVEN/BASLUS-20910DRIVEN):
// reading, editing a few known values and writing it back with a valid checksum.
//
// Layout: a 0x90-byte header ("HSCM", version 3, header size, payload size,
// checksum, section count, then tag/version/size per section) followed by the
// sections in order. Sections: OPTs (the game's named settings), Idnt
// (player names), Chmp, Rep0 (reputation), Feat, MVid, Crer (career: money at
// +0x84, a copy of the upgrades data from +0x80 and of the standings from
// +0xCE8), CCar and Pant (paint jobs).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct EodSaveOption {
    std::string name;
    int group = 0;  // settings group (see eodsave_group_name)
    int type = 0;   // 0 any, 1 yes/no, 2 integer, 3 text (controller bindings), 4 decimal
    std::string value;
};

class EodSave {
public:
    bool load(const std::string &path, std::string &err);
    // Writes the save (checksum updated).
    bool save(const std::string &path, std::string &err);
    bool checksum_ok() const { return checksum_ok_; }

    // named settings
    const std::vector<EodSaveOption> &options() const { return opts_; }
    bool set_option(size_t index, const std::string &text, std::string &err);

    // career (when the save has one)
    bool has_career() const { return crer_ != 0; }
    int32_t money() const;
    void set_money(int32_t v);
    // reputation: total = a * 100 + b * 50 + points (the game caps it at 999999)
    bool has_reputation() const { return rep_ != 0; }
    void reputation(int32_t &a, int32_t &b, int32_t &points) const;
    void set_reputation(int32_t a, int32_t b, int32_t points);
    static int32_t reputation_total(int32_t a, int32_t b, int32_t points);

    std::string player_name() const;  // the first identity's name

    // career garage: the cars owned (up to 4 slots), each a career vehicle
    // (800-929: one of the dealer's cars, see eodsave_vehicles)
    int car_count() const;
    int slot_vehicle(int slot) const;
    // Puts career vehicles in the slots (one per owned car, all different);
    // the upgrades bought for a slot's car move with the slot.
    bool set_slot_vehicles(const std::vector<int> &vehicles, std::string &err);

private:
    int32_t rd(size_t off) const;
    void wr(size_t off, int32_t v);
    std::vector<uint8_t> d_;
    std::vector<EodSaveOption> opts_;
    std::vector<size_t> opt_off_;  // record offsets
    size_t crer_ = 0, rep_ = 0, idnt_ = 0;
    bool checksum_ok_ = false;
};

// The game's vehicles: a model in one paint job. 800-929 are the career
// cars the dealer sells (5 per model); the others are Action mode paint jobs
// and the special vehicles (bus, hearse, taxi...).
struct EodVehicle {
    int id;
    const char *model;
    uint8_t r, g, b;  // body colour
    int car_class;    // 0 compact, 1 midsize, 2 muscle, 3 special
};
const std::vector<EodVehicle> &eodsave_vehicles();
const EodVehicle *eodsave_vehicle(int id);
// "Durand - career paint, white", "Durand - Action paint 2", "Bus 3"
std::string eodsave_vehicle_label(int id);
inline bool eodsave_is_career_vehicle(int id) { return id >= 800 && id <= 929; }

// Career cars shown as another vehicle by the port (eod.ini [Career]
// CarLooks=845:790,...: career vehicle -> the vehicle it looks like).
std::vector<std::pair<int, int>> eodsave_parse_looks(const std::string &s);
std::string eodsave_format_looks(const std::vector<std::pair<int, int>> &looks);

// Puts any vehicles in the garage slots (one per owned car): a career vehicle
// goes into the save; any other one is shown by the port over the career car
// the slot already has (looks updated).
bool eodsave_set_slots(EodSave &save, const std::vector<int> &shown, std::vector<std::pair<int, int>> &looks,
                       std::string &err);
// What a slot shows: its career car, or the vehicle in looks for it.
int eodsave_slot_shown(const EodSave &save, int slot, const std::vector<std::pair<int, int>> &looks);

const char *eodsave_group_name(int group);
uint32_t eodsave_checksum(const uint8_t *p, size_t n);
