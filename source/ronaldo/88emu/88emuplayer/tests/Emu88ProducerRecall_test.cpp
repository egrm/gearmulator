// One selected SC-88/VL sound, edited by the native panel and host MIDI.
// This is a baseline for the existing hardware-image VST3 path; the smaller
// logical SoundState adapter must pass the same scenario before replacing it.
#include "88emuplayer/Emu88Processor.h"
#include "88emuplayer/app/Emu88LaunchOptions.h"
#include "88lib/settingsChunk.h"
#include "88lib/boards/sc88types.h"
#include "common/test_util.hpp"
#include "baseLib/os.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 128;
    constexpr int panelHoldFrames = 4410;
    constexpr int panelReleaseFrames = 11025;
    // docs/research/88emu-sc88-vl-state-map.md and
    // docs/research/88emu-userinst-panel-procedure.md.
    constexpr size_t partsPerGroup = 16;
    constexpr size_t primaryStride = 0x70;
    constexpr size_t secondaryStride = 0x20;
    constexpr size_t reverbSendOffset = 0x0f;
    constexpr std::array<size_t,3> envelopeOffsets{0x0a,0x0b,0x0c};
    constexpr uint8_t envelopeGroup = 3;
    constexpr uint8_t drySend = 0;
    // Four native InstR presses select GM's first electric piano from program zero.
    constexpr uint8_t electricPianoProgram = 4;

    void render(emu88Player::Processor& processor, int frames, juce::MidiBuffer midi = {})
    {
        juce::AudioBuffer<float> audio(2, blockSize);
        while(frames > 0)
        {
            const auto count = std::min(frames, blockSize);
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
        if(!state.getSize()) throw std::runtime_error("Producer capture returned no state");
        return state;
    }

    emu88Lib::SettingsChunk hardware(const juce::MemoryBlock& state)
    {
        auto envelope = juce::parseXML(juce::String::fromUTF8(
            static_cast<const char*>(state.getData()), static_cast<int>(state.getSize())));
        const auto* payload = envelope ? envelope->getChildByName("Payload") : nullptr;
        const auto* encoded = payload ? payload->getChildByName("Hardware") : nullptr;
        CHECK(encoded != nullptr);
        if(!encoded) throw std::runtime_error("Producer state lacks Hardware");
        juce::MemoryBlock bytes;
        CHECK(bytes.fromBase64Encoding(encoded->getAllSubText()));
        auto decoded = emu88Lib::SettingsChunk::decode(bytes.getData(), bytes.getSize());
        CHECK(decoded.has_value());
        if(!decoded) throw std::runtime_error("Producer Hardware is invalid");
        return *decoded;
    }

    void press(emu88Player::Processor& processor, emu88Lib::Button button)
    {
        processor.setPanelButtons(emu88Lib::buttonBit(button));
        render(processor, panelHoldFrames);
        processor.setPanelButtons(0);
        render(processor, panelReleaseFrames);
    }

    size_t selectedPart(const std::vector<uint8_t>& memory, emu88Lib::DeviceModel model)
    {
        const auto address = model == emu88Lib::DeviceModel::Sc88 ? size_t{0x56f0} : size_t{0x56fa};
        const auto part = (size_t{memory.at(address)} << 8) | memory.at(address + 1);
        CHECK(part < partsPerGroup * 2);
        return part;
    }

    size_t primaryBase(size_t part)
    {
        return (part < partsPerGroup ? size_t{0x8088} : size_t{0x9588}) +
            (part % partsPerGroup) * primaryStride;
    }

    size_t secondaryBase(size_t part)
    {
        // Native A1 edit begins at 87E8, B1 at 9CE8; index zero precedes it.
        return (part < partsPerGroup ? size_t{0x87c8} : size_t{0x9cc8}) +
            (part % partsPerGroup) * secondaryStride;
    }

    void checkSound(const emu88Lib::SettingsChunk& actual,
                    const emu88Lib::SettingsChunk& expected, bool dry)
    {
        CHECK(actual.model == expected.model);
        const auto part = selectedPart(expected.memory, expected.model);
        CHECK(selectedPart(actual.memory, actual.model) == part);
        const auto primary = primaryBase(part);
        const auto secondary = secondaryBase(part);
        CHECK(actual.memory.at(primary) == expected.memory.at(primary));
        CHECK(actual.memory.at(primary + 1) == expected.memory.at(primary + 1));
        for(const auto offset : envelopeOffsets)
            CHECK(actual.memory.at(secondary + offset) == expected.memory.at(secondary + offset));
        CHECK(actual.memory.at(primary + reverbSendOffset) ==
              expected.memory.at(primary + reverbSendOffset));
        if(dry) CHECK_EQ(actual.memory.at(primary + reverbSendOffset), drySend);
    }
}

