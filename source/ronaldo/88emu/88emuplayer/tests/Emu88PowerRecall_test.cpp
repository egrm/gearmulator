// Headless supply-off project recall. Uses the processor's real state callbacks.
#include "88emuplayer/Emu88Processor.h"
#include "88emuplayer/app/Emu88LaunchOptions.h"
#include "88lib/settingsChunk.h"
#include "common/test_util.hpp"
#include "baseLib/os.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 257;
    constexpr int auditionBlocks = 120;

    void prepare(emu88Player::Processor& processor)
    {
        processor.setRateAndBufferSizeDetails(sampleRate, blockSize);
        processor.prepareToPlay(sampleRate, blockSize);
    }

    juce::MemoryBlock save(emu88Player::Processor& processor)
    {
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        CHECK(processor.lastStateOperationSucceeded());
        CHECK(state.getSize() > 0);
        return state;
    }

    std::unique_ptr<juce::XmlElement> payload(const juce::MemoryBlock& state)
    {
        auto envelope = juce::parseXML(juce::String::fromUTF8(
            static_cast<const char*>(state.getData()), static_cast<int>(state.getSize())));
        CHECK(envelope != nullptr);
        const auto* child = envelope ? envelope->getChildByName("Payload") : nullptr;
        CHECK(child != nullptr);
        return child ? std::make_unique<juce::XmlElement>(*child) : nullptr;
    }

    std::vector<float> render(emu88Player::Processor& processor, juce::MidiBuffer midi = {})
    {
        juce::AudioBuffer<float> buffer(2, blockSize);
        std::vector<float> audio;
        audio.reserve(static_cast<size_t>(auditionBlocks * blockSize * 2));
        for(int block{}; block < auditionBlocks; ++block)
        {
            buffer.clear();
            processor.processBlock(buffer, midi);
            midi.clear();
            for(int channel{}; channel < buffer.getNumChannels(); ++channel)
                for(int sample{}; sample < buffer.getNumSamples(); ++sample)
                    audio.push_back(buffer.getSample(channel, sample));
        }
        return audio;
    }

    juce::MidiBuffer note()
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(96)), 13);
        return midi;
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
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
    emu88Player::Processor source;
    CHECK(source.setDeviceModel(emu88Lib::DeviceModel::Sc88Pro));
    CHECK(source.hasValidRom());
    prepare(source);
    auto boot = source.bootOptions();
    boot.factoryReset = false;
    source.setBootOptions(boot);
    juce::MidiBuffer edits;
    edits.addEvent(juce::MidiMessage::programChange(1, 17), 0);
    edits.addEvent(juce::MidiMessage::controllerEvent(1, 7, 43), 23);
    (void)render(source, edits);
    const auto onState = save(source);
    auto onPayload = payload(onState);
    CHECK(onPayload->getBoolAttribute("power"));
    CHECK(onPayload->getChildByName("Hardware") != nullptr);
    // Begin sounding and silent controls from the same restored state and
    // block timeline; a nonzero meter value alone could be idle noise.
    source.setStateInformation(onState.getData(), static_cast<int>(onState.getSize()));
    CHECK(source.lastStateOperationSucceeded());
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
    emu88Player::Processor quietSource;
    prepare(quietSource);
    quietSource.setStateInformation(onState.getData(), static_cast<int>(onState.getSize()));
    CHECK(quietSource.lastStateOperationSucceeded());
    const auto sounding = render(source, note());
    const auto idle = render(quietSource);
    CHECK(sounding != idle);
    CHECK(source.setPower(false));
    CHECK(!source.isPoweredOn());
    const auto offState = save(source);
    auto offPayload = payload(offState);
    CHECK(!offPayload->getBoolAttribute("power"));
    CHECK(offPayload->getChildByName("Hardware") != nullptr);
    CHECK(offPayload->getStringAttribute("assets") == onPayload->getStringAttribute("assets"));
    const auto fixture = juce::File(emu88Player::defaultDataFolder()).getChildFile("power-off-model-2.component");
    CHECK(fixture.replaceWithData(offState.getData(), offState.getSize()));
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
    emu88Player::Processor offRestored;
    prepare(offRestored);
    offRestored.setStateInformation(offState.getData(), static_cast<int>(offState.getSize()));
    CHECK(offRestored.lastStateOperationSucceeded());
    CHECK(!offRestored.isPoweredOn());
    CHECK(save(offRestored) == offState);
    const auto offAudio = render(offRestored, note());
    CHECK(std::all_of(offAudio.begin(), offAudio.end(), [](float sample) { return sample == 0.f; }));
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
    emu88Player::Processor onReference;
    prepare(onReference);
    onReference.setStateInformation(onState.getData(), static_cast<int>(onState.getSize()));
    CHECK(onReference.lastStateOperationSucceeded());
    CHECK(offRestored.setPower(true));
    CHECK(offRestored.isPoweredOn());
    const auto referenceNote = render(onReference, note());
    const auto wakeNote = render(offRestored, note());
    CHECK(referenceNote == wakeNote);
    emu88Player::standaloneLaunch = nullptr;
    std::cout << "supply-off retained hardware and exact first-note wake\n";
    return test::finish("88emuPowerRecall");
}
