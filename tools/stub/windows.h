// Native stand-in for <windows.h> in the checks under tools/: a clock the test moves by hand.
#pragma once
#include <cstdint>
extern uint64_t g_fakeTicks;
inline uint64_t GetTickCount64() { return g_fakeTicks; }
