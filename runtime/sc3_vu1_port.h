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
}
