// Headless additional-family native producer seed for the actual VST3 harness.
// Each family branch must prove its own firmware, controls and capture boundary
// before it is admitted here. The SC-55mk1 branch is the first such case.
#include "88emuplayer/Emu88Processor.h"
#include "88emuplayer/app/Emu88LaunchOptions.h"
#include "88lib/settingsChunk.h"
#include "88lib/boards/sc88types.h"
#include "common/test_util.hpp"
#include "baseLib/os.h"

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
}

int main(int argc, char** argv)
{
    if(!std::getenv("TUS_DATA_FOLDER") || !std::getenv("TUS_TEST_ROM_DIR")) return 77;
    if(argc != 2 || std::string{argv[1]} != "--sc55-mk1")
        throw std::invalid_argument("No supported additional-family producer fixture for this model");
    baseLib::disableErrorDialogs();
    juce::ScopedJuceInitialiser_GUI juceLifetime;
    emu88Player::LaunchOptions launch;
    launch.values["rom-dir"] = std::getenv("TUS_TEST_ROM_DIR");
    emu88Player::standaloneLaunch = &launch;
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
