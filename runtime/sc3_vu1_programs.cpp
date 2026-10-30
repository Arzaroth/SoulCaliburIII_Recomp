#include "sc3_vu1_ops.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace sc3
{
    namespace
    {
        struct Dispatcher
        {
            uint32_t program, pc, offset;
        };

        constexpr Dispatcher kDispatchers[] = {
            {kProgramA, 0x10u, 17u}, {kProgramA, 0x1c80u, 14u}, {kProgramA, 0x2098u, 14u},
            {kProgramB, 0x10u, 17u}, {kProgramB, 0x1990u, 14u}, {kProgramB, 0x21e0u, 14u},
        };
    }

    namespace vu1
    {
        template <bool kSticky>
        uint64_t programA(uint32_t entry, VU1State &state, uint8_t *data, uint32_t top, Vu1Kick out, void *user);
        template <bool kSticky>
        uint64_t programB(uint32_t entry, VU1State &state, uint8_t *data, uint32_t top, Vu1Kick out, void *user);

        constexpr uint32_t kEntriesA[] = {0x10u, 0xd8u, 0x2b0u, 0x450u, 0x768u, 0xa48u, 0xc18u, 0xda8u, 0xf68u, 0x1268u,
                                          0x1558u, 0x1730u, 0x18c0u, 0x1ab8u, 0x1c80u, 0x1d28u, 0x1f00u, 0x2098u, 0x2140u, 0x2330u};
        constexpr uint32_t kTargetsA[] = {0xd8u, 0x2b0u, 0x450u, 0x768u, 0xa48u, 0xc18u, 0xda8u, 0xf68u, 0x1268u,
                                          0x1558u, 0x1730u, 0x18c0u, 0x1ab8u, 0x1d28u, 0x1f00u, 0x2140u, 0x2330u};
        constexpr uint32_t kEntriesB[] = {0x10u, 0xd8u, 0x350u, 0x5b0u, 0x910u, 0xb78u, 0xdd0u, 0x10f0u, 0x1388u,
                                          0x15e8u, 0x1990u, 0x1a40u, 0x1ca8u, 0x1ef0u, 0x21e0u, 0x2290u, 0x24f8u, 0x2740u};
        constexpr uint32_t kTargetsB[] = {0xd8u, 0x350u, 0x5b0u, 0x910u, 0xb78u, 0xdd0u, 0x10f0u, 0x1388u,
                                          0x15e8u, 0x1a40u, 0x1ca8u, 0x1ef0u, 0x2290u, 0x24f8u, 0x2740u};

        template <size_t N>
        bool contains(const uint32_t (&list)[N], uint32_t pc)
        {
            return std::find(std::begin(list), std::end(list), pc) != std::end(list);
        }
    }

    uint32_t vu1DispatchTarget(uint32_t program, uint32_t pc, const uint8_t *data, uint32_t top)
    {
        for (const Dispatcher &d : kDispatchers)
            if (d.program == program && d.pc == pc)
            {
                const uint32_t qword = (top + 0x1adu + d.offset) & 0x3FFu;
                uint32_t word;
                std::memcpy(&word, data + qword * 16u + 8u, 4);
                return ((word & 0xFFFFu) * 8u) & 0x3FFFu;
            }
        return 0u;
    }

    bool vu1Ported(uint32_t program, uint32_t pc, const uint8_t *data, uint32_t top)
    {
        const uint32_t target = vu1DispatchTarget(program, pc, data, top);
        if (program == kProgramA)
            return vu1::contains(vu1::kEntriesA, pc) && (target == 0u || vu1::contains(vu1::kTargetsA, target));
        if (program == kProgramB)
            return vu1::contains(vu1::kEntriesB, pc) && (target == 0u || vu1::contains(vu1::kTargetsB, target));
        return false;
    }

    uint64_t runVu1Program(uint32_t program, uint32_t pc, VU1State &state, uint8_t *data, uint32_t top, uint32_t itop, Vu1Kick kick,
                           void *user)
    {
        if (program == kProgramB && pc == kSkinnedBodyPc && skinnedBodyReady(state))
            return runSkinnedBody(state, data, top, itop, kick, user);
        const vu1::RoundTowardZero rounding;
        const bool sticky = (state.status & 0x0C0u) != 0x0C0u;
        uint64_t cycles;
        if (program == kProgramA)
            cycles = sticky ? vu1::programA<true>(pc, state, data, top, kick, user) : vu1::programA<false>(pc, state, data, top, kick, user);
        else
            cycles = sticky ? vu1::programB<true>(pc, state, data, top, kick, user) : vu1::programB<false>(pc, state, data, top, kick, user);
        state.top = top;
        state.itop = itop;
        return cycles;
    }
}
