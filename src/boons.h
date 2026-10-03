// The boons an aura can watch: arcdps buff id, the game's French name, the icon on render.guildwars2.com.
#pragma once
#include <cstdint>

struct BoonInfo { uint32_t id; const char* name; const char* icon; bool intensity; };

inline constexpr BoonInfo BOONS[] = {
    { 1187, "Célérité", "/file/D4AB6401A6D6917C3D4F230764452BCCE1035B0D/1012835.png", false },
    { 30328, "Alacrité", "/file/4FDAC2113B500104121753EF7E026E45C141E94D/1938787.png", false },
    { 740, "Pouvoir", "/file/2FA9DF9D6BC17839BBEA14723F1C53D645DDB5E1/102852.png", true },
    { 1122, "Stabilité", "/file/3D3A1C2D6D791C05179AB871902D28782C65C244/415959.png", true },
    { 725, "Fureur", "/file/96D90DF84CAFE008233DD1C2606A12C1A0E68048/102842.png", false },
    { 743, "Égide", "/file/DFB4D1B50AE4D6A275B349E15B179261EE3EB0AF/102854.png", false },
    { 717, "Protection", "/file/CD77D1FAB7B270223538A8F8ECDA1CFB044D65F4/102834.png", false },
    { 873, "Résolution", "/file/D104A6B9344A2E2096424A3C300E46BC2926E4D7/2440718.png", false },
    { 26980, "Résistance", "/file/50BAC1B8E10CFAB9E749A5D910D4A9DCF29EBB7C/961398.png", false },
    { 718, "Régénération", "/file/F69996772B9E18FD18AD0AABAB25D7E3FC42F261/102835.png", false },
    { 726, "Vigueur", "/file/58E92EBAF0DB4DA7C4AC04D9B22BCA5ECF0100DE/102843.png", false },
    { 719, "Rapidité", "/file/20CFC14967E67F7A3FD4A4B8722B4CF5B8565E11/102836.png", false },
    { 5974, "Super vitesse", "/file/E04392C3D8ED985125973AEB99D8460C483263F9/103458.png", false },
};

inline const BoonInfo* FindBoon(uint32_t id) {
    for (const auto& boon : BOONS) if (boon.id == id) return &boon;
    return nullptr;
}