int main()
{
    if(!std::getenv("TUS_DATA_FOLDER") || !std::getenv("TUS_TEST_ROM_DIR")) return 77;
    baseLib::disableErrorDialogs();
    juce::ScopedJuceInitialiser_GUI juceLifetime;
    emu88Player::LaunchOptions launch;
    launch.values["rom-dir"] = std::getenv("TUS_TEST_ROM_DIR");
    emu88Player::standaloneLaunch = &launch;
    for(const auto model : {emu88Lib::DeviceModel::Sc88, emu88Lib::DeviceModel::Sc88VL})
    {
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        emu88Player::Processor source;
        CHECK(source.setDeviceModel(model));
        CHECK(source.hasValidRom());
        source.setRateAndBufferSizeDetails(sampleRate, blockSize);
        source.prepareToPlay(sampleRate, blockSize);
        source.config().setValue("audioSetup", "host-owned-routing-marker");
        source.config().setValue("portMidiEnabled", true);
        const auto initial = hardware(save(source));
        const auto part = selectedPart(initial.memory, model);
        const auto primary = primaryBase(part);
        const auto secondary = secondaryBase(part);

        for(uint8_t step{}; step < electricPianoProgram; ++step)
            press(source, emu88Lib::Button::InstR);
        CHECK_EQ(hardware(save(source)).memory.at(primary + 1), electricPianoProgram);
        press(source, emu88Lib::Button::ReverbR);
        CHECK(hardware(save(source)).memory.at(primary + reverbSendOffset) !=
              initial.memory.at(primary + reverbSendOffset));
        press(source, emu88Lib::Button::UserInst);
        press(source, emu88Lib::Button::Select); // Filter group
        press(source, emu88Lib::Button::Select); // Envelope group
        const auto group = model == emu88Lib::DeviceModel::Sc88 ? size_t{0x54d7} : size_t{0x54df};
        CHECK_EQ(hardware(save(source)).memory.at(group), envelopeGroup);
        for(const auto button : {emu88Lib::Button::VibRateR,
                                 emu88Lib::Button::VibDepthR,
                                 emu88Lib::Button::VibDelayR})
            press(source, button);
        const auto wet = save(source);
        const auto wetHardware = hardware(wet);
        for(const auto offset : envelopeOffsets)
            CHECK(wetHardware.memory.at(secondary + offset) != initial.memory.at(secondary + offset));
        CHECK(wetHardware.memory.at(primary + reverbSendOffset) != drySend);
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        emu88Player::Processor wetRestored;
        wetRestored.setStateInformation(wet.getData(), static_cast<int>(wet.getSize()));
        CHECK(wetRestored.lastStateOperationSucceeded());
        wetRestored.setRateAndBufferSizeDetails(sampleRate, blockSize);
        wetRestored.prepareToPlay(sampleRate, blockSize);
        render(wetRestored, blockSize);
        checkSound(hardware(save(wetRestored)), wetHardware, false);

        // Native CC91 writes the selected part's reverb send. Zero is dry;
        // save immediately so the private capture must resolve accepted MIDI.
        press(source, emu88Lib::Button::UserInst); // leave edit page
        juce::MidiBuffer dryEdit;
        dryEdit.addEvent(juce::MidiMessage::controllerEvent(1, 91, drySend), 11);
        render(source, blockSize, dryEdit);
        const auto dry = save(source);
        const auto dryHardware = hardware(dry);
        CHECK_EQ(dryHardware.memory.at(primary + reverbSendOffset), drySend);
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        emu88Player::Processor dryRestored;
        dryRestored.setStateInformation(dry.getData(), static_cast<int>(dry.getSize()));
        CHECK(dryRestored.lastStateOperationSucceeded());
        dryRestored.setRateAndBufferSizeDetails(sampleRate, blockSize);
        dryRestored.prepareToPlay(sampleRate, blockSize);
        render(dryRestored, blockSize);
        checkSound(hardware(save(dryRestored)), dryHardware, true);

        const auto data = juce::File(emu88Player::defaultDataFolder());
        const auto suffix = juce::String(static_cast<int>(model)) + ".component";
        CHECK(data.getChildFile("producer-wet-model-" + suffix)
              .replaceWithData(wet.getData(), wet.getSize()));
        CHECK(data.getChildFile("producer-dry-model-" + suffix)
              .replaceWithData(dry.getData(), dry.getSize()));
        std::cout << "producer native model=" << static_cast<int>(model)
                  << " part=" << part << " program=" << unsigned(wetHardware.memory.at(primary + 1))
                  << " reverb-wet=" << unsigned(wetHardware.memory.at(primary + reverbSendOffset))
                  << " dry=" << unsigned(dryHardware.memory.at(primary + reverbSendOffset)) << '\n';
    }
    emu88Player::standaloneLaunch = nullptr;
    return test::finish("88emuProducerRecall");
}
