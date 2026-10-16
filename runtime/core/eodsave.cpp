// eodsave.cpp - the game's save file (see eodsave.h).
#include "eodsave.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace {
const uint32_t kMagic = 0x4D435348;  // "HSCM"
const size_t kHeader = 0x90;
const size_t kOptRecord = 56;  // type, group, flag, int, float, text[16], name[20]
// career section: owned car count, upgrade list (count, a word, then part, car,
// 0, level bytes), then 4 slots of 0x300 bytes starting with the car's dealer index
const size_t kCrerCars = 0x8C, kCrerUpgrades = 0x90, kCrerSlots = 0xE8, kSlotSize = 0x300;

const EodVehicle kVehicles[] = {
    {910, "Hurricane", 0x43, 0x62, 0x78, 0},
    {911, "Hurricane", 0x77, 0xAC, 0xC0, 0},
    {912, "Hurricane", 0x00, 0x00, 0x10, 0},
    {913, "Hurricane", 0x77, 0x50, 0x07, 0},
    {914, "Hurricane", 0x11, 0x34, 0x9C, 0},
    {51, "Hurricane", 0x62, 0x39, 0x31, 0},
    {159, "Hurricane", 0x62, 0x39, 0x31, 0},
    {199, "Hurricane", 0x62, 0x39, 0x31, 0},
    {296, "Hurricane", 0x62, 0x39, 0x31, 0},
    {311, "Hurricane", 0x62, 0x39, 0x31, 0},
    {820, "Moth", 0x00, 0x00, 0x10, 0},
    {821, "Moth", 0x00, 0x41, 0x00, 0},
    {822, "Moth", 0x00, 0x9E, 0xB5, 0},
    {823, "Moth", 0x00, 0x2C, 0xAD, 0},
    {824, "Moth", 0x6C, 0x56, 0x33, 0},
    {4, "Moth", 0x62, 0x39, 0x31, 0},
    {23, "Moth", 0x62, 0x39, 0x31, 0},
    {28, "Moth", 0x62, 0x39, 0x31, 0},
    {60, "Moth", 0x62, 0x39, 0x31, 0},
    {85, "Moth", 0x62, 0x39, 0x31, 0},
    {815, "Palo", 0x00, 0x00, 0x10, 0},
    {816, "Palo", 0x94, 0x8A, 0x7B, 0},
    {817, "Palo", 0x00, 0x2C, 0xAD, 0},
    {818, "Palo", 0x73, 0xDB, 0xE7, 0},
    {819, "Palo", 0x00, 0x41, 0x00, 0},
    {6, "Palo", 0x62, 0x39, 0x31, 0},
    {25, "Palo", 0x62, 0x39, 0x31, 0},
    {46, "Palo", 0x62, 0x39, 0x31, 0},
    {64, "Palo", 0x62, 0x39, 0x31, 0},
    {141, "Palo", 0x62, 0x39, 0x31, 0},
    {865, "Top", 0x6A, 0x65, 0x63, 0},
    {866, "Top", 0x49, 0x7D, 0x85, 0},
    {867, "Top", 0xA8, 0x8C, 0x51, 0},
    {868, "Top", 0x7E, 0xD1, 0xDB, 0},
    {869, "Top", 0x2A, 0x41, 0x7C, 0},
    {77, "Top", 0x62, 0x39, 0x31, 0},
    {152, "Top", 0x62, 0x39, 0x31, 0},
    {210, "Top", 0x62, 0x39, 0x31, 0},
    {354, "Top", 0x62, 0x39, 0x31, 0},
    {392, "Top", 0x62, 0x39, 0x31, 0},
    {805, "Algar", 0xE5, 0xE5, 0xE5, 0},
    {806, "Algar", 0x08, 0x34, 0x52, 0},
    {807, "Algar", 0x00, 0x2C, 0xAD, 0},
    {808, "Algar", 0x73, 0xDB, 0xE7, 0},
    {809, "Algar", 0xA5, 0x6D, 0x00, 0},
    {2, "Algar", 0x62, 0x39, 0x31, 0},
    {5, "Algar", 0x62, 0x39, 0x31, 0},
    {20, "Algar", 0x62, 0x39, 0x31, 0},
    {40, "Algar", 0x62, 0x39, 0x31, 0},
    {61, "Algar", 0x62, 0x39, 0x31, 0},
    {890, "Blitz", 0x1B, 0x49, 0x1B, 0},
    {891, "Blitz", 0x00, 0x08, 0x9C, 0},
    {892, "Blitz", 0x80, 0x96, 0x9C, 0},
    {893, "Blitz", 0x1C, 0x1C, 0x1C, 0},
    {894, "Blitz", 0x4F, 0x44, 0x30, 0},
    {38, "Blitz", 0x62, 0x39, 0x31, 0},
    {175, "Blitz", 0x62, 0x39, 0x31, 0},
    {234, "Blitz", 0x62, 0x39, 0x31, 0},
    {271, "Blitz", 0x62, 0x39, 0x31, 0},
    {338, "Blitz", 0x62, 0x39, 0x31, 0},
    {810, "Bulldog", 0x6C, 0x56, 0x33, 0},
    {811, "Bulldog", 0x6B, 0x9E, 0xA5, 0},
    {812, "Bulldog", 0x63, 0xA2, 0x73, 0},
    {813, "Bulldog", 0x10, 0x59, 0x8C, 0},
    {814, "Bulldog", 0x00, 0x00, 0x10, 0},
    {10, "Bulldog", 0x62, 0x39, 0x31, 0},
    {21, "Bulldog", 0x62, 0x39, 0x31, 0},
    {48, "Bulldog", 0x62, 0x39, 0x31, 0},
    {62, "Bulldog", 0x62, 0x39, 0x31, 0},
    {81, "Bulldog", 0x62, 0x39, 0x31, 0},
    {800, "Digit", 0xD1, 0xEB, 0xF9, 0},
    {801, "Digit", 0x95, 0x90, 0x5F, 0},
    {802, "Digit", 0x00, 0x08, 0x9C, 0},
    {803, "Digit", 0x63, 0xA2, 0x73, 0},
    {804, "Digit", 0x47, 0x43, 0x38, 0},
    {9, "Digit", 0x62, 0x39, 0x31, 0},
    {26, "Digit", 0x62, 0x39, 0x31, 0},
    {44, "Digit", 0x62, 0x39, 0x31, 0},
    {65, "Digit", 0x62, 0x39, 0x31, 0},
    {86, "Digit", 0x62, 0x39, 0x31, 0},
    {870, "Desio", 0x45, 0x3B, 0x45, 0},
    {871, "Desio", 0x63, 0x6B, 0x61, 0},
    {872, "Desio", 0x1C, 0x21, 0x7F, 0},
    {873, "Desio", 0x5E, 0x54, 0x12, 0},
    {874, "Desio", 0x4B, 0x86, 0x9B, 0},
    {18, "Desio", 0x62, 0x39, 0x31, 0},
    {134, "Desio", 0x62, 0x39, 0x31, 0},
    {173, "Desio", 0x62, 0x39, 0x31, 0},
    {215, "Desio", 0x62, 0x39, 0x31, 0},
    {356, "Desio", 0x62, 0x39, 0x31, 0},
    {915, "Arrow", 0x50, 0x53, 0x5B, 1},
    {916, "Arrow", 0x0C, 0x0F, 0x48, 1},
    {917, "Arrow", 0x8F, 0x7A, 0x67, 1},
    {918, "Arrow", 0x5B, 0x56, 0x2F, 1},
    {919, "Arrow", 0x4A, 0x93, 0xAF, 1},
    {74, "Arrow", 0x62, 0x39, 0x31, 1},
    {99, "Arrow", 0x62, 0x39, 0x31, 1},
    {139, "Arrow", 0x62, 0x39, 0x31, 1},
    {231, "Arrow", 0x62, 0x39, 0x31, 1},
    {299, "Arrow", 0x62, 0x39, 0x31, 1},
    {830, "Cannon", 0x02, 0x08, 0x84, 1},
    {831, "Cannon", 0x6C, 0x56, 0x33, 1},
    {832, "Cannon", 0x08, 0x34, 0x52, 1},
    {833, "Cannon", 0x00, 0x00, 0x10, 1},
    {834, "Cannon", 0xE5, 0xE5, 0xE5, 1},
    {22, "Cannon", 0x62, 0x39, 0x31, 1},
    {43, "Cannon", 0x62, 0x39, 0x31, 1},
    {165, "Cannon", 0x62, 0x39, 0x31, 1},
    {184, "Cannon", 0x62, 0x39, 0x31, 1},
    {925, "Empire", 0x29, 0x2F, 0x78, 1},
    {926, "Empire", 0x19, 0x54, 0xBC, 1},
    {927, "Empire", 0x63, 0x6B, 0x61, 1},
    {928, "Empire", 0x09, 0x38, 0x09, 1},
    {929, "Empire", 0x55, 0x4A, 0x12, 1},
    {57, "Empire", 0x62, 0x39, 0x31, 1},
    {196, "Empire", 0x62, 0x39, 0x31, 1},
    {232, "Empire", 0x62, 0x39, 0x31, 1},
    {291, "Empire", 0x62, 0x39, 0x31, 1},
    {377, "Empire", 0x62, 0x39, 0x31, 1},
    {835, "Guerilla", 0x00, 0x00, 0x10, 1},
    {836, "Guerilla", 0xA5, 0x6D, 0x00, 1},
    {837, "Guerilla", 0x63, 0xA2, 0x73, 1},
    {838, "Guerilla", 0x73, 0xDB, 0xE7, 1},
    {839, "Guerilla", 0x00, 0x08, 0x9C, 1},
    {63, "Guerilla", 0x62, 0x39, 0x31, 1},
    {120, "Guerilla", 0x62, 0x39, 0x31, 1},
    {148, "Guerilla", 0x62, 0x39, 0x31, 1},
    {162, "Guerilla", 0x62, 0x39, 0x31, 1},
    {180, "Guerilla", 0x62, 0x39, 0x31, 1},
    {825, "Hunter", 0x00, 0x08, 0x9C, 1},
    {826, "Hunter", 0x73, 0xDB, 0xE7, 1},
    {827, "Hunter", 0x6C, 0x56, 0x33, 1},
    {828, "Hunter", 0x00, 0x41, 0x00, 1},
    {829, "Hunter", 0x08, 0x34, 0x52, 1},
    {49, "Hunter", 0x62, 0x39, 0x31, 1},
    {89, "Hunter", 0x62, 0x39, 0x31, 1},
    {126, "Hunter", 0x62, 0x39, 0x31, 1},
    {166, "Hunter", 0x62, 0x39, 0x31, 1},
    {182, "Hunter", 0x62, 0x39, 0x31, 1},
    {885, "Limo", 0x45, 0x34, 0x33, 1},
    {886, "Limo", 0x54, 0x50, 0x4E, 1},
    {887, "Limo", 0x67, 0x67, 0x8F, 1},
    {888, "Limo", 0x20, 0x18, 0x56, 1},
    {889, "Limo", 0x5C, 0x52, 0x1A, 1},
    {76, "Limo", 0x62, 0x39, 0x31, 1},
    {136, "Limo", 0x62, 0x39, 0x31, 1},
    {279, "Limo", 0x62, 0x39, 0x31, 1},
    {315, "Limo", 0x62, 0x39, 0x31, 1},
    {378, "Limo", 0x62, 0x39, 0x31, 1},
    {900, "Ocelot", 0x65, 0x4C, 0x1E, 1},
    {901, "Ocelot", 0x5C, 0xA5, 0xAC, 1},
    {902, "Ocelot", 0x49, 0x54, 0x44, 1},
    {903, "Ocelot", 0x1B, 0x20, 0x8B, 1},
    {904, "Ocelot", 0x48, 0x49, 0x4A, 1},
    {73, "Ocelot", 0x62, 0x39, 0x31, 1},
    {98, "Ocelot", 0x62, 0x39, 0x31, 1},
    {111, "Ocelot", 0x62, 0x39, 0x31, 1},
    {254, "Ocelot", 0x62, 0x39, 0x31, 1},
    {337, "Ocelot", 0x62, 0x39, 0x31, 1},
    {860, "Prince", 0x5F, 0x54, 0x12, 1},
    {861, "Prince", 0x2A, 0x3D, 0xA4, 1},
    {862, "Prince", 0x63, 0x6B, 0x61, 1},
    {863, "Prince", 0x0F, 0x18, 0x3E, 1},
    {864, "Prince", 0x6D, 0xB5, 0xBD, 1},
    {31, "Prince", 0x62, 0x39, 0x31, 1},
    {117, "Prince", 0x62, 0x39, 0x31, 1},
    {219, "Prince", 0x62, 0x39, 0x31, 1},
    {238, "Prince", 0x62, 0x39, 0x31, 1},
    {351, "Prince", 0x62, 0x39, 0x31, 1},
    {895, "Gladiator", 0x46, 0x30, 0x08, 2},
    {896, "Gladiator", 0x19, 0x38, 0x94, 2},
    {897, "Gladiator", 0x06, 0x2A, 0x42, 2},
    {898, "Gladiator", 0x27, 0x4C, 0x1C, 2},
    {899, "Gladiator", 0x1A, 0x1A, 0x1A, 2},
    {13, "Gladiator", 0x62, 0x39, 0x31, 2},
    {55, "Gladiator", 0x62, 0x39, 0x31, 2},
    {113, "Gladiator", 0x62, 0x39, 0x31, 2},
    {179, "Gladiator", 0x62, 0x39, 0x31, 2},
    {211, "Gladiator", 0x62, 0x39, 0x31, 2},
    {880, "Mongoose", 0x0F, 0x18, 0x3E, 2},
    {881, "Mongoose", 0x42, 0x88, 0x92, 2},
    {882, "Mongoose", 0x17, 0x1C, 0x71, 2},
    {883, "Mongoose", 0x33, 0x67, 0x0F, 2},
    {884, "Mongoose", 0x75, 0x63, 0x50, 2},
    {36, "Mongoose", 0x62, 0x39, 0x31, 2},
    {53, "Mongoose", 0x62, 0x39, 0x31, 2},
    {172, "Mongoose", 0x62, 0x39, 0x31, 2},
    {194, "Mongoose", 0x62, 0x39, 0x31, 2},
    {316, "Mongoose", 0x62, 0x39, 0x31, 2},
    {855, "Orbital", 0x08, 0x34, 0x52, 2},
    {856, "Orbital", 0x6C, 0x56, 0x33, 2},
    {857, "Orbital", 0x0F, 0x24, 0x6C, 2},
    {858, "Orbital", 0x00, 0x9E, 0xB5, 2},
    {859, "Orbital", 0x00, 0x2C, 0xAD, 2},
    {7, "Orbital", 0x62, 0x39, 0x31, 2},
    {42, "Orbital", 0x62, 0x39, 0x31, 2},
    {121, "Orbital", 0x62, 0x39, 0x31, 2},
    {122, "Orbital", 0x62, 0x39, 0x31, 2},
    {168, "Orbital", 0x62, 0x39, 0x31, 2},
    {920, "Outlaw", 0x6A, 0x81, 0x83, 2},
    {921, "Outlaw", 0x92, 0x88, 0x6D, 2},
    {922, "Outlaw", 0x29, 0x29, 0x29, 2},
    {923, "Outlaw", 0x26, 0x42, 0x33, 2},
    {924, "Outlaw", 0x1F, 0x2F, 0x89, 2},
    {11, "Outlaw", 0x62, 0x39, 0x31, 2},
    {94, "Outlaw", 0x62, 0x39, 0x31, 2},
    {133, "Outlaw", 0x62, 0x39, 0x31, 2},
    {193, "Outlaw", 0x62, 0x39, 0x31, 2},
    {395, "Outlaw", 0x62, 0x39, 0x31, 2},
    {850, "Rocket", 0x2D, 0x67, 0x3F, 2},
    {851, "Rocket", 0xB5, 0x8E, 0x39, 2},
    {852, "Rocket", 0x00, 0x9E, 0xB5, 2},
    {853, "Rocket", 0x00, 0x00, 0x10, 2},
    {854, "Rocket", 0x00, 0x08, 0x9C, 2},
    {1, "Rocket", 0x62, 0x39, 0x31, 2},
    {80, "Rocket", 0x62, 0x39, 0x31, 2},
    {109, "Rocket", 0x62, 0x39, 0x31, 2},
    {145, "Rocket", 0x62, 0x39, 0x31, 2},
    {160, "Rocket", 0x62, 0x39, 0x31, 2},
    {905, "Venom", 0x10, 0x59, 0x8C, 2},
    {906, "Venom", 0x11, 0x17, 0x8B, 2},
    {907, "Venom", 0x72, 0x52, 0x15, 2},
    {908, "Venom", 0x0A, 0x37, 0x0A, 2},
    {909, "Venom", 0x00, 0x00, 0x10, 2},
    {16, "Venom", 0x62, 0x39, 0x31, 2},
    {39, "Venom", 0x62, 0x39, 0x31, 2},
    {114, "Venom", 0x62, 0x39, 0x31, 2},
    {156, "Venom", 0x62, 0x39, 0x31, 2},
    {258, "Venom", 0x62, 0x39, 0x31, 2},
    {397, "Venom", 0x62, 0x39, 0x31, 2},
    {840, "Coyote", 0x95, 0x90, 0x5F, 2},
    {841, "Coyote", 0x00, 0x9E, 0xB5, 2},
    {842, "Coyote", 0xD1, 0xEB, 0xF9, 2},
    {843, "Coyote", 0x00, 0x08, 0x9C, 2},
    {844, "Coyote", 0x00, 0x00, 0x10, 2},
    {8, "Coyote", 0x62, 0x39, 0x31, 2},
    {41, "Coyote", 0x62, 0x39, 0x31, 2},
    {83, "Coyote", 0x62, 0x39, 0x31, 2},
    {127, "Coyote", 0x62, 0x39, 0x31, 2},
    {143, "Coyote", 0x62, 0x39, 0x31, 2},
    {845, "Durand", 0xE5, 0xE5, 0xE5, 2},
    {846, "Durand", 0x47, 0x43, 0x38, 2},
    {847, "Durand", 0x6B, 0x9E, 0xA5, 2},
    {848, "Durand", 0x00, 0x08, 0x9C, 2},
    {849, "Durand", 0xA5, 0x6D, 0x00, 2},
    {68, "Durand", 0x62, 0x39, 0x31, 2},
    {107, "Durand", 0x62, 0x39, 0x31, 2},
    {146, "Durand", 0x62, 0x39, 0x31, 2},
    {161, "Durand", 0x62, 0x39, 0x31, 2},
    {186, "Durand", 0x62, 0x39, 0x31, 2},
    {875, "Fillmore", 0x60, 0x87, 0x8C, 2},
    {876, "Fillmore", 0x12, 0x35, 0x9B, 2},
    {877, "Fillmore", 0x94, 0x8A, 0x7B, 2},
    {878, "Fillmore", 0x5B, 0x8A, 0x67, 2},
    {879, "Fillmore", 0x0F, 0x24, 0x6C, 2},
    {72, "Fillmore", 0x62, 0x39, 0x31, 2},
    {91, "Fillmore", 0x62, 0x39, 0x31, 2},
    {255, "Fillmore", 0x62, 0x39, 0x31, 2},
    {274, "Fillmore", 0x62, 0x39, 0x31, 2},
    {333, "Fillmore", 0x62, 0x39, 0x31, 2},
    {540, "Bus", 0x62, 0x39, 0x31, 3},
    {602, "Bus", 0x62, 0x39, 0x31, 3},
    {620, "Bus", 0x62, 0x39, 0x31, 3},
    {790, "Bus", 0x62, 0x39, 0x31, 3},
    {791, "Bus", 0x62, 0x39, 0x31, 3},
    {792, "Bus", 0x62, 0x39, 0x31, 3},
    {793, "Bus", 0x62, 0x39, 0x31, 3},
    {794, "Bus", 0x62, 0x39, 0x31, 3},
    {504, "Copcar", 0x62, 0x39, 0x31, 3},
    {502, "Hearse", 0x62, 0x39, 0x31, 3},
    {505, "Hearse", 0x62, 0x39, 0x31, 3},
    {506, "Hearse", 0x62, 0x39, 0x31, 3},
    {507, "Hearse", 0x62, 0x39, 0x31, 3},
    {503, "Postal", 0x62, 0x39, 0x31, 3},
    {508, "Taxi", 0x62, 0x39, 0x31, 3},
    {601, "Ambulance", 0x62, 0x39, 0x31, 3},
};

