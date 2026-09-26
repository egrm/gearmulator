#include "88lib/boards/sc88proSettings.h"
#include "88lib/boards/sc88pro.h"
#include "88lib/rom/rom.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>

using namespace emu88Lib;

// SC-88Pro ROM 0C:94C9/9500 enters page 2; 0C:5CC8 commits the
// descriptor cursor at 4C60, and 0C:8A38 advances it in steps of 0010.
static constexpr size_t g_panelPageAddress = 0x4b47;
static constexpr uint8_t g_partEditPage = 2;
static constexpr size_t g_menuCursorAddress = 0x4c60;
static constexpr uint16_t g_invalidUnalignedCursor = 0x11;
// ROM 0C:950D enters page 1; 0C:89E2 and 0C:89F3 bound its 4C68 cursor.
static constexpr uint8_t g_systemPage = 1;
static constexpr size_t g_systemCursorAddress = 0x4c68;
static constexpr uint16_t g_invalidSystemCursor = 0xd0;

static std::vector<uint8_t> read(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

int main()
{
    const auto* folder = std::getenv("TUS_TEST_ROM_DIR");
    if(!folder) return 77;
    auto rom = read(std::filesystem::path(folder) / "sc88pro_control.bin");
    if(rom.empty()) return 77;
    normalizeH8WordOrder(rom);
    auto source = std::make_unique<Sc88Pro>(rom, std::vector<uint8_t>{}, true);
    for(unsigned sample = 0; sample < g_sampleRate * 10; ++sample) source->renderSample();
    std::vector<uint8_t> image;
    unsigned failed = 0, checks = 0;
    const auto check = [&](bool value, const char* message)
    {
        ++checks;
        if(!value) { ++failed; std::cerr << "FAIL " << message << '\n'; }
    };
    using Result = Sc88ProSettings::Result;
    check(Sc88ProSettings::capture(*source, image) == Result::Success, "capture supported firmware");
    check(image.size() == Sc88Pro::SramSize, "exact SRAM image size");
    auto fresh = std::make_unique<Sc88Pro>(rom, std::vector<uint8_t>{}, false);
    const auto originalCycles = fresh->cycles();
    auto truncated = image; truncated.pop_back();
    check(Sc88ProSettings::restore(*fresh, truncated) == Result::InvalidImage, "reject truncated image");
    auto oversized = image; oversized.push_back(0);
    check(Sc88ProSettings::restore(*fresh, oversized) == Result::InvalidImage, "reject oversized image");
    auto invalidParameter = image;
    const auto parameterAddress = (unsigned(rom[0x1aef4]) << 8) | rom[0x1aef5];
    invalidParameter[parameterAddress] = 0xff;
    check(Sc88ProSettings::restore(*fresh, invalidParameter) == Result::InvalidImage, "reject invalid native EFX data");
    auto invalidSelection = image;
    invalidSelection[0x4d79] = 32;
    check(Sc88ProSettings::restore(*fresh, invalidSelection) == Result::InvalidImage,
          "reject selected part outside both sixteen-part groups");
    auto invalidMenuCursor = image;
    invalidMenuCursor[g_panelPageAddress] = g_partEditPage;
    invalidMenuCursor[g_menuCursorAddress] = static_cast<uint8_t>(g_invalidUnalignedCursor >> 8);
    invalidMenuCursor[g_menuCursorAddress + sizeof(uint8_t)] = static_cast<uint8_t>(g_invalidUnalignedCursor);
    check(Sc88ProSettings::restore(*fresh, invalidMenuCursor) == Result::InvalidImage,
          "reject unaligned normal-part menu cursor before boot");
    auto invalidSystemCursor = image;
    invalidSystemCursor[g_panelPageAddress] = g_systemPage;
    invalidSystemCursor[g_systemCursorAddress] = static_cast<uint8_t>(g_invalidSystemCursor >> 8);
    invalidSystemCursor[g_systemCursorAddress + sizeof(uint8_t)] = static_cast<uint8_t>(g_invalidSystemCursor);
    check(Sc88ProSettings::restore(*fresh, invalidSystemCursor) == Result::InvalidImage,
          "reject System cursor beyond native endpoint before boot");
    invalidSystemCursor[g_systemCursorAddress + sizeof(uint8_t)] =
        static_cast<uint8_t>(g_invalidUnalignedCursor);
    check(Sc88ProSettings::restore(*fresh, invalidSystemCursor) == Result::InvalidImage,
          "reject unaligned System cursor before boot");
    check(fresh->cycles() == originalCycles, "invalid image cannot run board");
    check(Sc88ProSettings::restore(*fresh, image) == Result::Success, "restore supported fresh board");
    const auto restoredCycles = fresh->cycles();
    check(Sc88ProSettings::restore(*fresh, image) == Result::RequiresFreshBoard, "reject live-board restore");
    check(fresh->cycles() == restoredCycles, "rejected restore leaves board unchanged");
    rom.back() ^= 1;
    auto unknown = std::make_unique<Sc88Pro>(rom, std::vector<uint8_t>{}, false);
    check(Sc88ProSettings::restore(*unknown, image) == Result::UnsupportedFirmware, "reject different firmware");
    std::vector<uint8_t> sentinel{42};
    check(Sc88ProSettings::capture(*unknown, sentinel) == Result::UnsupportedFirmware, "unsupported capture fails");
    check(sentinel == std::vector<uint8_t>{42}, "failed capture preserves destination");
    std::cout << checks << " checks, " << failed << " failures\n";
    return failed ? 1 : 0;
}
