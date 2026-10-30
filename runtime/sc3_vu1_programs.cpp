#include "sc3_draw_sink.h"
#include "sc3_vu1_port.h"

#include "runtime/ps2_vu1_lifted_ops.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace ps2x::vu1lift
{
    template <bool kSticky, bool kDirect>
    uint64_t programA(uint32_t entry, VU1State &state, uint8_t *data, uint32_t top, Vu1Kick out, void *user);
    template <bool kSticky>
    uint64_t programVu0(uint32_t entry, VU1State &state, uint8_t *data, uint32_t top, Vu1Kick out, void *user);
    template <bool kSticky, bool kDirect>
    uint64_t programB(uint32_t entry, VU1State &state, uint8_t *data, uint32_t top, Vu1Kick out, void *user);
}

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

        constexpr uint32_t kEntriesA[] = {0x10u, 0xd8u, 0x2b0u, 0x450u, 0x768u, 0xa48u, 0xc18u, 0xda8u, 0xf68u, 0x1268u,
                                          0x1558u, 0x1730u, 0x18c0u, 0x1ab8u, 0x1c80u, 0x1d28u, 0x1f00u, 0x2098u, 0x2140u, 0x2330u};
        constexpr uint32_t kTargetsA[] = {0xd8u, 0x2b0u, 0x450u, 0x768u, 0xa48u, 0xc18u, 0xda8u, 0xf68u, 0x1268u,
                                          0x1558u, 0x1730u, 0x18c0u, 0x1ab8u, 0x1d28u, 0x1f00u, 0x2140u, 0x2330u};
        constexpr uint32_t kEntriesB[] = {0x10u, 0xd8u, 0x350u, 0x5b0u, 0x910u, 0xb78u, 0xdd0u, 0x10f0u, 0x1388u,
                                          0x15e8u, 0x1990u, 0x1a40u, 0x1ca8u, 0x1ef0u, 0x21e0u, 0x2290u, 0x24f8u, 0x2740u};
        constexpr uint32_t kTargetsB[] = {0xd8u, 0x350u, 0x5b0u, 0x910u, 0xb78u, 0xdd0u, 0x10f0u, 0x1388u,
                                          0x15e8u, 0x1a40u, 0x1ca8u, 0x1ef0u, 0x2290u, 0x24f8u, 0x2740u};

        constexpr uint64_t kVu0Image = 0x5e40f8313a95b45full;
        constexpr uint32_t kEntriesVu0[] = {0x0u, 0x18u, 0x38u, 0x60u, 0xf0u, 0x360u, 0x3c8u, 0x3e8u, 0x438u};

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
        const ps2x::vu1lift::RoundTowardZero rounding;
        const bool sticky = (state.status & 0x0C0u) != 0x0C0u;
        uint64_t cycles;
        if (program == kProgramA)
            cycles = sticky ? ps2x::vu1lift::programA<true, false>(pc, state, data, top, kick, user) : ps2x::vu1lift::programA<false, false>(pc, state, data, top, kick, user);
        else
            cycles = sticky ? ps2x::vu1lift::programB<true, false>(pc, state, data, top, kick, user) : ps2x::vu1lift::programB<false, false>(pc, state, data, top, kick, user);
        state.top = top;
        state.itop = itop;
        return cycles;
    }

    uint64_t drawVu1Program(uint32_t program, uint32_t pc, VU1State &state, uint8_t *data, uint32_t top, uint32_t itop, DrawSink &sink)
    {
        if (program == kProgramB && pc == kSkinnedBodyPc && skinnedBodyReady(state))
            return drawSkinnedBody(state, data, top, itop, sink);
        const ps2x::vu1lift::RoundTowardZero rounding;
        const bool sticky = (state.status & 0x0C0u) != 0x0C0u;
        uint64_t cycles;
        if (program == kProgramA)
            cycles = sticky ? ps2x::vu1lift::programA<true, true>(pc, state, data, top, nullptr, &sink)
                            : ps2x::vu1lift::programA<false, true>(pc, state, data, top, nullptr, &sink);
        else
            cycles = sticky ? ps2x::vu1lift::programB<true, true>(pc, state, data, top, nullptr, &sink)
                            : ps2x::vu1lift::programB<false, true>(pc, state, data, top, nullptr, &sink);
        state.top = top;
        state.itop = itop;
        return cycles;
    }

    bool vu0Ported(uint64_t image, uint32_t pc) { return image == vu1::kVu0Image && vu1::contains(vu1::kEntriesVu0, pc); }

    uint64_t runVu0Program(uint64_t, uint32_t pc, VU1State &state, uint8_t *data)
    {
        const ps2x::vu1lift::RoundTowardZero rounding;
        const bool sticky = (state.status & 0x0C0u) != 0x0C0u;
        const uint64_t cycles = sticky ? ps2x::vu1lift::programVu0<true>(pc, state, data, 0u, nullptr, nullptr)
                                       : ps2x::vu1lift::programVu0<false>(pc, state, data, 0u, nullptr, nullptr);
        state.top = 0u;
        return cycles;
    }
}