// a name for a body colour
const char *colour_name(int r, int g, int b) {
    int mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    if (mx < 40) return "black";
    if (mx - mn < 24 || (mx - mn) * 4 < mx) return mx > 200 ? "white" : mx > 120 ? "silver" : "grey";
    float h;
    float d = (float)(mx - mn);
    if (mx == r) h = 60.0f * (float)(g - b) / d;
    else if (mx == g) h = 60.0f * (2.0f + (float)(b - r) / d);
    else h = 60.0f * (4.0f + (float)(r - g) / d);
    if (h < 0) h += 360.0f;
    bool dark = mx < 130;
    if (h < 20 || h >= 340) return dark ? "dark red" : "red";
    if (h < 45) return dark ? "brown" : "orange";
    if (h < 70) return dark ? "olive" : "yellow";
    if (h < 160) return dark ? "dark green" : "green";
    if (h < 200) return dark ? "teal" : "light blue";
    if (h < 260) return dark ? "dark blue" : "blue";
    return dark ? "dark purple" : "purple";
}

uint32_t tag(const char *s) { return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | (uint8_t)s[3]; }

std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool parse_number(const std::string &s, double &v) {
    if (s.empty()) return false;
    char *end;
    v = strtod(s.c_str(), &end);
    return *end == 0;
}
}  // namespace

