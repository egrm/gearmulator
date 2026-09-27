// Headless additional-family native producer seed for the actual VST3 harness.
// Each family branch must prove its own firmware, controls and capture boundary
// before it is admitted here. SC-55mk1 is accepted; MT-32 old remains gated
// until its logical SettingsChunk layout and restoration are verified.
#include "88emuplayer/Emu88Processor.h"
#include "88emuplayer/app/Emu88LaunchOptions.h"
#include "88lib/settingsChunk.h"
#include "88lib/boards/sc88types.h"
#include "88lib/boards/laBoard.h"
#include "88lib/boards/laSettings.h"
#include "88lib/rom/romloader.h"
#include "common/test_util.hpp"
#include "baseLib/os.h"
#include "synthLib/romLoader.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 257;
    // sc55Settings_test.cpp establishes a held LevelR scanner cycle before release.
    constexpr int panelHoldFrames = 22050;
    constexpr int panelReleaseFrames = 22050;
    // sc55Settings_test.cpp: native part-one CC7 is SRAM byte 192.
    constexpr size_t selectedPartVolumeAddress = 192;
    // SC-55 native MIDI test: selected A1 program is SRAM byte 185.
    constexpr size_t selectedPartProgramAddress = 185;
    constexpr uint8_t editedProgram = 17;
    constexpr uint8_t initialVolume = 100;
    constexpr uint8_t firstEditedVolume = 101;
    constexpr uint8_t secondEditedVolume = 102;

    void render(emu88Player::Processor& processor, int frames, juce::MidiBuffer midi = {})
    {
        juce::AudioBuffer<float> audio(2, blockSize);
        while(frames > 0)
        {
            const int count = std::min(frames, blockSize);
            audio.setSize(2, count, false, false, true);
            audio.clear();
            processor.processBlock(audio, midi);
            midi.clear();
            frames -= count;
        }
    }

    juce::MemoryBlock save(emu88Player::Processor& processor)
    {
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        CHECK(processor.lastStateOperationSucceeded());
        CHECK(state.getSize() > 0);
        if(!processor.lastStateOperationSucceeded() || state.getSize() == 0)
            throw std::runtime_error("Additional-family Hardware capture is unsupported or incomplete");
        return state;
    }

    emu88Lib::SettingsChunk hardware(const juce::MemoryBlock& state)
    {
        auto envelope = juce::parseXML(juce::String::fromUTF8(
            static_cast<const char*>(state.getData()), static_cast<int>(state.getSize())));
        CHECK(envelope != nullptr);
        const auto* payload = envelope ? envelope->getChildByName("Payload") : nullptr;
        const auto* element = payload ? payload->getChildByName("Hardware") : nullptr;
        CHECK(element != nullptr);
        juce::MemoryBlock encoded;
        CHECK(element && encoded.fromBase64Encoding(element->getAllSubText()));
        auto decoded = emu88Lib::SettingsChunk::decode(encoded.getData(), encoded.getSize());
        CHECK(decoded.has_value());
        if(!decoded) throw std::runtime_error("Additional-family Hardware chunk cannot be decoded");
        return decoded.value();
    }

    juce::MidiMessage mt32ReverbLevel(uint8_t value)
    {
        // Roland MT-32 MIDI Implementation: DT1 System address 10 00 03 is
        // reverb level; LaBoard::parameterMessage supplies the same framing.
        const auto sum = unsigned{0x10} + 0x03 + value;
        const uint8_t payload[]{0x41, 0x10, 0x16, 0x12, 0x10, 0x00, 0x03,
            value, static_cast<uint8_t>(-sum & 0x7f)};
        return juce::MidiMessage::createSysExMessage(payload, sizeof(payload));
    }

    void pressMt32Panel(emu88Player::Processor& processor, emu88Lib::Mt32Button button)
    {
        // Same 0.1 s press / 0.25 s release cadence that produced the
        // measured MT-32 1.07 PART 1 / GROUP / SOUND RQ1 sequence.
        processor.setPanelButtons(emu88Lib::mt32ButtonBit(button));
        render(processor, static_cast<int>(sampleRate / 10));
        processor.setPanelButtons(0);
        render(processor, static_cast<int>(sampleRate / 4));
    }

    void requireMt32ProducerOracle(const emu88Lib::SettingsChunk& state, bool dry)
    {
        using emu88Lib::LaSettings;
        const auto& memory = state.memory;
        const auto valid = state.model == emu88Lib::DeviceModel::Mt32Old &&
            state.layout == LaSettings::LayoutVersion &&
            memory.size() == LaSettings::ImageBytes &&
            memory[LaSettings::PatchHeadOffset] == 0 &&
            memory[LaSettings::PatchHeadOffset + 1] == 63 &&
            memory[LaSettings::SystemOffset + 3] == (dry ? 0 : 1);
        CHECK(valid);
        if(!valid)
            throw std::runtime_error("MT-32 native Sitar and reverb edit is absent from captured LaSettings");
    }

    void runMt32OldProducer()
    {
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        emu88Player::Processor source;
        CHECK(source.setDeviceModel(emu88Lib::DeviceModel::Mt32Old));
        CHECK(source.hasValidRom());
        source.setRateAndBufferSizeDetails(sampleRate, blockSize);
        source.prepareToPlay(sampleRate, blockSize);
        source.config().setValue("audioSetup", "host-owned-routing-marker");
        source.config().setValue("portMidiEnabled", true);
        render(source, static_cast<int>(sampleRate * 10));

        // MT-32 1.07 native probe (`mt32-panel-probe.log`) selected part-one
        // patch group 0 / timbre 63, LCD "Sitar", from factory group 1 / 4.
        pressMt32Panel(source, emu88Lib::Mt32Button::Part1);
        pressMt32Panel(source, emu88Lib::Mt32Button::SoundGroup);
        source.turnPanelEncoder(-16);
        render(source, static_cast<int>(sampleRate));
        pressMt32Panel(source, emu88Lib::Mt32Button::Sound);
        source.turnPanelEncoder(-1);
        render(source, static_cast<int>(sampleRate));
        const auto display = source.hardwareDisplaySnapshot();
        const auto sitarVisible = display && !display->screens[0].text.empty() &&
            display->screens[0].text[0].find("Sitar") != std::string::npos;
        CHECK(sitarVisible);
        if(!sitarVisible) throw std::runtime_error("MT-32 native panel did not select Sitar");
        const auto selected = save(source);

        juce::MidiBuffer dryEdit;
        dryEdit.addEvent(mt32ReverbLevel(0), 11);
        render(source, blockSize, dryEdit);
        render(source, static_cast<int>(sampleRate));
        const auto dry = save(source);
        requireMt32ProducerOracle(hardware(dry), true);

        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        emu88Player::Processor wetSource;
        wetSource.setStateInformation(selected.getData(), static_cast<int>(selected.getSize()));
        CHECK(wetSource.lastStateOperationSucceeded());
        wetSource.setRateAndBufferSizeDetails(sampleRate, blockSize);
        wetSource.prepareToPlay(sampleRate, blockSize);
        juce::MidiBuffer wetEdit;
        wetEdit.addEvent(mt32ReverbLevel(1), 11);
        render(wetSource, blockSize, wetEdit);
        render(wetSource, static_cast<int>(sampleRate));
        const auto wet = save(wetSource);
        const auto wetHardware = hardware(wet);
        requireMt32ProducerOracle(wetHardware, false);
        const auto dryHardware = hardware(dry);
        for(size_t index = 0; index < emu88Lib::LaSettings::ImageBytes; ++index)
            if(index != emu88Lib::LaSettings::SystemOffset + 3)
                CHECK_EQ(dryHardware.memory[index], wetHardware.memory[index]);

        const auto data = juce::File(emu88Player::defaultDataFolder());
        CHECK(data.getChildFile("additional-family-mt32old-dry-model-21.component")
            .replaceWithData(dry.getData(), dry.getSize()));
        CHECK(data.getChildFile("additional-family-mt32old-wet-model-21.component")
            .replaceWithData(wet.getData(), wet.getSize()));
    }

    void runLaMidiProducer(emu88Lib::DeviceModel model, const std::string& name)
    {
        // The real-ROM laBoard_test probes part-one PC17 on MIDI channel 2
        // and system reverb level through DT1 on all three supported boards.
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        emu88Player::Processor source;
        CHECK(source.setDeviceModel(model));
        CHECK(source.hasValidRom());
        source.setRateAndBufferSizeDetails(sampleRate, blockSize);
        source.prepareToPlay(sampleRate, blockSize);
        source.config().setValue("audioSetup", "host-owned-routing-marker");
        source.config().setValue("portMidiEnabled", true);
        render(source, static_cast<int>(sampleRate * 10));
        const auto before = hardware(save(source));
        CHECK(before.model == model);
        CHECK(before.layout == emu88Lib::LaSettings::LayoutVersion);
        CHECK(before.memory.size() == emu88Lib::LaSettings::ImageBytes);

        juce::MidiBuffer programEdit;
        programEdit.addEvent(juce::MidiMessage::programChange(2, 17), 11);
        render(source, blockSize, programEdit);
        render(source, static_cast<int>(sampleRate));
        const auto selected = save(source);
        const auto selectedHardware = hardware(selected);
        CHECK(selectedHardware.model == model);
        CHECK(selectedHardware.memory.size() == emu88Lib::LaSettings::ImageBytes);
        CHECK(selectedHardware.memory[emu88Lib::LaSettings::PatchHeadOffset] !=
                  before.memory[emu88Lib::LaSettings::PatchHeadOffset] ||
              selectedHardware.memory[emu88Lib::LaSettings::PatchHeadOffset + 1] !=
                  before.memory[emu88Lib::LaSettings::PatchHeadOffset + 1]);

        const auto saveWithReverb = [&](uint8_t level)
        {
            juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
            emu88Player::Processor edited;
            edited.setStateInformation(selected.getData(), static_cast<int>(selected.getSize()));
            CHECK(edited.lastStateOperationSucceeded());
            edited.setRateAndBufferSizeDetails(sampleRate, blockSize);
            edited.prepareToPlay(sampleRate, blockSize);
            juce::MidiBuffer reverbEdit;
            reverbEdit.addEvent(mt32ReverbLevel(level), 11);
            render(edited, blockSize, reverbEdit);
            render(edited, static_cast<int>(sampleRate));
            const auto state = save(edited);
            const auto image = hardware(state);
            CHECK(image.model == model);
            CHECK(image.layout == emu88Lib::LaSettings::LayoutVersion);
            CHECK(image.memory.size() == emu88Lib::LaSettings::ImageBytes);
            CHECK_EQ(image.memory[emu88Lib::LaSettings::SystemOffset + 3], level);
            CHECK_EQ(image.memory[emu88Lib::LaSettings::PatchHeadOffset],
                     selectedHardware.memory[emu88Lib::LaSettings::PatchHeadOffset]);
            CHECK_EQ(image.memory[emu88Lib::LaSettings::PatchHeadOffset + 1],
                     selectedHardware.memory[emu88Lib::LaSettings::PatchHeadOffset + 1]);
            return state;
        };
        const auto dry = saveWithReverb(0);
        const auto wet = saveWithReverb(1);
        const auto dryHardware = hardware(dry);
        const auto wetHardware = hardware(wet);
        for(size_t index = 0; index < emu88Lib::LaSettings::ImageBytes; ++index)
            if(index != emu88Lib::LaSettings::SystemOffset + 3)
                CHECK_EQ(dryHardware.memory[index], wetHardware.memory[index]);

        const auto data = juce::File(emu88Player::defaultDataFolder());
        const auto modelNumber = static_cast<int>(model);
        CHECK(data.getChildFile("additional-family-" + juce::String(name.c_str()) + "-dry-model-" +
                                juce::String(modelNumber) + ".component")
            .replaceWithData(dry.getData(), dry.getSize()));
        CHECK(data.getChildFile("additional-family-" + juce::String(name.c_str()) + "-wet-model-" +
                                juce::String(modelNumber) + ".component")
            .replaceWithData(wet.getData(), wet.getSize()));
    }

    void runMt32PanelProbe()
    {
        // Roland MT-32 Owner's Manual, Timbre Setup p.20: PART 1,
        // SOUND GROUP / SELECT-VOLUME, then SOUND / SELECT-VOLUME.
        // The private RQ1 probe reads the resulting part-one patch selection;
        // no guessed display position or knob detent is an acceptance oracle.
        synthLib::RomLoader::setSearchPath(std::getenv("TUS_TEST_ROM_DIR"));
        emu88Lib::RomLoader::rescan();
        const auto roms = emu88Lib::RomLoader::findLaRomSet(emu88Lib::LaModel::Mt32Old);
        CHECK(roms.isValid());
        if(!roms.isValid()) throw std::runtime_error("MT-32 old ROM trio is incomplete");
        emu88Lib::LaBoard board(roms);
        constexpr auto boardRate = emu88Lib::LaBoard::SampleRate;
        constexpr uint32_t patchOneAddress = 0x03u << 14;
        constexpr size_t patchSelectionBytes = 2;
        constexpr size_t replyBudgetSamples = boardRate * 5;
        const auto renderBoard = [&](size_t samples)
        {
            for(size_t sample = 0; sample < samples; ++sample) board.renderSample();
        };
        const auto lcd = [&]()
        {
            std::string result;
            for(unsigned column = 0; column < board.lcd().getVisibleColumns(); ++column)
                result.push_back(static_cast<char>(board.lcd().getVisibleCharacter(column)));
            return result;
        };
        const auto patch = [&]()
        {
            std::vector<uint8_t> result;
            CHECK(board.requestParameterBlock(patchOneAddress, patchSelectionBytes,
                                              replyBudgetSamples, result));
            if(result.size() != patchSelectionBytes)
                throw std::runtime_error("MT-32 panel probe could not read part-one patch");
            return result;
        };
        const auto press = [&](emu88Lib::Mt32Button button)
        {
            board.setButtons(emu88Lib::mt32ButtonBit(button));
            renderBoard(boardRate / 10);
            board.setButtons(0);
            renderBoard(boardRate / 4);
        };
        renderBoard(boardRate * 10);
        const auto before = patch();
        std::cout << "MT-32 panel baseline patch=" << unsigned(before[0]) << ','
                  << unsigned(before[1]) << " LCD=" << lcd() << '\n';
        press(emu88Lib::Mt32Button::Part1);
        press(emu88Lib::Mt32Button::SoundGroup);
        board.turnKnob(-512); // 16 UI steps; observed patch group 0 / timbre 59.
        renderBoard(boardRate);
        const auto group = patch();
        std::cout << "MT-32 panel group patch=" << unsigned(group[0]) << ','
                  << unsigned(group[1]) << " LCD=" << lcd() << '\n';
        press(emu88Lib::Mt32Button::Sound);
        board.turnKnob(-32); // One UI step; observed group 0 / timbre 63.
        renderBoard(boardRate);
        const auto timbre = patch();
        std::cout << "MT-32 panel timbre patch=" << unsigned(timbre[0]) << ','
                  << unsigned(timbre[1]) << " LCD=" << lcd() << '\n';
        CHECK_EQ(before[0], 1);
        CHECK_EQ(before[1], 4);
        CHECK_EQ(group[0], 0);
        CHECK_EQ(group[1], 59);
        CHECK_EQ(timbre[0], 0);
        CHECK_EQ(timbre[1], 63);
    }
}

