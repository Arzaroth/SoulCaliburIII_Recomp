#include "sc3_vu1_port.h"

#include <cstring>

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
}
