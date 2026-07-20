#pragma once
// Dong ho don dieu micro giay (QPC) - dung lam nowUs bom vao core (rgc) va lam
// timestamp video phia agent. Chung cho main/AgentLoop/ClientLoop.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>

inline uint64_t QpcUs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / freq.QuadPart) * 1'000'000ull +
           (uint64_t)(c.QuadPart % freq.QuadPart) * 1'000'000ull / (uint64_t)freq.QuadPart;
}