uint32_t eodsave_checksum(const uint8_t *p, size_t n) {
    // as the game computes it: add each byte, rotate right by one, xor
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) {
        s += p[i];
        s = (s >> 1) | (s << 31);
        s ^= 0xFEEDFACEu;
    }
    return s;
}

const char *eodsave_group_name(int group) {
    switch (group) {
    case 0: return "General";
    case 1: return "Video";
    case 4: return "Sound";
    case 5: return "Gameplay";
    case 8: return "Controller 1";
    case 9: return "Controller 2";
    case 10: return "Controller 3";
    case 11: return "Controller 4";
    case 12: return "Controller 5";
    case 13: return "Driving aids 1";
    case 14: return "Driving aids 2";
    case 15: return "Driving aids 3";
    case 16: return "Driving aids 4";
    case 17: return "Driving aids 5";
    default: return "Other";
    }
}

int32_t EodSave::rd(size_t off) const {
    int32_t v;
    memcpy(&v, &d_[off], 4);
    return v;
}

void EodSave::wr(size_t off, int32_t v) { memcpy(&d_[off], &v, 4); }

bool EodSave::load(const std::string &path, std::string &err) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        err = "can't open " + path;
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < (long)kHeader || size > (16 << 20)) {
        fclose(f);
        err = "not a save of this game (unexpected size)";
        return false;
    }
    d_.resize((size_t)size);
    size_t got = fread(d_.data(), 1, d_.size(), f);
    fclose(f);
    if (got != d_.size() || (uint32_t)rd(0) != kMagic || rd(4) != 3 || rd(8) != (int32_t)kHeader ||
        (size_t)rd(0xC) != d_.size() - kHeader) {
        err = "not a save of this game (bad header)";
        return false;
    }
    checksum_ok_ = eodsave_checksum(&d_[kHeader], d_.size() - kHeader) == (uint32_t)rd(0x10);
    int n = rd(0x14);
    if (n < 0 || n > 9 || 0x18 + (size_t)n * 12 > kHeader) {
        err = "not a save of this game (bad section table)";
        return false;
    }
    size_t off = kHeader;
    crer_ = rep_ = idnt_ = 0;
    size_t opts = 0, opts_size = 0;
    for (int i = 0; i < n; i++) {
        uint32_t t = (uint32_t)rd(0x18 + i * 12);
        size_t sz = (size_t)rd(0x18 + i * 12 + 8);
        if (sz > d_.size() - off) {
            err = "not a save of this game (section sizes)";
            return false;
        }
        if (t == tag("Crer") && sz == 0xE7C) crer_ = off;
        if (t == tag("Rep0") && sz == 12) rep_ = off;
        if (t == tag("Idnt") && sz >= 22) idnt_ = off;
        if (t == tag("OPTs")) opts = off, opts_size = sz;
        off += sz;
    }
    opts_.clear();
    opt_off_.clear();
    if (opts && opts_size >= 8) {
        size_t count = (size_t)rd(opts + 4);
        if (count <= (opts_size - 8) / kOptRecord) {
            for (size_t i = 0; i < count; i++) {
                size_t r = opts + 8 + i * kOptRecord;
                EodSaveOption o;
                o.type = rd(r);
                o.group = rd(r + 4);
                char text[17] = {}, name[21] = {};
                memcpy(text, &d_[r + 20], 16);
                memcpy(name, &d_[r + 36], 20);
                o.value = text;
                o.name = name;
                opts_.push_back(o);
                opt_off_.push_back(r);
            }
        }
    }
    return true;
}

