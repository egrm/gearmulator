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
