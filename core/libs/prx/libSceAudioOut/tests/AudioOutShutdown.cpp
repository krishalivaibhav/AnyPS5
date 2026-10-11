#include "SceTypes.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

extern "C" {
int APS5_VABI sceAudioOutInit();
int APS5_VABI sceAudioOutOpen(int, int, int, std::uint32_t, std::uint32_t, std::uint32_t);
int APS5_VABI sceAudioOutClose(int);
int APS5_VABI sceAudioOutOutput(int, const void*);
int APS5_VABI sceAudioOutOutputs(AudioOutOutputParam*, std::uint32_t);
int APS5_VABI sceAudioOutSetVolume(int, std::uint32_t, int*);
int APS5_VABI sceAudioOutGetLastOutputTime(int, std::uint64_t*);
int APS5_VABI sceAudioOutGetPortState(int, AudioOutPortState*);
int APS5_VABI sceAudioOutSetMixLevelPadSpk(int, int);
}

namespace {
constexpr int invalidPort = static_cast<int>(0x80260003);
constexpr int pending = std::numeric_limits<int>::max();
constexpr std::uint64_t poison = 0xa5a5a5a5a5a5a5a5ULL;
std::atomic<int> shutdownCallbacks{0};
std::atomic<bool> callbackEntered{false};
std::atomic<bool> releaseCallback{false};
std::atomic<bool> outputEntered{false};
thread_local bool traceOutput = false;
void Require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}
void CountShutdown() {
    shutdownCallbacks.fetch_add(1);
    callbackEntered.store(true);
    callbackEntered.notify_all();
    releaseCallback.wait(false);
}
}

extern "C" std::uint64_t APS5_VABI sceKernelGetProcessTime() {
    if (traceOutput) {
        outputEntered.store(true);
        outputEntered.notify_all();
    }
    return 1000;
}

int main() {
#ifdef _WIN32
    _putenv_s("SDL_AUDIODRIVER", "dummy");
#else
    setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif
    Require(sceAudioOutInit() == 0, "audio init failed");
    const int physical = sceAudioOutOpen(0x10000000, 0, 0, 256, 48000, 0);
    const int first = sceAudioOutOpen(0x10000000, 10, 0, 48000, 48000, 0);
    const int second = sceAudioOutOpen(0x10000000, 10, 0, 48000, 48000, 0);
    Require(physical > 0 && first > 0 && second > 0, "audio ports failed to open");
    std::vector<std::int16_t> samples(48000, 0);
    Require(sceAudioOutOutput(physical, samples.data()) == 256, "dummy-device output failed");
    Require(sceAudioOutOutput(first, samples.data()) == 48000, "first queue prefill failed");
    Require(sceAudioOutOutput(second, samples.data()) == 48000, "second queue prefill failed");
    std::atomic<int> outputResult{pending};
    std::atomic<int> outputsResult{pending};
    std::thread output([&] {
        traceOutput = true;
        outputResult.store(sceAudioOutOutput(first, nullptr));
    });
    outputEntered.wait(false);
    std::atomic<bool> stopQueries{false};
    std::atomic<unsigned> accepted{0}, rejected{0};
    std::thread queries([&] {
        while (!stopQueries.load()) {
            std::uint64_t time = poison;
            const int rc = sceAudioOutGetLastOutputTime(first, &time);
            if (rc == 0) {
                accepted.fetch_add(1);
                accepted.notify_all();
            }
            else {
                Require(rc == invalidPort && time == poison, "racing query touched rejected output");
                rejected.fetch_add(1);
                rejected.notify_all();
            }
            std::this_thread::yield();
        }
    });
    accepted.wait(0);
    Require(accepted.load() > 0, "query loop did not overlap admitted audio output");
    Require(outputResult.load() == pending, "prefilled virtual output was not blocked");
    std::thread outputs([&] {
        AudioOutOutputParam param{second, nullptr};
        outputsResult.store(sceAudioOutOutputs(&param, 1));
    });
    LibcRegisterShutdown_nid_postfix(CountShutdown);
    std::thread shutdown1([] { LibcRunShutdown_nid_postfix(); });
    callbackEntered.wait(false);
    std::atomic<bool> secondShutdownEntered{false};
    std::thread shutdown2([&] {
        secondShutdownEntered.store(true);
        secondShutdownEntered.notify_all();
        LibcRunShutdown_nid_postfix();
    });
    secondShutdownEntered.wait(false);
    releaseCallback.store(true);
    releaseCallback.notify_all();
    shutdown1.join();
    shutdown2.join();
    output.join();
    outputs.join();
    Require(outputResult.load() == invalidPort && outputsResult.load() == invalidPort, "pending output was not cancelled");
    rejected.wait(0);
    stopQueries.store(true);
    queries.join();
    Require(rejected.load() > 0, "query loop did not exercise shutdown rejection");
    LibcRunShutdown_nid_postfix();
    Require(shutdownCallbacks.load() == 1, "shutdown callbacks ran more than once");

    AudioOutPortState state;
    std::memset(&state, 0xa5, sizeof(state));
    const auto oldState = state;
    std::uint64_t time = poison;
    int volume[8]{};
    AudioOutOutputParam param{first, samples.data()};
    Require(sceAudioOutInit() == invalidPort, "late init accepted");
    Require(sceAudioOutOpen(0, -1, -1, 0, 0, 0xffffffffu) == invalidPort, "late open reached argument validation");
    Require(sceAudioOutClose(physical) == invalidPort, "late close accepted");
    Require(sceAudioOutOutput(first, samples.data()) == invalidPort, "late output accepted");
    Require(sceAudioOutOutputs(&param, 1) == invalidPort && sceAudioOutOutputs(nullptr, 0) == invalidPort, "late outputs accepted");
    Require(sceAudioOutSetVolume(first, 0xffu, volume) == invalidPort && sceAudioOutSetVolume(first, 0xffu, nullptr) == invalidPort, "late volume accepted");
    Require(sceAudioOutGetLastOutputTime(first, &time) == invalidPort && time == poison, "late time query touched output");
    Require(sceAudioOutGetLastOutputTime(first, nullptr) == invalidPort, "late null time query reached validation");
    Require(sceAudioOutGetPortState(first, &state) == invalidPort && std::memcmp(&state, &oldState, sizeof(state)) == 0, "late state query touched output");
    Require(sceAudioOutGetPortState(first, nullptr) == invalidPort, "late null state query reached validation");
    Require(sceAudioOutSetMixLevelPadSpk(first, -1) == invalidPort, "late mix call reached validation");
}