bool EodSave::save(const std::string &path, std::string &err) {
    uint32_t c = eodsave_checksum(&d_[kHeader], d_.size() - kHeader);
    memcpy(&d_[0x10], &c, 4);
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) {
        err = "can't write " + tmp;
        return false;
    }
    bool ok = fwrite(d_.data(), 1, d_.size(), f) == d_.size();
    ok = fclose(f) == 0 && ok;
    remove(path.c_str());
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        err = "can't write " + path;
        return false;
    }
    checksum_ok_ = true;
    return true;
}

bool EodSave::set_option(size_t index, const std::string &text_in, std::string &err) {
    if (index >= opts_.size()) return false;
    EodSaveOption &o = opts_[index];
    std::string text = text_in, l = lower(text_in);
    double v = 0;
    bool yes = l == "yes" || l == "true" || l == "on", no = l == "no" || l == "false" || l == "off";
    switch (o.type) {
    case 1:  // yes / no
        if (!yes && !no && !(parse_number(text, v) && (v == 0 || v == 1))) {
            err = "enter yes or no";
            return false;
        }
        if (yes || no) v = yes ? 1 : 0;
        text = v ? "yes" : "no";
        break;
    case 2:  // integer
        if (!parse_number(text, v) || v != (double)(int32_t)v) {
            err = "enter a whole number";
            return false;
        }
        text = std::to_string((int32_t)v);
        break;
    case 4: {  // decimal
        if (!parse_number(text, v)) {
            err = "enter a number";
            return false;
        }
        char b[32];
        snprintf(b, sizeof b, "%f", (double)(float)v);
        text = b;
        break;
    }
    case 3:  // text
        v = 0;
        break;
    default:  // any: yes/no, a number or text
        if (yes || no) {
            v = yes ? 1 : 0;
            text = yes ? "yes" : "no";
        } else if (!parse_number(text, v)) {
            v = 0;
        }
        break;
    }
    if (text.size() > 15) {
        err = "at most 15 characters";
        return false;
    }
    size_t r = opt_off_[index];
    float fv = (float)v;
    wr(r + 8, fv != 0.0f ? 1 : 0);
    wr(r + 12, (int32_t)fv);
    memcpy(&d_[r + 16], &fv, 4);
    memset(&d_[r + 20], 0, 16);
    memcpy(&d_[r + 20], text.data(), text.size());
    o.value = text;
    return true;
}

