#pragma once
#include <cstdint>
namespace HdPipe {
constexpr uint32_t Magic = 0x31444853; // SHD1, followed by raw 24kHz/16-bit/mono PCM frames.
constexpr uint32_t MaximumFrame = 4 * 1024 * 1024;
constexpr uint32_t EventMagic = 0x32444853; // SHD2: typed audio, timing and completion frames.
constexpr uint32_t ReusableEventMagic = 0x33444853; // SHD3: cancellation is drained; synthesizer remains usable.
enum class Type : uint32_t { Audio=1, Word=2, Sentence=3, Viseme=4, Bookmark=5, Complete=6 };
struct Header { uint32_t type; uint32_t size; };
struct Event {
    uint64_t ticks = 0, duration = 0;
    uint32_t textOffset = 0, textLength = 0, value = 0, reserved = 0;
};
static_assert(sizeof(Event) == 32, "Stable cross-bitness timing event");
}
