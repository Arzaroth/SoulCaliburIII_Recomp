#include "sc3_native.h"

#include "ps2_runtime.h"
#include "runtime/native/native_vu1_stream.h"
#include "runtime/ps2_vu1.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sc3
{
    namespace
    {
        using ps2x::native::Vu1StreamAdapter;

        constexpr uint32_t kElapsedLines = 0x00148098u;
        constexpr uint32_t kFrameEndReturn = 0x0012EA90u;
        constexpr uint32_t kPlayerHealth = 0x004D36A0u;
        constexpr uint32_t kOpponentHealth = 0x004D36A4u;
        constexpr float kFullHealth = 240.0f;
        constexpr size_t kCodeBytes = 16u * 1024u;

        struct KnownProgram
        {
            const char *name;
            uint32_t pairs;
            uint64_t hash;
        };

        constexpr KnownProgram kPrograms[] = {
            {"A", 1177u, 0x95e81f9a833c1b45ull},
            {"B", 1352u, 0xd2fbf7af837a8df4ull},
        };

        enum class Mode
        {
            Observe,
            Sandbox,
        };

        struct ProgramSet
        {
            std::set<std::pair<std::string, uint32_t>> entries;
            std::set<std::string> whole;

            bool empty() const { return entries.empty() && whole.empty(); }
            bool contains(const std::string &name, uint32_t pc) const
            {
                return whole.count(name) != 0u || entries.count({name, pc}) != 0u;
            }
        };

        struct Stats
        {
            std::unordered_map<uint64_t, uint64_t> calls;
            uint64_t frames = 0, mscals = 0, codeChanges = 0, sandboxed = 0, hidden = 0;
            uint64_t frameMin = ~0ull, frameMax = 0, frameCalls = 0;
        };

        struct State
        {
            Mode mode = Mode::Observe;
            bool sandbox = false;
            std::string switchPath;
            uint8_t *rdram = nullptr;
            PS2Runtime::RecompiledFunction elapsedLines = nullptr;
            uint64_t codeVersion = ~0ull;
            uint32_t program = 0;
            std::vector<std::string> names;
            std::vector<std::vector<uint8_t>> images;
            ProgramSet hidden, only;
            std::string hidePath, hideSpec;
            bool log = false;
            Stats stats;
            std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
        };

        State g_state;

        uint64_t key(uint32_t program, uint32_t pc) { return (static_cast<uint64_t>(program) << 32) | pc; }

        uint32_t residentProgram(const Vu1StreamAdapter &adapter)
        {
            State &s = g_state;
            if (adapter.codeVersion() == s.codeVersion)
                return s.program;
            s.codeVersion = adapter.codeVersion();
            ++s.stats.codeChanges;
            const uint8_t *code = adapter.code();
            for (uint32_t i = 0; i < std::size(kPrograms); ++i)
            {
                const KnownProgram &known = kPrograms[i];
                const size_t bytes = size_t{known.pairs} * 8u;
                std::vector<uint8_t> &image = s.images[i];
                const bool same = image.empty() ? VU1Interpreter::codeHash(code, static_cast<uint32_t>(bytes)) == known.hash
                                                : std::memcmp(code, image.data(), bytes) == 0;
                if (same)
                {
                    if (image.empty())
                        image.assign(code, code + bytes);
                    return s.program = i;
                }
            }
            char name[24];
            std::snprintf(name, sizeof(name), "%016llx",
                          static_cast<unsigned long long>(VU1Interpreter::codeHash(code, static_cast<uint32_t>(kCodeBytes))));
            const auto found = std::find(s.names.begin(), s.names.end(), name);
            if (found != s.names.end())
                return s.program = static_cast<uint32_t>(found - s.names.begin());
            s.names.emplace_back(name);
            return s.program = static_cast<uint32_t>(s.names.size() - 1u);
        }

        bool inFight()
        {
            float player = 0.0f, opponent = 0.0f;
            std::memcpy(&player, g_state.rdram + kPlayerHealth, sizeof(player));
            std::memcpy(&opponent, g_state.rdram + kOpponentHealth, sizeof(opponent));
            auto alive = [](float health) { return health >= 0.5f && health <= kFullHealth; };
            return alive(player) && alive(opponent);
        }

        Vu1StreamAdapter::Choice choose(Vu1StreamAdapter &adapter, uint32_t pc)
        {
            State &s = g_state;
            const uint32_t program = residentProgram(adapter);
            ++s.stats.mscals;
            ++s.stats.frameCalls;
            if (s.log)
                ++s.stats.calls[key(program, pc)];
            if (!s.hidden.empty() && s.hidden.contains(s.names[program], pc))
            {
                ++s.stats.hidden;
                return Vu1StreamAdapter::Choice::Port;
            }
            if (s.sandbox && (s.only.empty() || s.only.contains(s.names[program], pc)))
            {
                ++s.stats.sandboxed;
                return Vu1StreamAdapter::Choice::Sandbox;
            }
            return Vu1StreamAdapter::Choice::Vu1;
        }

        void report()
        {
            State &s = g_state;
            Stats &st = s.stats;
            ++st.frames;
            st.frameMin = std::min(st.frameMin, st.frameCalls);
            st.frameMax = std::max(st.frameMax, st.frameCalls);
            st.frameCalls = 0;
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - s.since).count();
            if (seconds < 5.0)
                return;
            if (s.log)
            {
                std::vector<std::pair<uint64_t, uint64_t>> calls(st.calls.begin(), st.calls.end());
                std::sort(calls.begin(), calls.end());
                std::string line;
                for (const auto &[programPc, count] : calls)
                {
                    char item[64];
                    std::snprintf(item, sizeof(item), " %s:%#x x%llu", s.names[programPc >> 32].c_str(),
                                  static_cast<uint32_t>(programPc), static_cast<unsigned long long>(count));
                    line += item;
                }
                std::fprintf(stderr,
                             "[sc3:vu1] %.1f s: %llu frames, mscal=%llu (per frame %.1f, %llu-%llu), code changes %llu, sandbox %llu, "
                             "hidden %llu;%s\n",
                             seconds, static_cast<unsigned long long>(st.frames), static_cast<unsigned long long>(st.mscals),
                             st.frames ? static_cast<double>(st.mscals) / static_cast<double>(st.frames) : 0.0,
                             static_cast<unsigned long long>(st.frameMin), static_cast<unsigned long long>(st.frameMax),
                             static_cast<unsigned long long>(st.codeChanges), static_cast<unsigned long long>(st.sandboxed),
                             static_cast<unsigned long long>(st.hidden), line.c_str());
            }
            st = Stats{};
            s.since = now;
        }

        ProgramSet parsePrograms(const std::string &text)
        {
            ProgramSet set;
            size_t start = 0;
            while (start < text.size())
            {
                size_t end = text.find(',', start);
                if (end == std::string::npos)
                    end = text.size();
                const std::string item = text.substr(start, end - start);
                start = end + 1;
                if (item.empty())
                    continue;
                const size_t colon = item.find(':');
                const std::string name = item.substr(0, colon);
                const std::string pc = colon == std::string::npos ? "*" : item.substr(colon + 1);
                if (pc == "*")
                    set.whole.insert(name);
                else
                    set.entries.insert({name, static_cast<uint32_t>(std::strtoul(pc.c_str(), nullptr, 16))});
            }
            return set;
        }

        void followHideFile()
        {
            State &s = g_state;
            if (s.hidePath.empty())
                return;
            std::ifstream file(s.hidePath);
            std::string spec;
            std::getline(file, spec);
            if (spec == s.hideSpec)
                return;
            s.hideSpec = spec;
            s.hidden = parsePrograms(spec);
            std::cerr << "[sc3] native VU1: hiding '" << spec << "' from the next frame" << std::endl;
        }

        void followSwitch()
        {
            State &s = g_state;
            if (s.switchPath.empty())
                return;
            std::ifstream file(s.switchPath);
            char value = 0;
            if (!(file >> value))
                return;
            const bool sandbox = value == '1';
            if (sandbox != s.sandbox)
                std::cerr << "[sc3] native VU1: " << (sandbox ? "sandbox" : "VU1") << " from the next frame" << std::endl;
            s.sandbox = sandbox;
        }

        void frameEndElapsedLines(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (getRegU32(ctx, 31) == kFrameEndReturn)
            {
                followSwitch();
                followHideFile();
                report();
                Vu1StreamAdapter::instance().frameEnd();
            }
            g_state.elapsedLines(rdram, ctx, runtime);
        }

        ps2x::native::Screen screen()
        {
            ps2x::native::Screen screen;
            screen.width = 640.0f;
            screen.height = 448.0f;
            screen.originX = 2048.0f - screen.width / 2.0f;
            screen.originY = 2048.0f - screen.height / 2.0f;
            return screen;
        }
    }

    void installNativeVu1(PS2Runtime &runtime, bool nativeGs)
    {
        const char *value = std::getenv("SC3_NATIVE_VU1");
        const char *hide = std::getenv("SC3_NATIVE_VU1_HIDE");
        const bool on = value && *value && *value != '0';
        if (!on && !(hide && *hide))
            return;
        State &s = g_state;
        s.mode = value && std::strcmp(value, "1") == 0 ? Mode::Sandbox : Mode::Observe;
        s.sandbox = s.mode == Mode::Sandbox;
        if (const char *path = std::getenv("SC3_NATIVE_VU1_SWITCH"); path && *path && s.mode == Mode::Sandbox)
        {
            s.switchPath = path;
            followSwitch();
        }
        s.rdram = runtime.memory().getRDRAM();
        for (const KnownProgram &known : kPrograms)
            s.names.emplace_back(known.name);
        s.images.resize(std::size(kPrograms));
        if (hide && *hide == '@')
        {
            s.hidePath = hide + 1;
            followHideFile();
        }
        else if (hide)
            s.hidden = parsePrograms(hide);
        if (const char *only = std::getenv("SC3_NATIVE_VU1_ONLY"); only && *only)
            s.only = parsePrograms(only);
        const char *log = std::getenv("SC3_NATIVE_VU1_LOG");
        s.log = (log && *log == '1') || s.mode == Mode::Observe;
        s.elapsedLines = runtime.lookupFunction(kElapsedLines);
        if (!s.elapsedLines)
        {
            std::cerr << "[sc3] SC3_NATIVE_VU1: no function at 0x148098, the native frame would never end" << std::endl;
            return;
        }
        runtime.registerFunction(kElapsedLines, frameEndElapsedLines);
        Vu1StreamAdapter::Config config;
        config.screen = screen();
        config.choose = choose;
        config.draw = [](Vu1StreamAdapter &, uint32_t, bool) {};
        config.directs = s.mode == Mode::Sandbox;
        const char *gsWrites = std::getenv("SC3_NATIVE_VU1_GSWRITES");
        config.gsWrites = nativeGs && !(gsWrites && *gsWrites == '0');
        if (s.mode == Mode::Sandbox)
            config.active = [] { return g_state.sandbox && inFight(); };
        Vu1StreamAdapter::instance().install(config);
        std::cerr << "[sc3] native VU1: "
                  << (s.mode == Mode::Sandbox ? "every program on the sandbox in fights" : "observed, every program on VU1")
                  << (s.hidden.empty() && s.hidePath.empty() ? "" : ", some hidden")
                  << (s.only.empty() ? "" : ", only some on the sandbox")
                  << (config.gsWrites ? ", drawn by the GS back end" : ", drawn by the native renderer") << std::endl;
    }
}