int32_t EodSave::money() const { return crer_ ? rd(crer_ + 0x84) : 0; }

void EodSave::set_money(int32_t v) {
    if (crer_) wr(crer_ + 0x84, v);
}

void EodSave::reputation(int32_t &a, int32_t &b, int32_t &points) const {
    a = rep_ ? rd(rep_) : 0;
    b = rep_ ? rd(rep_ + 4) : 0;
    points = rep_ ? rd(rep_ + 8) : 0;
}

void EodSave::set_reputation(int32_t a, int32_t b, int32_t points) {
    if (!rep_) return;
    wr(rep_, a);
    wr(rep_ + 4, b);
    wr(rep_ + 8, points);
}

int32_t EodSave::reputation_total(int32_t a, int32_t b, int32_t points) {
    int64_t t = (int64_t)a * 100 + (int64_t)b * 50 + points;
    return t > 999999 ? 999999 : (int32_t)t;
}

std::string EodSave::player_name() const {
    if (!idnt_) return "";
    char n[19] = {};
    memcpy(n, &d_[idnt_ + 4], 18);
    return n;
}

int EodSave::car_count() const {
    if (!crer_) return 0;
    int n = rd(crer_ + kCrerCars);
    return n < 0 ? 0 : n > 4 ? 4 : n;
}

