// SC-55 board execution cloning is a private capture continuation, not a settings image.
#include "88lib/boards/sc55Board.h"
#include "88lib/rom/romloader.h"
#include "baseLib/os.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace emu88Lib;

static void require(const bool condition, const char* message)
{
    if(!condition) throw std::runtime_error(message);
}

static void exercise(const DeviceModel model)
{
    auto roms = RomLoader::findSc55RomSet(model);
    if(!roms.isValid()) throw std::runtime_error("SC-55 fixture ROMs unavailable");
    auto live = std::make_unique<Sc55Board>(std::move(roms), false);
    require(live->isValid(), "SC-55 board did not boot");
    for(uint32_t sample = 0; sample < live->sampleRate(); ++sample) live->renderSample();

    // Establish a sounding fixture against an identical idle board. Pace the
    // three wire bytes so the first-generation H8 SCI cannot overrun.
    auto idle = live->cloneExecution();
    require(bool(idle), "SC-55 idle reference clone rejected");
    const uint8_t noteOn[] = {0x90, 60, 100};
    for(const uint8_t byte : noteOn)
    {
        live->sendMidiByte(byte);
        for(uint32_t sample = 0; sample < live->sampleRate() / 1000; ++sample)
        {
            live->renderSample();
            idle->renderSample();
        }
    }
    bool sounded = false;
    for(uint32_t sample = 0; sample < live->sampleRate() && !sounded; ++sample)
        sounded = live->renderSample() != idle->renderSample();
    require(sounded, "SC-55 note never differed from idle reference");
    // This clone starts after the firmware has made the note audible, so the
    // comparison below exercises active GP voice, envelope and DAC state.
    const auto sourceCycle = live->cycles();
    auto clone = live->cloneExecution();
    require(bool(clone), "SC-55 execution clone rejected");
    require(live->cycles() == sourceCycle, "cloning advanced live CPU");
    require(clone->cycles() == sourceCycle, "clone CPU time differs");
    require(clone->buttons() == live->buttons(), "clone panel differs");

    std::vector<Sc55Board::SampleFrame> expected;
    for(uint32_t sample = 0; sample < live->sampleRate(); ++sample)
        expected.push_back(clone->renderSample());
    require(live->cycles() == sourceCycle, "rendering clone advanced live CPU");
    for(const auto& frame : expected)
        require(live->renderSample() == frame, "SC-55 continuation audio differs");
    require(live->cycles() == clone->cycles(), "SC-55 continuation CPU differs");
    require(live->lcd().getDdRam() == clone->lcd().getDdRam(), "SC-55 continuation LCD differs");
    std::vector<uint8_t> liveOut, cloneOut;
    live->readMidiOut(liveOut);
    clone->readMidiOut(cloneOut);
    require(liveOut == cloneOut, "SC-55 continuation MIDI OUT differs");

    auto prepare = live->prepareExecutionClone();
    auto shell = prepare();
    require(bool(shell) && !prepare(), "SC-55 clone factory must be one-use");
    for(uint32_t sample = 0; sample < live->sampleRate() / 8; ++sample) live->renderSample();
    require(shell->copyExecutionFrom(*live), "delayed SC-55 clone copy rejected");
    require(!shell->copyExecutionFrom(*live), "SC-55 capture shell reused");
    for(uint32_t sample = 0; sample < live->sampleRate() / 8; ++sample)
        require(shell->renderSample() == live->renderSample(), "delayed SC-55 continuation differs");

    auto detached = live->prepareExecutionClone();
    live.reset();
    auto survivor = detached();
    require(bool(survivor), "SC-55 clone factory lost its ROM assets");
    require(survivor->copyExecutionFrom(*shell), "SC-55 detached clone rejected");
    for(uint32_t sample = 0; sample < shell->sampleRate() / 8; ++sample)
        require(survivor->renderSample() == shell->renderSample(), "SC-55 detached continuation differs");
}

int main(int argc, char** argv)
{
    baseLib::disableErrorDialogs();
    if(argc != 2) return 77;
    synthLib::RomLoader::setSearchPath(argv[1]);
    try
    {
        exercise(DeviceModel::Sc55Mk1);
        exercise(DeviceModel::Sc55Mk2);
        std::cout << "SC-55 and SC-55mkII execution clone continuation passed\n";
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
