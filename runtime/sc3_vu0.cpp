#include "sc3_native.h"
#include "sc3_vu1_port.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_trace.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sc3
{
    namespace vu0
    {
        constexpr uint32_t kVu0Memory = 4u * 1024u;

        enum class Mode
        {
            Interpreter,
            Port,
            Check,
        };

        struct Stats
        {
            std::map<std::pair<uint64_t, uint32_t>, uint64_t> calls, fallbacks;
            uint64_t runs = 0, ported = 0, checked = 0, mismatches = 0;
        };

        struct State
        {
            Mode mode = Mode::Port;
            PS2Runtime *runtime = nullptr;
            bool log = false;
            uint64_t generation = ~0ull;
            uint64_t image = 0;
            std::unique_ptr<VU1Interpreter> vu;
            std::unique_ptr<GS> gs;
            FILE *record = nullptr;
            uint64_t recordCap = 2000u;
            uint64_t recordedImage = 0;
            std::map<std::pair<uint64_t, uint32_t>, uint64_t> recordSeen, recordKept;
            Stats stats;
            uint32_t sinceCheck = 0;
            std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
        };

        State g_vu0;

        void resolveImage(const uint8_t *code)
        {
            State &s = g_vu0;
            const uint64_t generation = s.runtime->memory().getVU0CodeGeneration();
            if (generation == s.generation)
                return;
            s.generation = generation;
            s.image = VU1Interpreter::codeHash(code, kVu0Memory);
            if (s.vu)
                s.vu->invalidateDecodedCode();
        }

        uint64_t interpret(VU1State &state, uint8_t *code, uint8_t *data, uint32_t pc, std::vector<std::vector<uint8_t>> *packets)
        {
            State &s = g_vu0;
            if (!s.vu)
            {
                s.vu = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU0);
                s.gs = std::make_unique<GS>();
            }
            VU1Interpreter &vu = *s.vu;
            vu.reset();
            vu.state() = state;
            vu.state().cycles = 0;
            (void)packets;
            vu.execute(code, kVu0Memory, data, kVu0Memory, *s.gs, nullptr, pc, 0u, state.itop, 4096, true);
            const uint64_t cycles = vu.state().cycles;
            state = vu.state();
            return cycles;
        }

        bool sampleRecord(uint32_t pc)
        {
            State &s = g_vu0;
            if (!s.record)
                return false;
            const std::pair<uint64_t, uint32_t> key{s.image, pc};
            const uint64_t n = ++s.recordSeen[key];
            if (!(n <= 100u || (n <= 1000u && n % 10u == 0u) || n % 500u == 0u))
                return false;
            uint64_t &kept = s.recordKept[key];
            if (kept >= s.recordCap)
                return false;
            ++kept;
            return true;
        }

        void writeRecord(const VU1State &before, const std::vector<uint8_t> &dataBefore, const VU1State &after, const uint8_t *code,
                         const uint8_t *data, uint32_t pc, uint64_t cycles)
        {
            State &s = g_vu0;
            Vu1TraceRecord record;
            record.startPC = pc;
            record.itop = before.itop;
            record.maxCycles = 4096u;
            if (s.recordedImage != s.image)
            {
                record.code.assign(Vu1TraceRecord::kMemorySize, 0u);
                std::memcpy(record.code.data(), code, kVu0Memory);
                s.recordedImage = s.image;
            }
            record.stateBefore = before;
            record.stateBefore.cycles = 0;
            record.dataBefore.assign(Vu1TraceRecord::kMemorySize, 0u);
            std::memcpy(record.dataBefore.data(), dataBefore.data(), kVu0Memory);
            record.stateAfter = after;
            record.stateAfter.cycles = cycles;
            record.dataAfter.assign(Vu1TraceRecord::kMemorySize, 0u);
            std::memcpy(record.dataAfter.data(), data, kVu0Memory);
            writeVu1TraceRecord(s.record, record);
        }

        std::string differences(const VU1State &a, const VU1State &b)
        {
            std::string out;
            for (int r = 0; r < 32; ++r)
                if (std::memcmp(a.vf[r], b.vf[r], 16) != 0)
                    out += " vf" + std::to_string(r);
            for (int r = 1; r < 16; ++r)
                if (a.vi[r] != b.vi[r])
                    out += " vi" + std::to_string(r);
            if (std::memcmp(a.acc, b.acc, 16) != 0)
                out += " acc";
            if (std::memcmp(&a.q, &b.q, 4) != 0 || std::memcmp(&a.i, &b.i, 4) != 0 || std::memcmp(&a.p, &b.p, 4) != 0)
                out += " q/i/p";
            char item[128];
            if (a.clip != b.clip || a.mac != b.mac || a.status != b.status || a.pc != b.pc)
            {
                std::snprintf(item, sizeof(item), " clip %#x/%#x mac %#x/%#x status %#x/%#x pc %#x/%#x", a.clip, b.clip, a.mac, b.mac, a.status,
                              b.status, a.pc, b.pc);
                out += item;
            }
            return out;
        }

        void report()
        {
            State &s = g_vu0;
            if (++s.sinceCheck < 4096u)
                return;
            s.sinceCheck = 0;
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - s.since).count();
            if (seconds < 5.0)
                return;
            Stats &st = s.stats;
            if (s.log)
            {
                std::string line;
                for (const auto &[key, count] : st.calls)
                {
                    char item[64];
                    std::snprintf(item, sizeof(item), " %08llx:%#x x%llu", static_cast<unsigned long long>(key.first >> 32), key.second,
                                  static_cast<unsigned long long>(count));
                    line += item;
                }
                std::fprintf(stderr, "[sc3:vu0] %.1f s: %llu runs, port %llu (checked %llu, mismatches %llu);%s\n", seconds,
                             static_cast<unsigned long long>(st.runs), static_cast<unsigned long long>(st.ported),
                             static_cast<unsigned long long>(st.checked), static_cast<unsigned long long>(st.mismatches), line.c_str());
            }
            if (!st.fallbacks.empty())
            {
                std::string line;
                uint64_t total = 0;
                for (const auto &[key, count] : st.fallbacks)
                {
                    char item[64];
                    std::snprintf(item, sizeof(item), " %08llx:%#x x%llu", static_cast<unsigned long long>(key.first >> 32), key.second,
                                  static_cast<unsigned long long>(count));
                    line += item;
                    total += count;
                }
                std::fprintf(stderr, "[sc3:vu0] %.1f s: %llu microprograms on the interpreter:%s\n", seconds,
                             static_cast<unsigned long long>(total), line.c_str());
            }
            if (s.record)
                std::fflush(s.record);
            st = Stats{};
            s.since = now;
        }

        bool runMicroprogram(VU1State &state, uint8_t *code, uint8_t *data, uint32_t pc)
        {
            State &s = g_vu0;
            resolveImage(code);
            ++s.stats.runs;
            if (s.log)
                ++s.stats.calls[{s.image, pc}];
            const bool ported = s.mode != Mode::Interpreter && vu0Ported(s.image, pc);
            if (!ported)
            {
                ++s.stats.fallbacks[{s.image, pc}];
                if (sampleRecord(pc))
                {
                    const VU1State before = state;
                    const std::vector<uint8_t> dataBefore(data, data + kVu0Memory);
                    const uint64_t cycles = interpret(state, code, data, pc, nullptr);
                    writeRecord(before, dataBefore, state, code, data, pc, cycles);
                    report();
                    return true;
                }
                report();
                return false;
            }
            ++s.stats.ported;
            if (s.mode == Mode::Check)
            {
                static std::vector<uint8_t> s_before(kVu0Memory), s_portData(kVu0Memory);
                std::memcpy(s_before.data(), data, kVu0Memory);
                const VU1State before = state;
                VU1State port = state;
                const uint64_t portCycles = runVu0Program(s.image, pc, port, data);
                std::memcpy(s_portData.data(), data, kVu0Memory);
                std::memcpy(data, s_before.data(), kVu0Memory);
                const uint64_t cycles = interpret(state, code, data, pc, nullptr);
                ++s.stats.checked;
                std::string difference = differences(port, state);
                if (std::memcmp(s_portData.data(), data, kVu0Memory) != 0)
                    difference += " memory";
                if (portCycles != cycles)
                    difference += " cycles " + std::to_string(portCycles) + "/" + std::to_string(cycles);
                if (!difference.empty())
                {
                    ++s.stats.mismatches;
                    static int s_logged = 0;
                    if (s_logged++ < 50)
                        std::fprintf(stderr, "[sc3:vu0] port %08llx:%#x differs from the interpreter:%s\n",
                                     static_cast<unsigned long long>(s.image >> 32), pc, difference.c_str());
                    if (s.record)
                    {
                        s.recordedImage = 0;
                        writeRecord(before, s_before, state, code, data, pc, cycles);
                    }
                }
                report();
                return true;
            }
            runVu0Program(s.image, pc, state, data);
            report();
            return true;
        }
    }

    void installNativeVu0(PS2Runtime &runtime)
    {
        vu0::State &s = vu0::g_vu0;
        const char *mode = std::getenv("SC3_NATIVE_VU0");
        if (mode && std::strcmp(mode, "check") == 0)
            s.mode = vu0::Mode::Check;
        else if (mode && *mode == '0')
            s.mode = vu0::Mode::Interpreter;
        const char *log = std::getenv("SC3_NATIVE_VU0_LOG");
        s.log = log && *log == '1';
        if (const char *path = std::getenv("SC3_NATIVE_VU0_RECORD"); path && *path)
        {
            s.record = std::fopen(path, "wb");
            if (const char *cap = std::getenv("SC3_NATIVE_VU0_RECORD_CAP"); cap && *cap)
                s.recordCap = std::strtoull(cap, nullptr, 0);
        }
        s.runtime = &runtime;
        runtime.setVu0MicroHook(vu0::runMicroprogram);
        std::cerr << "[sc3] VU0 microprograms: "
                  << (s.mode == vu0::Mode::Interpreter ? "on the interpreter" : s.mode == vu0::Mode::Check ? "C++ ports checked against the interpreter" : "C++ ports")
                  << (s.record ? ", sampled runs recorded" : "") << std::endl;
    }
}
