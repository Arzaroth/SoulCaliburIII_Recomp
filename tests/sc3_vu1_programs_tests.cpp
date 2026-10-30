#include "MiniTest.h"
#include "sc3_vu1_port.h"

#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_lifted.h"
#include "runtime/ps2_vu1_trace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    constexpr uint32_t kMemory = 16u * 1024u;

    struct Known
    {
        uint32_t pairs;
        uint64_t hash;
    };

    constexpr Known kKnown[] = {{1177u, 0x95e81f9a833c1b45ull}, {1352u, 0xd2fbf7af837a8df4ull}};

    uint32_t bitsOf(float value)
    {
        uint32_t b;
        std::memcpy(&b, &value, 4);
        return b;
    }

    struct Run
    {
        VU1State state{};
        std::vector<uint8_t> data;
        std::vector<std::vector<uint8_t>> packets;
        uint64_t cycles = 0;
    };

    std::string compare(const Run &port, const Vu1TraceRecord &e)
    {
        std::string out;
        char item[160];
        const VU1State &a = port.state, &b = e.stateAfter;
        for (int r = 1; r < 32; ++r)
            if (std::memcmp(a.vf[r], b.vf[r], 16) != 0)
            {
                std::snprintf(item, sizeof(item), " vf%d (%08x %08x %08x %08x / %08x %08x %08x %08x)", r, bitsOf(a.vf[r][0]), bitsOf(a.vf[r][1]),
                              bitsOf(a.vf[r][2]), bitsOf(a.vf[r][3]), bitsOf(b.vf[r][0]), bitsOf(b.vf[r][1]), bitsOf(b.vf[r][2]), bitsOf(b.vf[r][3]));
                out += item;
            }
        for (int r = 1; r < 16; ++r)
            if (a.vi[r] != b.vi[r])
            {
                std::snprintf(item, sizeof(item), " vi%d %#x/%#x", r, a.vi[r], b.vi[r]);
                out += item;
            }
        if (std::memcmp(a.acc, b.acc, 16) != 0)
            out += " acc";
        if (bitsOf(a.q) != bitsOf(b.q) || bitsOf(a.i) != bitsOf(b.i) || bitsOf(a.p) != bitsOf(b.p))
        {
            std::snprintf(item, sizeof(item), " q %08x/%08x i %08x/%08x", bitsOf(a.q), bitsOf(b.q), bitsOf(a.i), bitsOf(b.i));
            out += item;
        }
        if (a.clip != b.clip || a.mac != b.mac || a.status != b.status || a.pc != b.pc)
        {
            std::snprintf(item, sizeof(item), " clip %#x/%#x mac %#x/%#x status %#x/%#x pc %#x/%#x", a.clip, b.clip, a.mac, b.mac, a.status,
                          b.status, a.pc, b.pc);
            out += item;
        }
        if (port.packets != e.packets)
        {
            std::snprintf(item, sizeof(item), " packets (%zu / %zu)", port.packets.size(), e.packets.size());
            out += item;
            for (size_t p = 0; p < std::min(port.packets.size(), e.packets.size()); ++p)
            {
                const std::vector<uint8_t> &x = port.packets[p], &y = e.packets[p];
                if (x.size() != y.size())
                {
                    std::snprintf(item, sizeof(item), " packet %zu size %zu/%zu", p, x.size(), y.size());
                    out += item;
                    break;
                }
                for (size_t i = 0; i < x.size(); i += 4)
                    if (std::memcmp(x.data() + i, y.data() + i, 4) != 0)
                    {
                        uint32_t wa, wb;
                        std::memcpy(&wa, x.data() + i, 4);
                        std::memcpy(&wb, y.data() + i, 4);
                        std::snprintf(item, sizeof(item), " first at packet %zu byte %zu: %08x/%08x", p, i, wa, wb);
                        out += item;
                        break;
                    }
            }
        }
        for (uint32_t q = 0; q < kMemory / 16u; ++q)
            if (std::memcmp(port.data.data() + q * 16u, e.dataAfter.data() + q * 16u, 16) != 0)
            {
                std::snprintf(item, sizeof(item), " memory from qword %#x", q);
                out += item;
                break;
            }
        const uint64_t cycles = e.stateAfter.cycles - e.stateBefore.cycles;
        if (port.cycles != cycles)
            out += " cycles " + std::to_string(port.cycles) + "/" + std::to_string(cycles);
        return out;
    }

    struct EntryStats
    {
        size_t runs = 0, mismatches = 0;
        double microseconds = 0.0, sandboxMicroseconds = 0.0;
    };

    double sandboxMicroseconds(const std::vector<uint8_t> &code, const Vu1TraceRecord &record)
    {
        static GS s_gs;
        static VU1Interpreter s_vu(VU1Interpreter::Unit::VU1);
        static std::vector<uint8_t> s_image;
        if (s_image != code)
        {
            s_image = code;
            s_image.resize(kMemory);
            s_vu.reset();
            s_vu.invalidateDecodedCode();
        }
        std::vector<uint8_t> data = record.dataBefore;
        s_vu.state() = record.stateBefore;
        Vu1Lifted::setClip(s_vu, record.stateBefore.clip);
        s_vu.setPacketsToObserverOnly(true);
        size_t packets = 0;
        s_vu.setPacketObserver([&packets](const uint8_t *, uint32_t) { ++packets; });
        const auto begin = std::chrono::steady_clock::now();
        s_vu.execute(s_image.data(), kMemory, data.data(), kMemory, s_gs, nullptr, record.startPC, record.top, record.itop, 1u << 24);
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
    }
}

