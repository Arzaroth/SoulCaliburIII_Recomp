#pragma once

#include <cstdint>

struct VU1State;

namespace sc3
{
    class DrawSink;

    constexpr uint32_t kSkinnedBodyPc = 0x1388u;

    using Vu1Kick = void (*)(void *user, const uint8_t *packet, uint32_t bytes);

    // Program B at 0x1388 on the VU state and the 16 KB of VU1 data memory, as VU1Interpreter runs it: every register,
    // the clip flag, MAC and status, the memory it writes, each XGKICK packet handed to kick. Returns the VU cycles.
    // Status must already hold ZS and SS (skinnedBodyReady): their sticky updates are not computed.
    uint64_t runSkinnedBody(VU1State &state, uint8_t *data, uint32_t top, uint32_t itop, Vu1Kick kick, void *user, bool fast = true);
    bool skinnedBodyReady(const VU1State &state);
    uint64_t drawSkinnedBody(VU1State &state, uint8_t *data, uint32_t top, uint32_t itop, DrawSink &sink);

    // SC3's VU1 programs, A (0) and B (1), from a start pc as VU1Interpreter runs them (registers, flags, memory,
    // packets, cycles). A start pc whose setup jumps on (JR) is ported for the targets the port knows.
    constexpr uint32_t kProgramA = 0u, kProgramB = 1u;
    uint32_t vu1DispatchTarget(uint32_t program, uint32_t pc, const uint8_t *data, uint32_t top);
    bool vu1Ported(uint32_t program, uint32_t pc, const uint8_t *data, uint32_t top);
    uint64_t runVu1Program(uint32_t program, uint32_t pc, VU1State &state, uint8_t *data, uint32_t top, uint32_t itop, Vu1Kick kick,
                           void *user);
    uint64_t drawVu1Program(uint32_t program, uint32_t pc, VU1State &state, uint8_t *data, uint32_t top, uint32_t itop, DrawSink &sink);

    // SC3's VU0 microprograms (VCALLMS), by the hash of VU0's 4 KB of code memory and the start pc: VU0 as
    // VU1Interpreter runs it from a reset, on its 4 KB of data memory. Returns the VU cycles.
    bool vu0Ported(uint64_t image, uint32_t pc);
    uint64_t runVu0Program(uint64_t image, uint32_t pc, VU1State &state, uint8_t *data);
}
