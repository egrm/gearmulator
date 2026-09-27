// SC-55 board execution cloning is a private capture continuation, not a settings image.
#include "88lib/boards/sc55Board.h"
#include "88lib/boards/sc55Settings.h"
#include "88lib/rom/romloader.h"
#include "baseLib/os.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
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

static void probeMk2Settings()
{
    const auto roms = RomLoader::findSc55RomSet(DeviceModel::Sc55Mk2);
    require(roms.isValid(), "Stock SC-55mkII ROM set unavailable");
    Sc55Board source(roms, false);
    require(source.isValid() && source.usesSubMcu(), "Stock mkII sub-MCU board unavailable");
    source.setSwitchPosition(Sc55Board::SwitchMidi);
    const auto rate = source.sampleRate();
    const auto run = [](Sc55Board& board, const uint32_t samples)
    {
        for(uint32_t sample = 0; sample < samples; ++sample) board.renderSample();
    };
    const auto send = [&](Sc55Board& board, const std::initializer_list<uint8_t> bytes)
    {
        // Same byte pacing as the existing SC-55 execution-clone fixture.
        for(const auto byte : bytes)
        {
            board.sendMidiByte(byte);
            run(board, rate / 1000);
        }
    };
    run(source, rate * 10);
    require(Sc55Settings::probeMk2InputDrained(source), "MkII sub-MCU did not drain after boot");
    std::vector<uint8_t> baseline, selected, dry, wet;
    require(Sc55Settings::probeMk2Image(source, baseline), "Stock mkII diagnostic image rejected");
    auto unedited = source.cloneExecution();
    require(bool(unedited), "MkII unedited timing reference rejected");
    source.sendMidiByte(0xc0);
    require(!Sc55Settings::probeMk2InputDrained(source),
            "MkII sub-MCU barrier accepted a queued Program Change status");
    run(source, rate / 1000);
    source.sendMidiByte(17);
    require(!Sc55Settings::probeMk2InputDrained(source),
            "MkII sub-MCU barrier accepted queued Program Change data");
    run(source, rate / 1000);
    run(source, rate);
    require(Sc55Settings::probeMk2InputDrained(source),
            "MkII sub-MCU failed to acknowledge Program Change wire input");
    run(*unedited, 2 * (rate / 1000) + rate);
    require(Sc55Settings::probeMk2Image(source, selected), "MkII program image rejected");
    std::vector<uint8_t> alignedDefault;
    require(Sc55Settings::probeMk2Image(*unedited, alignedDefault), "MkII aligned default rejected");
    require(selected != alignedDefault, "MkII Program Change left SRAM unchanged");
    auto wetBranch = source.cloneExecution();
    require(bool(wetBranch), "MkII wet branch rejected");
    send(source, {0xb0, 91, 0});
    run(source, rate);
    require(Sc55Settings::probeMk2Image(source, dry), "MkII dry image rejected");
    send(*wetBranch, {0xb0, 91, 127});
    run(*wetBranch, rate);
    require(Sc55Settings::probeMk2Image(*wetBranch, wet), "MkII wet image rejected");
    require(dry != wet, "MkII CC91 did not change SRAM");
    const auto logDelta = [](const char* label, const std::vector<uint8_t>& left,
                             const std::vector<uint8_t>& right)
    {
        size_t count{};
        std::cout << label;
        for(size_t address{}; address < left.size(); ++address)
        {
            if(left[address] == right[address]) continue;
            if(count < 32)
                std::cout << ' ' << address << ':' << unsigned(left[address])
                          << '>' << unsigned(right[address]);
            ++count;
        }
        std::cout << " changed=" << count << '\n';
    };
    logDelta("MkII PC17 aligned SRAM", alignedDefault, selected);
    logDelta("MkII CC91 dry/wet aligned SRAM", dry, wet);

    const auto audition = [&](const std::vector<uint8_t>& image)
    {
        Sc55Board candidate(roms, false);
        candidate.setSwitchPosition(Sc55Board::SwitchMidi);
        require(Sc55Settings::reopenMk2Probe(candidate, image), "MkII fresh probe boot rejected");
        send(candidate, {0x90, 60, 100});
        std::vector<Sc55Board::SampleFrame> first;
        first.reserve(rate / 2);
        for(uint32_t sample = 0; sample < rate / 2; ++sample)
            first.push_back(candidate.renderSample());
        send(candidate, {0x80, 60, 0});
        std::vector<Sc55Board::SampleFrame> tail;
        tail.reserve(rate);
        for(uint32_t sample = 0; sample < rate; ++sample)
            tail.push_back(candidate.renderSample());
        return std::pair{std::move(first), std::move(tail)};
    };
    const auto factoryAudio = audition(baseline);
    const auto selectedAudio = audition(selected);
    const auto dryAudio = audition(dry);
    const auto wetAudio = audition(wet);
    require(selectedAudio.first != factoryAudio.first,
            "MkII nondefault Program Change did not affect fresh first-note audio");
    require(dryAudio.second != wetAudio.second,
            "MkII CC91 did not affect fresh note-off reverb tail");
    require(audition(wet) == wetAudio,
            "MkII fresh probe boot is not repeatable for the selected sound");
    std::cout << "Stock SC-55mkII native PC17, CC91 dry/wet SRAM and fresh audio probe passed\n";
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
        probeMk2Settings();
        std::cout << "SC-55 and SC-55mkII execution clone continuation passed\n";
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