int EodSave::slot_vehicle(int slot) const {
    if (slot < 0 || slot >= car_count()) return -1;
    return 800 + rd(crer_ + kCrerSlots + (size_t)slot * kSlotSize);
}

bool EodSave::set_slot_vehicles(const std::vector<int> &vehicles, std::string &err) {
    int n = car_count();
    if ((int)vehicles.size() != n) {
        err = "one car per slot expected";
        return false;
    }
    for (int k = 0; k < n; k++) {
        if (!eodsave_is_career_vehicle(vehicles[(size_t)k])) {
            err = "slot " + std::to_string(k + 1) + ": not one of the dealer's cars";
            return false;
        }
        for (int j = 0; j < k; j++)
            if (vehicles[(size_t)j] == vehicles[(size_t)k]) {
                err = "slots " + std::to_string(j + 1) + " and " + std::to_string(k + 1) + " have the same car";
                return false;
            }
    }
    std::vector<int> old_index((size_t)n);
    for (int k = 0; k < n; k++) old_index[(size_t)k] = slot_vehicle(k) - 800;
    // the upgrades bought for the car in a slot stay with it (all slots at once: swaps work)
    int count = rd(crer_ + kCrerUpgrades);
    for (int i = 0; i < count && i < 256; i++) {
        size_t e = crer_ + kCrerUpgrades + 8 + (size_t)i * 4;
        if (e + 4 > crer_ + kCrerSlots) break;
        for (int k = 0; k < n; k++)
            if (d_[e + 1] == (uint8_t)old_index[(size_t)k]) {
                d_[e + 1] = (uint8_t)(vehicles[(size_t)k] - 800);
                break;
            }
    }
    for (int k = 0; k < n; k++) wr(crer_ + kCrerSlots + (size_t)k * kSlotSize, vehicles[(size_t)k] - 800);
    return true;
}

