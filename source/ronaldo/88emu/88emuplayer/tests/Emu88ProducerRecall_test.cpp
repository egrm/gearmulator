// One selected SC-88/VL sound, edited by the native panel and host MIDI.
// This is a baseline for the existing hardware-image VST3 path; the smaller
// logical SoundState adapter must pass the same scenario before replacing it.
#include "88emuplayer/Emu88Processor.h"
#include "88emuplayer/app/Emu88LaunchOptions.h"
#include "88lib/settingsChunk.h"
#include "88lib/boards/sc88types.h"
#include "88lib/boards/sc88pro.h"
#include "common/test_util.hpp"
#include "baseLib/os.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

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
    // SC-88 and SC-88VL owner's manuals: PC005 / CC0 008 is St.Soft EP;
    // CC32 02 selects the SC-88 map. sc88 bank audit: D860 and D820 are
    // pending CC0 and CC32 latches; the selected primary record commits MSB/PC.
    constexpr uint8_t softEpBankMsb = 8;
    constexpr uint8_t sc88MapLsb = 2;
    constexpr size_t pendingBankMsbBase = 0xd860;
    constexpr size_t pendingMapLsbBase = 0xd820;
    // sc88proSoundState_probe.cpp and pro-logical-sound-fields-2.log.
    constexpr size_t proSelectedPartWord = 0x4d78;
    constexpr size_t proDescriptorPointer = 0xcf7a;
    constexpr size_t proProgramOffset = 1;
    constexpr size_t proReverbSendOffset = 0x0f;
    constexpr std::array<size_t, 3> proEnvelopeOffsets{0x14, 0x15, 0x16};
    constexpr size_t proReverbMacroAddress = 0x506a;
    constexpr uint8_t proEditedEnvelope = 0x41;

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

    void press(emu88Player::Processor& processor, emu88Lib::Sc88ProButton button)
    {
        processor.setPanelButtons(uint32_t{1} << static_cast<uint8_t>(button));
        render(processor, panelHoldFrames);
        processor.setPanelButtons(0);
        render(processor, panelReleaseFrames);
    }

    void proDt1(emu88Player::Processor& processor, uint8_t address0,
                uint8_t address1, uint8_t address2, uint8_t value)
    {
        // Native Roland DT1 framing from sc88proSoundState_probe.cpp::dt1.
        const auto sum = address0 + address1 + address2 + value;
        const uint8_t payload[]{0x41, 0x10, 0x42, 0x12, address0,
            address1, address2, value, static_cast<uint8_t>(-sum & 0x7f)};
        juce::MidiBuffer edit;
        edit.addEvent(juce::MidiMessage::createSysExMessage(payload, sizeof(payload)), 11);
        render(processor, blockSize, edit);
    }

    size_t proDescriptor(const emu88Lib::SettingsChunk& state)
    {
        CHECK_EQ(state.model, emu88Lib::DeviceModel::Sc88Pro);
        CHECK_EQ(state.memory.at(proSelectedPartWord), 0);
        CHECK_EQ(state.memory.at(proSelectedPartWord + 1), 0);
        const auto address = (size_t{state.memory.at(proDescriptorPointer)} << 8) |
            state.memory.at(proDescriptorPointer + 1);
        CHECK(address + proEnvelopeOffsets.back() < state.memory.size());
        return address;
    }

    void checkProSound(const emu88Lib::SettingsChunk& actual,
                       const emu88Lib::SettingsChunk& expected, bool dry)
    {
        const auto descriptor = proDescriptor(expected);
        CHECK_EQ(proDescriptor(actual), descriptor);
        for(const auto offset : {size_t{0}, proProgramOffset, proReverbSendOffset,
                                   proEnvelopeOffsets[0], proEnvelopeOffsets[1], proEnvelopeOffsets[2]})
            CHECK_EQ(actual.memory.at(descriptor + offset), expected.memory.at(descriptor + offset));
        CHECK_EQ(actual.memory.at(proReverbMacroAddress), expected.memory.at(proReverbMacroAddress));
        CHECK_EQ(expected.memory.at(descriptor + proReverbSendOffset), dry ? 0 : 1);
    }

    void runProProducer()
    {
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        emu88Player::Processor source;
        CHECK(source.setDeviceModel(emu88Lib::DeviceModel::Sc88Pro));
        CHECK(source.hasValidRom());
        source.setRateAndBufferSizeDetails(sampleRate, blockSize);
        source.prepareToPlay(sampleRate, blockSize);
        source.config().setValue("audioSetup", "host-owned-routing-marker");
        source.config().setValue("portMidiEnabled", true);
        const auto initial = hardware(save(source));
        const auto descriptor = proDescriptor(initial);
        CHECK_EQ(initial.memory.at(descriptor + proProgramOffset), 0);
        CHECK_EQ(initial.memory.at(descriptor + proReverbSendOffset), 0x28);
        for(const auto offset : proEnvelopeOffsets)
            CHECK_EQ(initial.memory.at(descriptor + offset), 0x40);

        for(uint8_t step{}; step < electricPianoProgram; ++step)
            press(source, emu88Lib::Sc88ProButton::InstR);
        CHECK_EQ(hardware(save(source)).memory.at(descriptor + proProgramOffset),
            electricPianoProgram);
        proDt1(source, 0x40, 0x11, 0x22, 0); // Native A1 reverb send dry.
        proDt1(source, 0x40, 0x01, 0x30, 2); // Native system reverb macro.
        const auto base = save(source);
        const auto baseHardware = hardware(base);
        CHECK_EQ(baseHardware.memory.at(descriptor + proReverbSendOffset), 0);
        CHECK_EQ(baseHardware.memory.at(proReverbMacroAddress), 2);

        const auto editEnvelope = [](emu88Player::Processor& target)
        {
            for(int page = 0; page < 3; ++page)
                press(target, emu88Lib::Sc88ProButton::Select);
            for(const auto button : {emu88Lib::Sc88ProButton::VibRateR,
                                     emu88Lib::Sc88ProButton::VibDepthR,
                                     emu88Lib::Sc88ProButton::VibDelayR})
                press(target, button);
        };
        editEnvelope(source);
        const auto dry = save(source);
        const auto dryHardware = hardware(dry);
        for(const auto offset : proEnvelopeOffsets)
            CHECK_EQ(dryHardware.memory.at(descriptor + offset), proEditedEnvelope);
        checkProSound(dryHardware, dryHardware, true);

        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        emu88Player::Processor wetSource;
        wetSource.setStateInformation(base.getData(), static_cast<int>(base.getSize()));
        CHECK(wetSource.lastStateOperationSucceeded());
        wetSource.setRateAndBufferSizeDetails(sampleRate, blockSize);
        wetSource.prepareToPlay(sampleRate, blockSize);
        press(wetSource, emu88Lib::Sc88ProButton::ReverbR);
        CHECK_EQ(hardware(save(wetSource)).memory.at(descriptor + proReverbSendOffset), 1);
        editEnvelope(wetSource);
        const auto wet = save(wetSource);
        const auto wetHardware = hardware(wet);
        checkProSound(wetHardware, wetHardware, false);

        for(const auto& scenario : {std::pair{"dry", &dry}, std::pair{"wet", &wet}})
        {
            juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
            emu88Player::Processor restored;
            restored.setStateInformation(scenario.second->getData(),
                static_cast<int>(scenario.second->getSize()));
            CHECK(restored.lastStateOperationSucceeded());
            restored.setRateAndBufferSizeDetails(sampleRate, blockSize);
            restored.prepareToPlay(sampleRate, blockSize);
            render(restored, blockSize);
            checkProSound(hardware(save(restored)), hardware(*scenario.second),
                std::string{scenario.first} == "dry");
            const auto path = juce::File(emu88Player::defaultDataFolder()).getChildFile(
                juce::String("producer-pro-") + scenario.first + "-model-2.component");
            CHECK(path.replaceWithData(scenario.second->getData(), scenario.second->getSize()));
        }

        // SC-88Pro owner's manual pp.126-127: Native-map Piano3w is CC0 8,
        // CC32 3, PC 2. Observe the committed part descriptor separately from
        // the bank latches by changing those latches after the program change.
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        emu88Player::Processor bankSource;
        CHECK(bankSource.setDeviceModel(emu88Lib::DeviceModel::Sc88Pro));
        bankSource.setRateAndBufferSizeDetails(sampleRate, blockSize);
        bankSource.prepareToPlay(sampleRate, blockSize);
        juce::MidiBuffer bankEdit;
        bankEdit.addEvent(juce::MidiMessage::controllerEvent(1, 0, 8), 11);
        bankEdit.addEvent(juce::MidiMessage::controllerEvent(1, 32, 3), 23);
        bankEdit.addEvent(juce::MidiMessage::programChange(1, 2), 37);
        render(bankSource, blockSize, bankEdit);
        const auto committedBank = hardware(save(bankSource));
        const auto bankDescriptor = proDescriptor(committedBank);
        juce::MidiBuffer pendingEdit;
        pendingEdit.addEvent(juce::MidiMessage::controllerEvent(1, 0, 0), 11);
        pendingEdit.addEvent(juce::MidiMessage::controllerEvent(1, 32, 1), 23);
        render(bankSource, blockSize, pendingEdit);
        const auto pendingBank = hardware(save(bankSource));
        CHECK_EQ(committedBank.memory.at(bankDescriptor), 8);
        CHECK_EQ(committedBank.memory.at(bankDescriptor + proProgramOffset), 2);
        CHECK_EQ(pendingBank.memory.at(bankDescriptor), 8);
        CHECK_EQ(pendingBank.memory.at(bankDescriptor + proProgramOffset), 2);
        // MIDI channel 2 is firmware part slot 1; these are pending CC
        // selectors, deliberately different from the committed Piano3w tone.
        constexpr size_t proPartOneBankMsb = 0xc860 + 2;
        constexpr size_t proPartOneMapLsb = 0xc820 + 2;
        CHECK_EQ(committedBank.memory.at(proPartOneBankMsb), 8);
        CHECK_EQ(committedBank.memory.at(proPartOneMapLsb), 3);
        CHECK_EQ(pendingBank.memory.at(proPartOneBankMsb), 0);
        CHECK_EQ(pendingBank.memory.at(proPartOneMapLsb), 1);
        const auto bankComponent = save(bankSource);
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        emu88Player::Processor bankRestored;
        bankRestored.setStateInformation(bankComponent.getData(),
            static_cast<int>(bankComponent.getSize()));
        CHECK(bankRestored.lastStateOperationSucceeded());
        const auto restoredBank = hardware(save(bankRestored));
        CHECK_EQ(proDescriptor(restoredBank), bankDescriptor);
        CHECK_EQ(restoredBank.memory.at(bankDescriptor), 8);
        CHECK_EQ(restoredBank.memory.at(bankDescriptor + proProgramOffset), 2);
        CHECK_EQ(restoredBank.memory.at(proPartOneBankMsb), 0);
        CHECK_EQ(restoredBank.memory.at(proPartOneMapLsb), 1);
        CHECK(juce::File(emu88Player::defaultDataFolder())
            .getChildFile("producer-pro-bank-model-2.component")
            .replaceWithData(bankComponent.getData(), bankComponent.getSize()));
        std::cout << "producer native Pro program="
                  << unsigned(wetHardware.memory.at(descriptor + proProgramOffset))
                  << " reverb-wet=" << unsigned(wetHardware.memory.at(descriptor + proReverbSendOffset))
                  << " dry=" << unsigned(dryHardware.memory.at(descriptor + proReverbSendOffset)) << '\n';
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

int main(int argc, char** argv)
{
    if(!std::getenv("TUS_DATA_FOLDER") || !std::getenv("TUS_TEST_ROM_DIR")) return 77;
    baseLib::disableErrorDialogs();
    juce::ScopedJuceInitialiser_GUI juceLifetime;
    emu88Player::LaunchOptions launch;
    launch.values["rom-dir"] = std::getenv("TUS_TEST_ROM_DIR");
    emu88Player::standaloneLaunch = &launch;
    if(argc == 2 && std::string{argv[1]} == "--pro")
    {
        runProProducer();
        emu88Player::standaloneLaunch = nullptr;
        return test::finish("88emuProducerRecallPro");
    }
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
        juce::MidiBuffer bankEdit;
        bankEdit.addEvent(juce::MidiMessage::controllerEvent(1, 0, softEpBankMsb), 11);
        bankEdit.addEvent(juce::MidiMessage::controllerEvent(1, 32, sc88MapLsb), 23);
        bankEdit.addEvent(juce::MidiMessage::programChange(1, electricPianoProgram), 37);
        render(source, blockSize, bankEdit);
        const auto bank = save(source);
        const auto bankHardware = hardware(bank);
        CHECK_EQ(bankHardware.memory.at(primary), softEpBankMsb);
        CHECK_EQ(bankHardware.memory.at(primary + 1), electricPianoProgram);
        CHECK_EQ(bankHardware.memory.at(pendingBankMsbBase + 2 * part), softEpBankMsb);
        CHECK_EQ(bankHardware.memory.at(pendingMapLsbBase + 2 * part), sc88MapLsb);
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        emu88Player::Processor bankRestored;
        bankRestored.setStateInformation(bank.getData(), static_cast<int>(bank.getSize()));
        CHECK(bankRestored.lastStateOperationSucceeded());
        bankRestored.setRateAndBufferSizeDetails(sampleRate, blockSize);
        bankRestored.prepareToPlay(sampleRate, blockSize);
        render(bankRestored, blockSize);
        const auto bankRoundTrip = hardware(save(bankRestored));
        CHECK_EQ(bankRoundTrip.memory.at(primary), softEpBankMsb);
        CHECK_EQ(bankRoundTrip.memory.at(primary + 1), electricPianoProgram);
        CHECK_EQ(bankRoundTrip.memory.at(pendingBankMsbBase + 2 * part), softEpBankMsb);
        CHECK_EQ(bankRoundTrip.memory.at(pendingMapLsbBase + 2 * part), sc88MapLsb);
        CHECK(data.getChildFile("producer-bank-model-" + suffix)
              .replaceWithData(bank.getData(), bank.getSize()));
        std::cout << "producer native model=" << static_cast<int>(model)
                  << " part=" << part << " program=" << unsigned(wetHardware.memory.at(primary + 1))
                  << " reverb-wet=" << unsigned(wetHardware.memory.at(primary + reverbSendOffset))
                  << " dry=" << unsigned(dryHardware.memory.at(primary + reverbSendOffset)) << '\n';
    }
    emu88Player::standaloneLaunch = nullptr;
    return test::finish("88emuProducerRecall");
}