int main(int argc, char** argv)
{
    if(!std::getenv("TUS_DATA_FOLDER") || !std::getenv("TUS_TEST_ROM_DIR")) return 77;
    if(argc != 2 || (std::string{argv[1]} != "--sc55-mk1" &&
                     std::string{argv[1]} != "--mt32-old" &&
                     std::string{argv[1]} != "--mt32-new" &&
                     std::string{argv[1]} != "--cm32l" &&
                     std::string{argv[1]} != "--mt32-panel-probe"))
        throw std::invalid_argument("No supported additional-family producer fixture for this model");
    baseLib::disableErrorDialogs();
    juce::ScopedJuceInitialiser_GUI juceLifetime;
    emu88Player::LaunchOptions launch;
    launch.values["rom-dir"] = std::getenv("TUS_TEST_ROM_DIR");
    emu88Player::standaloneLaunch = &launch;
    if(std::string{argv[1]} == "--mt32-panel-probe")
    {
        runMt32PanelProbe();
        emu88Player::standaloneLaunch = nullptr;
        return test::finish("88emuMt32OldPanelProbe");
    }
    if(std::string{argv[1]} == "--mt32-old")
    {
        runMt32OldProducer();
        emu88Player::standaloneLaunch = nullptr;
        return test::finish("88emuMt32OldProducerRecall");
    }
    if(std::string{argv[1]} == "--mt32-new" || std::string{argv[1]} == "--cm32l")
    {
        const bool isMt32New = std::string{argv[1]} == "--mt32-new";
        runLaMidiProducer(isMt32New ? emu88Lib::DeviceModel::Mt32New :
            emu88Lib::DeviceModel::Cm32l, isMt32New ? "mt32new" : "cm32l");
        emu88Player::standaloneLaunch = nullptr;
        return test::finish("88emuLaMidiProducerRecall");
    }
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
    emu88Player::Processor source;
    CHECK(source.setDeviceModel(emu88Lib::DeviceModel::Sc55Mk1));
    CHECK(source.hasValidRom());
    source.setRateAndBufferSizeDetails(sampleRate, blockSize);
    source.prepareToPlay(sampleRate, blockSize);
    source.config().setValue("audioSetup", "host-owned-routing-marker");
    source.config().setValue("portMidiEnabled", true);
    const auto initial = hardware(save(source));
    CHECK(initial.model == emu88Lib::DeviceModel::Sc55Mk1);
    CHECK(initial.memory.size() > selectedPartVolumeAddress);
    CHECK_EQ(initial.memory.at(selectedPartVolumeAddress), initialVolume);
    juce::MidiBuffer programEdit;
    programEdit.addEvent(juce::MidiMessage::programChange(1, editedProgram), 11);
    render(source, blockSize, programEdit);
    CHECK_EQ(hardware(save(source)).memory.at(selectedPartProgramAddress), editedProgram);

    source.setPanelButtons(emu88Lib::buttonBit(emu88Lib::Button::LevelR));
    render(source, panelHoldFrames);
    source.setPanelButtons(0);
    render(source, panelReleaseFrames);
    auto edited = save(source);
    auto editedHardware = hardware(edited);
    CHECK_EQ(editedHardware.memory.at(selectedPartVolumeAddress), firstEditedVolume);

    // Save before another live callback: the private clone must consume both
    // queued panel edges, with firmware acknowledgement between them.
    const auto heldEditVolume = editedHardware.memory.at(selectedPartVolumeAddress);
    source.clickPanelButton(emu88Lib::buttonBit(emu88Lib::Button::LevelR), 0);
    edited = save(source);
    editedHardware = hardware(edited);
    CHECK_EQ(heldEditVolume, firstEditedVolume);
    CHECK_EQ(editedHardware.memory.at(selectedPartVolumeAddress), secondEditedVolume);
    CHECK_EQ(editedHardware.memory.at(selectedPartProgramAddress), editedProgram);

    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
    emu88Player::Processor restored;
    restored.setRateAndBufferSizeDetails(sampleRate, blockSize);
    restored.prepareToPlay(sampleRate, blockSize);
    restored.setStateInformation(edited.getData(), static_cast<int>(edited.getSize()));
    CHECK(restored.lastStateOperationSucceeded());
    const auto restoredHardware = hardware(save(restored));
    CHECK_EQ(restoredHardware.memory.at(selectedPartVolumeAddress), secondEditedVolume);
    CHECK_EQ(restoredHardware.memory.at(selectedPartProgramAddress), editedProgram);

    const auto fixture = juce::File(emu88Player::defaultDataFolder())
        .getChildFile("additional-family-sc55mk1-model-5.component");
    CHECK(fixture.replaceWithData(edited.getData(), edited.getSize()));
    emu88Player::standaloneLaunch = nullptr;
    std::cout << "SC-55 native LevelR volume "
              << unsigned(initial.memory.at(selectedPartVolumeAddress)) << " -> "
              << unsigned(editedHardware.memory.at(selectedPartVolumeAddress)) << '\n';
    return test::finish("88emuSc55Recall");
}