void register_sc3_vu1_programs_tests()
{
    MiniTest::Case("Sc3Vu1Programs", [](TestCase &tc)
    {
        tc.Run("every ported entry leaves what the sandbox did on every recorded run", [](TestCase &t)
        {
            const char *paths = std::getenv("SC3_VU1_TRACE");
            if (!paths || !*paths)
                return;
            std::map<std::tuple<uint32_t, uint32_t, uint32_t>, EntryStats> stats;
            size_t total = 0, mismatches = 0, skipped = 0, resumed = 0;
            std::string list = paths;
            size_t start = 0;
            while (start <= list.size())
            {
                size_t end = list.find(',', start);
                if (end == std::string::npos)
                    end = list.size();
                const std::string path = list.substr(start, end - start);
                start = end + 1;
                if (path.empty())
                    continue;
                FILE *file = std::fopen(path.c_str(), "rb");
                t.IsNotNull(file, "the trace opens");
                if (!file)
                    continue;
                std::vector<uint8_t> code;
                uint32_t program = ~0u;
                Vu1TraceRecord record;
                while (readVu1TraceRecord(file, record))
                {
                    if (!record.code.empty())
                    {
                        code = record.code;
                        program = ~0u;
                        for (uint32_t p = 0; p < std::size(kKnown); ++p)
                            if (VU1Interpreter::codeHash(code.data(), kKnown[p].pairs * 8u) == kKnown[p].hash)
                                program = p;
                    }
                    if (record.resume)
                    {
                        ++resumed;
                        continue;
                    }
                    if (program == ~0u || !sc3::vu1Ported(program, record.startPC, record.dataBefore.data(), record.top))
                    {
                        ++skipped;
                        continue;
                    }
                    const uint32_t target = sc3::vu1DispatchTarget(program, record.startPC, record.dataBefore.data(), record.top);
                    EntryStats &entry = stats[{program, record.startPC, target}];
                    Run port;
                    port.state = record.stateBefore;
                    port.data = record.dataBefore;
                    const auto begin = std::chrono::steady_clock::now();
                    port.cycles = sc3::runVu1Program(
                        program, record.startPC, port.state, port.data.data(), record.top, record.itop,
                        [](void *user, const uint8_t *packet, uint32_t bytes)
                        { static_cast<std::vector<std::vector<uint8_t>> *>(user)->emplace_back(packet, packet + bytes); },
                        &port.packets);
                    entry.microseconds += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
                    port.state.cycles = record.stateAfter.cycles;
                    if (std::getenv("SC3_VU1_TRACE_TIME"))
                        entry.sandboxMicroseconds += sandboxMicroseconds(code, record);
                    ++entry.runs;
                    ++total;
                    const std::string diff = compare(port, record);
                    if (!diff.empty())
                    {
                        if (entry.mismatches++ < 3)
                            std::printf("  %c:%#x>%#x (TOP %#x):%s\n", 'A' + program, record.startPC, target, record.top, diff.c_str());
                        ++mismatches;
                    }
                }
                std::fclose(file);
            }
            for (const auto &[key, entry] : stats)
                std::printf("  %c:%#x>%#x: %zu runs, %zu differ, %.2f us a run (sandbox %.2f)\n", 'A' + std::get<0>(key), std::get<1>(key),
                            std::get<2>(key), entry.runs, entry.mismatches, entry.runs ? entry.microseconds / entry.runs : 0.0,
                            entry.runs ? entry.sandboxMicroseconds / entry.runs : 0.0);
            std::printf("  %zu runs ported, %zu differ; %zu not ported, %zu MSCNT\n", total, mismatches, skipped, resumed);
            t.Equals(mismatches, size_t{0}, "runs that differ");
        });

        tc.Run("every ported VU0 microprogram leaves what the interpreter did on every recorded run", [](TestCase &t)
        {
            const char *path = std::getenv("SC3_VU0_TRACE");
            if (!path || !*path)
                return;
            FILE *file = std::fopen(path, "rb");
            t.IsNotNull(file, "the trace opens");
            if (!file)
                return;
            constexpr uint32_t kVu0Memory = 4u * 1024u;
            std::map<std::pair<uint64_t, uint32_t>, EntryStats> stats;
            size_t total = 0, mismatches = 0, skipped = 0;
            uint64_t image = 0;
            Vu1TraceRecord record;
            while (readVu1TraceRecord(file, record))
            {
                if (!record.code.empty())
                    image = VU1Interpreter::codeHash(record.code.data(), kVu0Memory);
                if (!sc3::vu0Ported(image, record.startPC))
                {
                    ++skipped;
                    continue;
                }
                EntryStats &entry = stats[{image, record.startPC}];
                Run port;
                port.state = record.stateBefore;
                port.data = record.dataBefore;
                const auto begin = std::chrono::steady_clock::now();
                port.cycles = sc3::runVu0Program(image, record.startPC, port.state, port.data.data());
                entry.microseconds += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
                port.state.cycles = record.stateAfter.cycles;
                ++entry.runs;
                ++total;
                const std::string diff = compare(port, record);
                if (!diff.empty())
                {
                    if (entry.mismatches++ < 3)
                        std::printf("  VU0 %08llx:%#x:%s\n", static_cast<unsigned long long>(image >> 32), record.startPC, diff.c_str());
                    ++mismatches;
                }
            }
            std::fclose(file);
            for (const auto &[key, entry] : stats)
                std::printf("  VU0 %08llx:%#x: %zu runs, %zu differ, %.2f us a run\n", static_cast<unsigned long long>(key.first >> 32),
                            key.second, entry.runs, entry.mismatches, entry.runs ? entry.microseconds / entry.runs : 0.0);
            std::printf("  %zu VU0 runs ported, %zu differ; %zu not ported\n", total, mismatches, skipped);
            t.Equals(mismatches, size_t{0}, "VU0 runs that differ");
        });
    });
}
