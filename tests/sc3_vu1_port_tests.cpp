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
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kMemory = 16u * 1024u;
    constexpr uint64_t kProgramBHash = 0xd2fbf7af837a8df4ull;
    constexpr uint32_t kProgramBPairs = 1352u;

    struct Result
    {
        VU1State state{};
        std::vector<uint8_t> data;
        std::vector<std::vector<uint8_t>> packets;
        uint64_t cycles = 0;
    };

    struct Input
    {
        VU1State state{};
        std::vector<uint8_t> data = std::vector<uint8_t>(kMemory);
        uint32_t top = 0, itop = 0;
    };

    void putWords(std::vector<uint8_t> &data, uint32_t qword, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
    {
        const uint32_t words[4] = {x, y, z, w};
        std::memcpy(data.data() + qword * 16u, words, 16);
    }

    uint32_t bitsOf(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        return bits;
    }

    void putFloats(std::vector<uint8_t> &data, uint32_t qword, float x, float y, float z, float w)
    {
        putWords(data, qword, bitsOf(x), bitsOf(y), bitsOf(z), bitsOf(w));
    }

    void setVf(VU1State &state, int reg, float x, float y, float z, float w)
    {
        state.vf[reg][0] = x, state.vf[reg][1] = y, state.vf[reg][2] = z, state.vf[reg][3] = w;
    }

    Input synthetic(uint32_t kickQwords)
    {
        const bool pendingKick = kickQwords != 0u;
        Input in;
        in.top = 0x20u;
        in.itop = 0x55u;
        const float bones[3][4][4] = {
            {{0.98f, 0.17f, 0.0f, 0.0f}, {-0.17f, 0.98f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}, {1.5f, -2.25f, 0.5f, 1.0f}},
            {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.87f, 0.5f, 0.0f}, {0.0f, -0.5f, 0.87f, 0.0f}, {-0.75f, 3.0f, 1.25f, 1.0f}},
            {{0.71f, 0.0f, -0.71f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.71f, 0.0f, 0.71f, 0.0f}, {0.0f, 0.5f, -4.0f, 1.0f}},
        };
        for (uint32_t b = 0; b < 3; ++b)
            for (uint32_t c = 0; c < 4; ++c)
                putFloats(in.data, b * 4u + c, bones[b][c][0], bones[b][c][1], bones[b][c][2], bones[b][c][3]);
        struct Influence
        {
            uint32_t bone;
            float weight;
        };
        struct Vertex
        {
            float x, y, z, nx, ny, nz, s, t;
            uint32_t flags;
            float r, g, b, a;
            std::vector<Influence> influences;
        };
        const Vertex vertices[] = {
            {1.0f, 2.0f, 3.0f, 0.0f, 0.0f, 1.0f, 0.25f, 0.5f, 0u, 128.0f, 96.0f, 64.0f, 128.0f, {{0, 1.0f}}},
            {-1.5f, 0.5f, 2.0f, 0.6f, 0.0f, 0.8f, 0.75f, 0.125f, 0u, 200.0f, 180.0f, 160.0f, 128.0f, {{0, 0.25f}, {4, 0.75f}}},
            {0.0f, -3.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.5f, 0.5f, 0x8000u, 16.0f, 255.0f, 32.0f, 64.0f, {{4, 0.5f}, {8, 0.3f}, {0, 0.2f}}},
            {900.0f, 4.0f, -2.0f, -0.8f, 0.6f, 0.0f, 1.0f, 1.0f, 0u, 255.0f, 255.0f, 255.0f, 128.0f, {{8, 1.0f}}},
            {2.5f, 1.25f, -0.5f, 0.0f, 0.71f, 0.71f, 0.0f, 0.875f, 0u, 90.0f, 45.0f, 30.0f, 128.0f, {{4, 0.6f}, {8, 0.4f}}},
        };
        const uint32_t count = static_cast<uint32_t>(std::size(vertices));
        putWords(in.data, in.top, 0x8000u | count, 0x302e4000u, 0x412u, 0u);
        uint32_t at = in.top + 1u;
        for (const Vertex &v : vertices)
        {
            for (size_t k = 0; k < v.influences.size(); ++k)
            {
                const Influence &i = v.influences[k];
                putFloats(in.data, at++, v.x * i.weight, v.y * i.weight, v.z * i.weight, i.weight);
                putWords(in.data, at++, bitsOf(v.nx * i.weight), bitsOf(v.ny * i.weight), bitsOf(v.nz * i.weight),
                         i.bone | (k + 1 == v.influences.size() ? 0x8000u : 0u));
            }
            putWords(in.data, at++, bitsOf(v.s), bitsOf(v.t), bitsOf(1.0f), v.flags);
            putFloats(in.data, at++, v.r, v.g, v.b, v.a);
        }
        if (pendingKick)
        {
            putWords(in.data, in.top + 0x1b0u, 0x8000u | (kickQwords - 1u), 0x10000000u, 0x0eu, 0u);
            for (uint32_t q = 1; q < kickQwords; ++q)
                putWords(in.data, in.top + 0x1b0u + q, 0x5000du + q, 0u, 0x47u, 0u);
        }

        VU1State &s = in.state;
        setVf(s, 0, 0.0f, 0.0f, 0.0f, 1.0f);
        for (int r = 1; r < 14; ++r)
            setVf(s, r, 0.5f * r, -0.25f * r, 3.0f + r, 1.0f + 0.125f * r);
        setVf(s, 15, 1.0f / 256.0f, 1.0f / 128.0f, 0.0f, 0.0f);
        setVf(s, 16, 0.0f, 0.0f, -0.25f, 0.5f);
        setVf(s, 17, 255.0f, 127.0f, 96.0f, 0.75f);
        setVf(s, 18, -2048.0f, -2048.0f, -32768.0f, 0.0f);
        setVf(s, 19, 1.0f / 1024.0f, 1.0f / 1024.0f, 1.0f / 32768.0f, 1.0f);
        setVf(s, 20, 0.3f, 0.3f, 0.35f, 0.0f);
        setVf(s, 21, 0.6f, 0.55f, 0.5f, 0.0f);
        setVf(s, 22, 0.1f, 0.1f, 0.2f, 0.0f);
        setVf(s, 23, 0.05f, 0.2f, 0.05f, 0.0f);
        setVf(s, 24, 0.0f, 0.0f, 0.0f, 128.0f);
        setVf(s, 25, 0.0f, 0.71f, -0.71f, 0.0f);
        setVf(s, 26, 0.5f, 0.0f, 0.0f, 0.3f);
        setVf(s, 27, -0.71f, 0.71f, 0.0f, 0.2f);
        setVf(s, 28, 320.0f, 0.0f, 0.0f, 0.0f);
        setVf(s, 29, 0.0f, -224.0f, 0.0f, 0.0f);
        setVf(s, 30, 2048.0f, 2048.0f, 8000.0f, 1.0f);
        setVf(s, 31, 2048.0f * 6.0f, 2048.0f * 6.0f, 65535.0f, 6.0f);
        for (int r = 0; r < 4; ++r)
            s.acc[r] = 7.0f + r;
        s.vi[1] = 0x1234, s.vi[3] = 0xff0, s.vi[4] = 9, s.vi[6] = pendingKick ? 0x1b0 : 0, s.vi[12] = -0x8000;
        s.vi[5] = 0x55, s.vi[7] = 0x7, s.vi[8] = 0x1;
        s.q = 0.5f;
        s.i = 3.0f;
        s.p = 2.0f;
        s.clip = 0x3c2d1eu;
        s.mac = 0x10u;
        s.status = 0x0c2u;
        s.pc = 0x15e8u;
        return in;
    }

    std::vector<uint8_t> loadFile(const char *path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
    }

    Result runPort(const Input &in, bool fast = true)
    {
        Result r;
        r.state = in.state;
        r.data = in.data;
        r.cycles = sc3::runSkinnedBody(
            r.state, r.data.data(), in.top, in.itop,
            [](void *user, const uint8_t *packet, uint32_t bytes)
            { static_cast<std::vector<std::vector<uint8_t>> *>(user)->emplace_back(packet, packet + bytes); },
            &r.packets, fast);
        return r;
    }

    Result runVu(const std::vector<uint8_t> &code, const Input &in)
    {
        static GS s_gs;
        static VU1Interpreter s_vu(VU1Interpreter::Unit::VU1);
        static std::vector<uint8_t> s_image;
        if (s_image != code)
        {
            s_image = code;
            s_vu.reset();
        }
        VU1Interpreter &vu = s_vu;
        Result r;
        r.data = in.data;
        vu.state() = in.state;
        Vu1Lifted::setClip(vu, in.state.clip);
        vu.setPacketsToObserverOnly(true);
        vu.setPacketObserver([&r](const uint8_t *packet, uint32_t bytes) { r.packets.emplace_back(packet, packet + bytes); });
        const uint64_t before = Vu1Lifted::cycle(vu);
        vu.execute(s_image.data(), kMemory, r.data.data(), kMemory, s_gs, nullptr, sc3::kSkinnedBodyPc, in.top, in.itop, 1u << 24);
        r.state = vu.state();
        r.cycles = Vu1Lifted::cycle(vu) - before;
        return r;
    }

    std::string difference(const Result &port, const VU1State &state, const std::vector<uint8_t> &data,
                           const std::vector<std::vector<uint8_t>> &packets, uint64_t cycles)
    {
        std::string out;
        char item[128];
        for (int r = 0; r < 32; ++r)
            if (std::memcmp(port.state.vf[r], state.vf[r], 16) != 0)
            {
                std::snprintf(item, sizeof(item), " vf%d (%08x %08x %08x %08x / %08x %08x %08x %08x)", r, bitsOf(port.state.vf[r][0]),
                              bitsOf(port.state.vf[r][1]), bitsOf(port.state.vf[r][2]), bitsOf(port.state.vf[r][3]), bitsOf(state.vf[r][0]),
                              bitsOf(state.vf[r][1]), bitsOf(state.vf[r][2]), bitsOf(state.vf[r][3]));
                out += item;
            }
        for (int r = 1; r < 16; ++r)
            if (port.state.vi[r] != state.vi[r])
            {
                std::snprintf(item, sizeof(item), " vi%d %#x/%#x", r, port.state.vi[r], state.vi[r]);
                out += item;
            }
        if (std::memcmp(port.state.acc, state.acc, 16) != 0)
            out += " acc";
        if (bitsOf(port.state.q) != bitsOf(state.q) || bitsOf(port.state.i) != bitsOf(state.i) || bitsOf(port.state.p) != bitsOf(state.p))
            out += " q/i/p";
        if (port.state.clip != state.clip || port.state.mac != state.mac || port.state.status != state.status || port.state.pc != state.pc)
        {
            std::snprintf(item, sizeof(item), " clip %#x/%#x mac %#x/%#x status %#x/%#x pc %#x/%#x", port.state.clip, state.clip,
                          port.state.mac, state.mac, port.state.status, state.status, port.state.pc, state.pc);
            out += item;
        }
        if (port.packets != packets)
        {
            std::snprintf(item, sizeof(item), " packets (%zu / %zu)", port.packets.size(), packets.size());
            out += item;
            for (size_t p = 0; p < std::min(port.packets.size(), packets.size()); ++p)
            {
                const std::vector<uint8_t> &a = port.packets[p], &b = packets[p];
                for (size_t i = 0; i < std::min(a.size(), b.size()); i += 4)
                    if (std::memcmp(a.data() + i, b.data() + i, 4) != 0)
                    {
                        uint32_t wa, wb;
                        std::memcpy(&wa, a.data() + i, 4);
                        std::memcpy(&wb, b.data() + i, 4);
                        std::snprintf(item, sizeof(item), " first at packet %zu byte %zu: %08x/%08x", p, i, wa, wb);
                        out += item;
                        break;
                    }
            }
        }
        for (uint32_t q = 0; q < kMemory / 16u; ++q)
            if (std::memcmp(port.data.data() + q * 16u, data.data() + q * 16u, 16) != 0)
            {
                std::snprintf(item, sizeof(item), " memory from qword %#x", q);
                out += item;
                break;
            }
        if (port.cycles != cycles)
            out += " cycles " + std::to_string(port.cycles) + "/" + std::to_string(cycles);
        return out;
    }

    uint64_t digest(const Result &r)
    {
        uint64_t hash = 1469598103934665603ull;
        const auto mix = [&hash](const void *data, size_t bytes)
        {
            const uint8_t *p = static_cast<const uint8_t *>(data);
            for (size_t i = 0; i < bytes; ++i)
                hash = (hash ^ p[i]) * 1099511628211ull;
        };
        for (const std::vector<uint8_t> &packet : r.packets)
        {
            const uint64_t size = packet.size();
            mix(&size, sizeof(size));
            mix(packet.data(), packet.size());
        }
        mix(r.data.data(), r.data.size());
        mix(r.state.vf, sizeof(r.state.vf));
        mix(r.state.vi, sizeof(r.state.vi));
        mix(r.state.acc, sizeof(r.state.acc));
        const uint32_t scalars[] = {bitsOf(r.state.q), bitsOf(r.state.i), bitsOf(r.state.p), r.state.clip, r.state.mac, r.state.status, r.state.pc};
        mix(scalars, sizeof(scalars));
        mix(&r.cycles, sizeof(r.cycles));
        return hash;
    }

    std::vector<uint8_t> programB()
    {
        const char *path = std::getenv("SC3_VU1_CODE");
        if (!path || !*path)
            return {};
        std::vector<uint8_t> code = loadFile(path);
        code.resize(kMemory);
        if (VU1Interpreter::codeHash(code.data(), kProgramBPairs * 8u) != kProgramBHash)
            return {};
        return code;
    }

    constexpr uint64_t kSyntheticDigest = 0xbe047bc8c88e4430ull;
    constexpr uint64_t kSyntheticKickDigest = 0x9c79f7af5a8df4b4ull;
    constexpr uint64_t kSyntheticLongKickDigest = 0x6490a05c27bbd06ull;
}