const std::vector<EodVehicle> &eodsave_vehicles() {
    static const std::vector<EodVehicle> v(std::begin(kVehicles), std::end(kVehicles));
    return v;
}

const EodVehicle *eodsave_vehicle(int id) {
    for (const EodVehicle &v : kVehicles)
        if (v.id == id) return &v;
    return nullptr;
}

std::string eodsave_vehicle_label(int id) {
    const EodVehicle *v = eodsave_vehicle(id);
    if (!v) return "vehicle " + std::to_string(id);
    // the paint job's number among the model's career / other paint jobs
    int nth = 0, total = 0;
    for (const EodVehicle &o : kVehicles)
        if (!strcmp(o.model, v->model) && eodsave_is_career_vehicle(o.id) == eodsave_is_career_vehicle(id)) {
            total++;
            if (o.id <= id) nth++;
        }
    std::string s = v->model;
    if (eodsave_is_career_vehicle(id)) return s + " - career, " + colour_name(v->r, v->g, v->b);
    if (v->car_class == 3) return total > 1 ? s + " " + std::to_string(nth) : s;
    return s + " - Action paint " + std::to_string(nth);
}

std::vector<std::pair<int, int>> eodsave_parse_looks(const std::string &s) {
    std::vector<std::pair<int, int>> out;
    size_t p = 0;
    while (p < s.size()) {
        size_t c = s.find(',', p);
        if (c == std::string::npos) c = s.size();
        std::string item = s.substr(p, c - p);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            int host = atoi(item.substr(0, colon).c_str()), look = atoi(item.substr(colon + 1).c_str());
            if (eodsave_is_career_vehicle(host) && eodsave_vehicle(look)) out.push_back({host, look});
        }
        p = c + 1;
    }
    return out;
}

