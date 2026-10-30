#pragma once

#include <cstdint>

struct VU1State;

namespace sc3
{
    constexpr uint32_t kSkinnedBodyPc = 0x1388u;

    using Vu1Kick = void (*)(void *user, const uint8_t *packet, uint32_t bytes);

    // Program B at 0x1388 on the VU state and the 16 KB of VU1 data memory, as VU1Interpreter runs it: every register,
    // the clip flag, MAC and status, the memory it writes, each XGKICK packet handed to kick. Returns the VU cycles.
    // Status must already hold ZS and SS (skinnedBodyReady): their sticky updates are not computed.
    uint64_t runSkinnedBody(VU1State &state, uint8_t *data, uint32_t top, uint32_t itop, Vu1Kick kick, void *user, bool fast = true);
    bool skinnedBodyReady(const VU1State &state);

    // SC3's VU1 programs, A (0) and B (1). A start pc whose setup jumps on (JR vi1) to a routine whose address the
    // stream left in VU memory: that routine's pc, else 0.
    constexpr uint32_t kProgramA = 0u, kProgramB = 1u;
    uint32_t vu1DispatchTarget(uint32_t program, uint32_t pc, const uint8_t *data, uint32_t top);
}
