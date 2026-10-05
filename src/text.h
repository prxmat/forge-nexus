// Nexus' font only has Latin-1 (ASCII and the accented letters of Western Europe): any other character shows as "?".
// Forge's guides are full of typographic French (’ – … « œuf »), timers bring bullets and arrows. ForFont() maps them
// to what the font has: ’ → ', – — → -, … → ..., œ → oe, → → ->, thin spaces → no-break space; emoji go.
// Portable: tools/text-check runs it natively.
#pragma once
#include <string>

std::string ForFont(const std::string& utf8);