std::string eodsave_format_looks(const std::vector<std::pair<int, int>> &looks) {
    std::string s;
    for (const auto &l : looks) {
        if (!s.empty()) s += ",";
        s += std::to_string(l.first) + ":" + std::to_string(l.second);
    }
    return s;
}

bool eodsave_set_slots(EodSave &save, const std::vector<int> &shown, std::vector<std::pair<int, int>> &looks,
                       std::string &err) {
    int n = save.car_count();
    if ((int)shown.size() != n) {
        err = "one car per slot expected";
        return false;
    }
    // each slot's career car: the one chosen, or (for another vehicle) the one it has
    std::vector<int> hosts((size_t)n);
    for (int k = 0; k < n; k++) {
        int v = shown[(size_t)k];
        if (!eodsave_vehicle(v)) {
            err = "no such vehicle " + std::to_string(v);
            return false;
        }
        hosts[(size_t)k] = eodsave_is_career_vehicle(v) ? v : save.slot_vehicle(k);
    }
    if (!save.set_slot_vehicles(hosts, err)) {
        if (err.find("same car") != std::string::npos)
            err += " underneath (a car shown as another vehicle keeps the career car it had)";
        return false;
    }
    looks.clear();  // (looks only matter for the cars owned)
    for (int k = 0; k < n; k++)
        if (!eodsave_is_career_vehicle(shown[(size_t)k])) looks.push_back({hosts[(size_t)k], shown[(size_t)k]});
    return true;
}

int eodsave_slot_shown(const EodSave &save, int slot, const std::vector<std::pair<int, int>> &looks) {
    int host = save.slot_vehicle(slot);
    for (const auto &l : looks)
        if (l.first == host) return l.second;
    return host;
}