void register_sc3_vu1_port_tests()
{
    MiniTest::Case("Sc3Vu1Port", [](TestCase &tc)
    {
        tc.Run("B:0x1388 leaves the packets, memory, registers and flags the interpreter does on a synthetic batch", [](TestCase &t)
        {
            const uint32_t kicks[] = {0u, 2u, 9u};
            for (const uint32_t kickQwords : kicks)
            {
                const Input in = synthetic(kickQwords);
                const Result port = runPort(in);
                const Result careful = runPort(in, false);
                t.IsTrue(difference(port, careful.state, careful.data, careful.packets, careful.cycles).empty(),
                         "the fast path differs from the careful one");
                std::printf("  synthetic, pending kick of %u qwords: digest %#llx, %zu packets, %llu cycles\n", kickQwords,
                            static_cast<unsigned long long>(digest(port)), port.packets.size(),
                            static_cast<unsigned long long>(port.cycles));
                const std::vector<uint8_t> code = programB();
                if (!code.empty())
                {
                    const Result vu = runVu(code, in);
                    const std::string diff = difference(port, vu.state, vu.data, vu.packets, vu.cycles);
                    std::printf("  interpreter digest %#llx%s\n", static_cast<unsigned long long>(digest(vu)), diff.c_str());
                    t.IsTrue(diff.empty(), "the port differs from the interpreter:" + diff);
                }
                t.Equals(digest(port), kickQwords == 0u ? kSyntheticDigest : kickQwords == 2u ? kSyntheticKickDigest : kSyntheticLongKickDigest,
                         "the digest of the interpreter's run");
            }
        });

        tc.Run("B:0x1388 leaves what VU1 did on every recorded run", [](TestCase &t)
        {
            const char *path = std::getenv("SC3_VU1_PORT_TRACE");
            if (!path || !*path)
                return;
            FILE *file = std::fopen(path, "rb");
            t.IsNotNull(file, "the trace opens");
            if (!file)
                return;
            std::vector<uint8_t> code;
            std::vector<Input> inputs;
            std::vector<Vu1TraceRecord> expected;
            Vu1TraceRecord record;
            while (readVu1TraceRecord(file, record))
            {
                if (!record.code.empty())
                    code = record.code;
                if (record.resume || record.startPC != sc3::kSkinnedBodyPc || code.empty() ||
                    VU1Interpreter::codeHash(code.data(), kProgramBPairs * 8u) != kProgramBHash)
                    continue;
                Input in;
                in.state = record.stateBefore;
                in.data = record.dataBefore;
                in.top = record.top;
                in.itop = record.itop;
                inputs.push_back(std::move(in));
                record.code.clear();
                expected.push_back(std::move(record));
                record = {};
            }
            std::fclose(file);
            size_t mismatches = 0;
            for (const bool fast : {true, false})
                for (size_t i = 0; i < inputs.size(); ++i)
                {
                    const Result port = runPort(inputs[i], fast);
                    const Vu1TraceRecord &e = expected[i];
                    const std::string diff = difference(port, e.stateAfter, e.dataAfter, e.packets, e.stateAfter.cycles - e.stateBefore.cycles);
                    if (!diff.empty() && mismatches++ < 10)
                        std::printf("  %s run %zu (TOP %#x):%s\n", fast ? "fast" : "careful", i, inputs[i].top, diff.c_str());
                }
            const auto time = [&](auto &&run)
            {
                const auto start = std::chrono::steady_clock::now();
                for (int pass = 0; pass < 5; ++pass)
                    for (const Input &in : inputs)
                        run(in);
                return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / (5.0 * inputs.size());
            };
            const double copyUs = time([](const Input &in) { Result r; r.state = in.state; r.data = in.data; return r.data.size(); });
            const double portUs = time([](const Input &in) { runPort(in); });
            const double carefulUs = time([](const Input &in) { runPort(in, false); });
            const double vuUs = time([&code](const Input &in) { runVu(code, in); });
            std::printf("  %zu runs, %zu differ; per run: port %.2f us (careful path %.2f), interpreter %.2f us, of which copying the input %.2f\n",
                        inputs.size(), mismatches, portUs, carefulUs, vuUs, copyUs);
            t.IsTrue(!inputs.empty(), "the trace holds runs of B:0x1388");
            t.Equals(mismatches, size_t{0}, "runs that differ");
        });
    });
}

void register_sc3_vu1_programs_tests();

int main()
{
    register_sc3_vu1_port_tests();
    register_sc3_vu1_programs_tests();
    const int result = MiniTest::Run();
    std::cout.flush();
    std::_Exit(result);
}
