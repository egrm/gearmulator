// Headless real processor callbacks; never opens an audio/MIDI device or editor.
#include "88emuplayer/Emu88Processor.h"
#include "88emuplayer/app/Emu88LaunchOptions.h"
#include "88lib/settingsChunk.h"
#include "88lib/boards/sc88pro.h"
#include "88lib/boards/sc88types.h"
#include "88lib/rom/romloader.h"
#include "common/test_util.hpp"
#include "baseLib/os.h"
#include <iostream>
#include <chrono>
#include <thread>

namespace
{
    void measureConcurrentSave(emu88Player::Processor& processor)
    {
        // A diagnostic workload, not a scheduler-dependent CI timing assertion.
        // No device is opened: one worker saves while the caller renders blocks.
        constexpr int frames = 64;
        constexpr double rate = 48000;
        constexpr size_t saveCount = 8;
        processor.setRateAndBufferSizeDetails(rate, frames);
        processor.prepareToPlay(rate, frames);
        std::atomic<bool> started{false}, finished{false};
        std::exception_ptr error;
        std::array<juce::MemoryBlock, saveCount> saved;
        juce::AudioBuffer<float> buffer(2, frames);
        juce::MidiBuffer midi;
        std::thread saver([&]
        {
            while(!started.load()) std::this_thread::yield();
            try
            {
                for(auto& state : saved) processor.getStateInformation(state);
            }
            catch(...) { error = std::current_exception(); }
            finished.store(true);
        });
        double maximumWaitMs{}, maximumBlockMs{};
        size_t blocks{};
        std::exception_ptr renderError;
        started.store(true);
        try
        {
            while(!finished.load())
            {
                const auto begin = std::chrono::steady_clock::now();
                {
                    const juce::ScopedLock lock(processor.getCallbackLock());
                    const auto acquired = std::chrono::steady_clock::now();
                    maximumWaitMs = std::max(maximumWaitMs,
                        std::chrono::duration<double, std::milli>(acquired - begin).count());
                    processor.processBlock(buffer, midi);
                }
                maximumBlockMs = std::max(maximumBlockMs,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
                ++blocks;
                std::this_thread::yield();
            }
        }
        catch(...) { renderError = std::current_exception(); }
        saver.join();
        if(error) std::rethrow_exception(error);
        if(renderError) std::rethrow_exception(renderError);
        for(const auto& state : saved) CHECK(state.getSize() > 0);
        std::cout << "capture-timing saves=" << saveCount << " blocks=" << blocks
                  << " maximum-callback-lock-wait-ms=" << maximumWaitMs
                  << " maximum-render-block-ms=" << maximumBlockMs
                  << " host-block-budget-ms=" << frames / rate * 1000 << '\n';
    }

    juce::MemoryBlock save(emu88Player::Processor& processor)
    {
        juce::MemoryBlock result;
        processor.getStateInformation(result);
        CHECK(processor.lastStateOperationSucceeded());
        CHECK(result.getSize() > 0);
        if(!result.getSize()) throw std::runtime_error("Save callback returned empty state");
        return result;
    }

    emu88Lib::SettingsChunk hardware(const juce::MemoryBlock& state)
    {
        auto root = juce::parseXML(juce::String::fromUTF8(static_cast<const char*>(state.getData()), static_cast<int>(state.getSize())));
        juce::MemoryBlock bytes;
        CHECK(root != nullptr);
        if(!root) throw std::runtime_error("Saved envelope is not readable");
        CHECK(bytes.fromBase64Encoding(root->getChildByName("Payload")->getChildByName("Hardware")->getAllSubText()));
        auto result = emu88Lib::SettingsChunk::decode(bytes.getData(), bytes.getSize());
        CHECK(result.has_value());
        return *result;
    }

    void load(emu88Player::Processor& processor, const juce::MemoryBlock& state)
    {
        processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        if(!processor.lastStateOperationSucceeded()) std::cerr << processor.stateDiagnostic() << '\n';
        CHECK(processor.lastStateOperationSucceeded());
    }

    double render(emu88Player::Processor& processor, int frames, juce::MidiBuffer midi = {})
    {
        juce::AudioBuffer<float> buffer(2, frames);
        processor.processBlock(buffer, midi);
        double energy{};
        for(int c = 0; c < buffer.getNumChannels(); ++c)
            for(int i = 0; i < frames; ++i) energy += std::abs(buffer.getSample(c, i));
        return energy;
    }

    void reject(emu88Player::Processor& processor, const void* bytes, int size)
    {
        bool rejected = false;
        try { processor.setStateInformation(bytes, size); }
        catch(const std::exception&) { rejected = true; }
        CHECK(rejected);
        CHECK(!processor.lastStateOperationSucceeded());
    }

    uint8_t selectedLevel(const emu88Lib::SettingsChunk& state)
    {
        // Audited sc88pro_state_probe.cpp panel-boundary fixture: A1 descriptor
        // pointer CF7A, level field at offset8. CF7C belongs to the A2 journal fixture.
        const auto address = (size_t{state.memory[0xcf7a]} << 8) | state.memory[0xcf7b];
        return state.memory.at(address + 8);
    }

    void familyPanelRecall()
    {
        // Existing family probe maps the selected internal part through 56F0/56FA;
        // visible A1 is internal slot1, not the first primary record.
        // Exercise the processor's actual click ownership and state callbacks.
        for(const auto model : {emu88Lib::DeviceModel::Sc88, emu88Lib::DeviceModel::Sc88VL})
        {
            emu88Player::Processor source;
            CHECK(source.setDeviceModel(model));
            source.setRateAndBufferSizeDetails(44100, 128);
            source.prepareToPlay(44100, 128);
            const auto baseline = save(source);
            const auto baselineHardware = hardware(baseline);
            const auto selectedAddress = model == emu88Lib::DeviceModel::Sc88 ? 0x56f0 : 0x56fa;
            const auto part = (size_t{baselineHardware.memory.at(selectedAddress)} << 8) |
                               baselineHardware.memory.at(selectedAddress + 1);
            CHECK(part < 32);
            const auto levelAddress = (part < 16 ? 0x8088 : 0x9588) + (part % 16) * 0x70 + 8;
            const auto level = baselineHardware.memory.at(levelAddress);
            const auto right = emu88Lib::buttonBit(emu88Lib::Button::LevelR);
            source.clickPanelButton(right, 0);
            source.clickPanelButton(right, 0);
            const auto clicked = save(source);
            CHECK(hardware(clicked).model == model);
            CHECK_EQ(hardware(clicked).memory.at(levelAddress), level + 2);
            emu88Player::Processor live;
            load(live, baseline);
            live.setRateAndBufferSizeDetails(44100, 128);
            live.prepareToPlay(44100, 128);
            live.clickPanelButton(right, 0);
            live.clickPanelButton(right, 0);
            render(live, 16384);
            CHECK_EQ(hardware(save(live)).memory.at(levelAddress), level + 2);
            emu88Player::Processor restored;
            load(restored, clicked);
            restored.setRateAndBufferSizeDetails(44100, 128);
            restored.prepareToPlay(44100, 128);
            render(restored, 128); // Invalidate retained bytes; inspect fresh hardware capture.
            CHECK_EQ(hardware(save(restored)).memory.at(levelAddress), level + 2);
            restored.clickPanelButton(right, 0);
            CHECK_EQ(hardware(save(restored)).memory.at(levelAddress), level + 3);
            const auto fixture = juce::File(emu88Player::defaultDataFolder()).getChildFile(
                "panel-model-" + juce::String(static_cast<int>(model)) + ".component");
            CHECK(fixture.replaceWithData(clicked.getData(), clicked.getSize()));
            std::cout << "family processor immediate panel recall model=" << static_cast<int>(model) << '\n';
        }
    }

    void familyEqMenuRecall()
    {
        // Roland SC-88 owner's manual, Ch. 3 Equalizer setting procedure:
        // ALL then USER INST EDIT SELECT opens the global Gain/Frequency menu.
        // The exact SC-88/VL page and cursor are intentionally learned from
        // the native processor fixture before fresh-board restoration expands.
        const auto eqValues=[](const std::string& text)
        {
            // Compare the displayed signed EQ numbers, excluding the cursor
            // and animated panel glyphs around them.
            std::string values;
            for(const char character : text.substr(std::string("ALLEQ Gain").size(),10))
                if((character>='0' && character<='9') || character=='+' || character=='-')
                    values.push_back(character);
            return values;
        };
        for(const auto model : {emu88Lib::DeviceModel::Sc88, emu88Lib::DeviceModel::Sc88VL})
        {
            emu88Player::Processor source;
            CHECK(source.setDeviceModel(model));
            source.setRateAndBufferSizeDetails(44100,128);
            source.prepareToPlay(44100,128);
            // Roland's SC-88 EQ procedure first enables EQ for the part.
            // The global gain controls may remain inert while it is off.
            source.clickPanelButton(emu88Lib::buttonBit(emu88Lib::Button::Eq),0);
            render(source,44100/2);
            const auto normal=hardware(save(source)).memory;
            source.clickPanelButton(emu88Lib::buttonBit(emu88Lib::Button::InstAll),0);
            render(source,44100/2);
            const auto beforeSelect=source.hardwareDisplaySnapshot();
            CHECK(beforeSelect.has_value());
            if(beforeSelect && !beforeSelect->screens[0].text.empty())
                std::cout << "ALL before SELECT model=" << static_cast<int>(model)
                          << " caption=" << beforeSelect->screens[0].text.front()
                          << " captureBoundary=" << source.hardware()->isSettingsBoundary() << '\n';
            source.clickPanelButton(emu88Lib::buttonBit(emu88Lib::Button::Select),0);
            // Unlike Save's private execution clone, live panel publication
            // advances only when this processor renders. The owner manual's
            // "ALL EQ Gain" caption is the native entry oracle.
            bool eqVisible=false;
            for(int frames{};frames<44100*2 && !eqVisible;frames+=128)
            {
                render(source,128);
                const auto display=source.hardwareDisplaySnapshot();
                if(display && !display->screens[0].text.empty())
                {
                    const auto& line=display->screens[0].text.front();
                    eqVisible=line.rfind("ALLEQ Gain",0)==0 && eqValues(line)=="00";
                }
            }
            CHECK(eqVisible);
            if(!eqVisible) continue;
            const auto opened=save(source);
            CHECK(hardware(opened).model==model);
            const auto contextBase=model==emu88Lib::DeviceModel::Sc88?size_t{0x54d0}:size_t{0x54d8};
            const auto printContext=[&](const char* phase,const std::vector<uint8_t>& memory)
            {
                std::cout << "EQ context model=" << static_cast<int>(model)
                          << " phase=" << phase << " 54dX=";
                for(size_t offset{};offset<8;++offset)
                    std::cout << unsigned(memory.at(contextBase+offset)) << ',';
                std::cout << '\n';
            };
            printContext("normal",normal);
            printContext("opened",hardware(opened).memory);
            const auto sourceDisplay=source.hardwareDisplaySnapshot();
            CHECK(sourceDisplay.has_value());
            emu88Player::Processor restored;
            load(restored,opened);
            restored.setRateAndBufferSizeDetails(44100,128);
            restored.prepareToPlay(44100,128);
            render(restored,128);
            printContext("restored",hardware(save(restored)).memory);
            const auto restoredDisplay=restored.hardwareDisplaySnapshot();
            CHECK(restoredDisplay.has_value());
            if(sourceDisplay && restoredDisplay)
            {
                const auto& nativeText=sourceDisplay->screens[0].text;
                const auto& reopenedText=restoredDisplay->screens[0].text;
                std::cout << "EQ display model=" << static_cast<int>(model)
                          << " native=" << (nativeText.empty()?std::string{}:nativeText.front())
                          << " restored=" << (reopenedText.empty()?std::string{}:reopenedText.front()) << '\n';
                // Character columns beyond 20 include animated meter glyphs.
                CHECK(!nativeText.empty() && !reopenedText.empty());
                if(!nativeText.empty() && !reopenedText.empty())
                    CHECK(nativeText.front().substr(0,20)==reopenedText.front().substr(0,20));
            }
            // Compare the real-ROM low-gain edit with the fresh restoration.
            const auto nativeBefore=hardware(save(source)).memory;
            const auto reopenedBefore=hardware(save(restored)).memory;
            // SC ALL-mode table 07:A4BA routes physical key29 through its
            // descriptor writer. The real-ROM candidate fixture showed it
            // changes the displayed low gain 0→+1 on both SC and VL.
            const auto edit=emu88Lib::buttonBit(emu88Lib::Button::VibDepthR);
            const auto pressEdit=[edit](emu88Player::Processor& processor)
            {
                // Use the native held/released scanner path, as the Pro
                // processor panel fixture does, and wait for LCD publication.
                processor.setPanelButtons(edit);
                render(processor,4410);
                processor.setPanelButtons(0);
                render(processor,11025);
            };
            pressEdit(source);
            pressEdit(restored);
            const auto nativeAfter=hardware(save(source)).memory;
            const auto reopenedAfter=hardware(save(restored)).memory;
            const auto nativeEditedDisplay=source.hardwareDisplaySnapshot();
            const auto reopenedEditedDisplay=restored.hardwareDisplaySnapshot();
            CHECK(nativeEditedDisplay.has_value() && reopenedEditedDisplay.has_value());
            if(nativeEditedDisplay && reopenedEditedDisplay)
            {
                std::cout << "EQ first edit model=" << static_cast<int>(model)
                          << " before=" << sourceDisplay->screens[0].text.front()
                          << " native=" << nativeEditedDisplay->screens[0].text.front()
                          << " restored=" << reopenedEditedDisplay->screens[0].text.front() << '\n';
                CHECK(eqValues(nativeEditedDisplay->screens[0].text.front())==
                      eqValues(reopenedEditedDisplay->screens[0].text.front()));
                CHECK(eqValues(nativeEditedDisplay->screens[0].text.front())!=
                      eqValues(sourceDisplay->screens[0].text.front()));
            }
            size_t nativeChanges{}, mismatchedEdits{};
            const auto compareSpan=[&](size_t first,size_t last)
            {
                for(size_t address=first;address<last;++address)
                {
                    const bool nativeChanged=nativeBefore[address]!=nativeAfter[address];
                    const bool reopenedChanged=reopenedBefore[address]!=reopenedAfter[address];
                    nativeChanges+=nativeChanged;
                    mismatchedEdits+=nativeChanged!=reopenedChanged ||
                                     (nativeChanged && nativeAfter[address]!=reopenedAfter[address]);
                }
            };
            // Explicit System/effect spans preserved by both ROM boot gates,
            // source-verified in 88emu-sc88-vl-clone-seam.md.
            compareSpan(0x8040,0x8088);
            compareSpan(0x8788,0x87c8);
            compareSpan(0x9540,0x9588);
            compareSpan(0x9c88,0x9cc8);
            std::cout << "EQ preserved-effect changes=" << nativeChanges << '\n';
            CHECK(nativeChanges>0);
            CHECK_EQ(mismatchedEdits,0);
            // Save a nondefault gain, then start another processor. This
            // distinguishes retaining the EQ value from merely reopening
            // the same native menu after a default-value capture.
            const auto edited=save(source);
            emu88Player::Processor editedRestored;
            load(editedRestored,edited);
            editedRestored.setRateAndBufferSizeDetails(44100,128);
            editedRestored.prepareToPlay(44100,128);
            render(editedRestored,128);
            (void)hardware(save(editedRestored));
            const auto retainedDisplay=editedRestored.hardwareDisplaySnapshot();
            CHECK(retainedDisplay.has_value());
            if(nativeEditedDisplay && retainedDisplay)
                CHECK(eqValues(nativeEditedDisplay->screens[0].text.front())==
                      eqValues(retainedDisplay->screens[0].text.front()));
            pressEdit(source);
            pressEdit(editedRestored);
            (void)hardware(save(source));
            (void)hardware(save(editedRestored));
            const auto secondNativeDisplay=source.hardwareDisplaySnapshot();
            const auto secondRestoredDisplay=editedRestored.hardwareDisplaySnapshot();
            CHECK(secondNativeDisplay.has_value() && secondRestoredDisplay.has_value());
            if(secondNativeDisplay && secondRestoredDisplay)
            {
                std::cout << "EQ second edit model=" << static_cast<int>(model)
                          << " native=" << secondNativeDisplay->screens[0].text.front()
                          << " restored=" << secondRestoredDisplay->screens[0].text.front() << '\n';
                CHECK(eqValues(secondNativeDisplay->screens[0].text.front())==
                      eqValues(secondRestoredDisplay->screens[0].text.front()));
                CHECK(eqValues(secondNativeDisplay->screens[0].text.front())!=
                      eqValues(nativeEditedDisplay->screens[0].text.front()));
            }
            const auto fixture=juce::File(emu88Player::defaultDataFolder()).getChildFile(
                "eq-model-"+juce::String(static_cast<int>(model))+".component");
            CHECK(fixture.replaceWithData(edited.getData(),edited.getSize()));
            std::cout << "family EQ menu recall model=" << static_cast<int>(model) << '\n';
        }
    }

    void proPanelRecall()
    {
        // tools/sc88pro_state_probe.cpp::testPanelMenuRecall establishes this
        // native button sequence and the ROM's Fine Tune menu cursor/part words.
        emu88Player::Processor source;
        CHECK(source.setDeviceModel(emu88Lib::DeviceModel::Sc88Pro));
        source.setRateAndBufferSizeDetails(44100, 128);
        source.prepareToPlay(44100, 128);
        source.config().setValue("audioSetup", "host-owned-routing-marker");
        source.config().setValue("portMidiEnabled", true);
        source.notifyStateChanged();
        const auto press = [&source](emu88Lib::Sc88ProButton button)
        {
            // The native probe holds for 3200 and releases for 8000 samples
            // at 32 kHz; equivalent host durations at this 44.1 kHz fixture.
            constexpr int heldHostFrames = 4410;
            constexpr int releasedHostFrames = 11025;
            source.setPanelButtons(uint32_t{1} << static_cast<unsigned>(button));
            render(source, heldHostFrames);
            source.setPanelButtons(0);
            render(source, releasedHostFrames);
        };
        const auto pressChord = [&source](uint32_t buttons)
        {
            constexpr int heldHostFrames = 4410;
            constexpr int releasedHostFrames = 11025;
            source.setPanelButtons(buttons);
            render(source, heldHostFrames);
            source.setPanelButtons(0);
            render(source, releasedHostFrames);
        };
        press(emu88Lib::Sc88ProButton::PartR);
        pressChord((uint32_t{1} << static_cast<unsigned>(emu88Lib::Sc88ProButton::PartL)) |
                   (uint32_t{1} << static_cast<unsigned>(emu88Lib::Sc88ProButton::PartR)));
        for(int next{}; next < 3; ++next) press(emu88Lib::Sc88ProButton::Sc88Map);
        const auto clicked = save(source);
        CHECK(!juce::String::fromUTF8(static_cast<const char*>(clicked.getData()),
            static_cast<int>(clicked.getSize())).contains("host-owned-routing-marker"));
        const auto settings = hardware(clicked);
        CHECK(settings.model == emu88Lib::DeviceModel::Sc88Pro);
        const auto word = [&settings](size_t address)
        {
            return (unsigned(settings.memory.at(address)) << 8) | settings.memory.at(address + 1);
        };
        CHECK_EQ(settings.memory.at(0x4b47), 2);
        CHECK_EQ(word(0x4c60), 0x40);
        CHECK_EQ(word(0x4d78), 1);
        const auto fixture = juce::File(emu88Player::defaultDataFolder()).getChildFile(
            "panel-model-2.component");
        CHECK(fixture.replaceWithData(clicked.getData(), clicked.getSize()));
        std::cout << "Pro processor Fine Tune menu component emitted\n";
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
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
    try
    {
        if(argc > 1 && std::string(argv[1]) == "--family-panel")
        {
            familyPanelRecall();
            emu88Player::standaloneLaunch = nullptr;
            return test::finish("88emuFamilyPanelRecall");
        }
        if(argc > 1 && std::string(argv[1]) == "--family-eq")
        {
            familyEqMenuRecall();
            emu88Player::standaloneLaunch = nullptr;
            return test::finish("88emuFamilyEqMenuRecall");
        }
        if(argc > 1 && std::string(argv[1]) == "--pro-panel")
        {
            proPanelRecall();
            emu88Player::standaloneLaunch = nullptr;
            return test::finish("88emuProPanelRecall");
        }
        emu88Player::Processor first;
        CHECK(first.hasValidRom());
        CHECK(!first.portMidiEnabled());
        if(argc > 1 && std::string(argv[1]) == "--capture-timing")
        {
            measureConcurrentSave(first);
            emu88Player::standaloneLaunch = nullptr;
            return test::finish("88emuCaptureTiming");
        }
        first.setRateAndBufferSizeDetails(44100, 128);
        first.prepareToPlay(44100, 128);
        juce::MidiBuffer setup;
        setup.addEvent(juce::MidiMessage::programChange(1, 17), 0);
        setup.addEvent(juce::MidiMessage::controllerEvent(1, 7, 53), 31);
        setup.addEvent(juce::MidiMessage::controllerEvent(1, 11, 71), 63);
        setup.addEvent(juce::MidiMessage::pitchWheel(1, 10240), 80);
        setup.addEvent(juce::MidiMessage::controllerEvent(1, 1, 39), 100);
        render(first, 128, setup);
        first.setOutputGain(0.375f);
        first.setOutputLimiterEnabled(true);
        first.setAnalogOutputMode(emu88Lib::AnalogOutputMode::Sc88Pro);
        first.config().setValue("scale", 137);
        first.config().setValue("audioSetup", "first-machine-routing");
        first.notifyStateChanged();
        const auto saved = save(first);
        CHECK(!juce::String::fromUTF8(static_cast<const char*>(saved.getData()), static_cast<int>(saved.getSize())).contains("first-machine-routing"));
        const auto savedHardware = hardware(saved);
        first.setOutputGain(1.5f);
        first.setAnalogOutputMode(emu88Lib::AnalogOutputMode::Off);
        juce::MidiBuffer mutate;
        mutate.addEvent(juce::MidiMessage::programChange(1, 3), 0);
        mutate.addEvent(juce::MidiMessage::controllerEvent(1, 11, 12), 0);
        render(first, 128, mutate);
        CHECK(hardware(save(first)).memory != savedHardware.memory);
        load(first, saved);
        CHECK_EQ(first.outputGain(), 0.375f);
        CHECK(first.outputLimiterEnabled());
        CHECK(first.analogOutputMode() == emu88Lib::AnalogOutputMode::Sc88Pro);
        CHECK_EQ(first.config().getIntValue("scale"), 137);
        CHECK(save(first) == saved);
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        emu88Player::Processor second;
        second.config().setValue("audioSetup", "second-machine-routing");
        auto* stableConfig = &second.config();
        load(second, saved); // No Prepare, editor, process or message loop.
        CHECK(&second.config() == stableConfig);
        CHECK(stableConfig->getValue("audioSetup") == "second-machine-routing");
        CHECK(save(second) == saved);
        second.setRateAndBufferSizeDetails(48000, 64);
        second.prepareToPlay(48000, 64);
        second.releaseResources();
        second.setRateAndBufferSizeDetails(44100, 128);
        second.prepareToPlay(44100, 128);
        CHECK_EQ(second.outputGain(), 0.375f);
        CHECK(render(second, 1024) == 0.0); // Fresh silent timeline.
        // Audited receive fields from Sc88ProSettings and ROM handlers3F19..455A.
        // Reading a newly captured running board prevents the unchanged-blob cache from
        // making a no-op Load appear to work.
        const auto restoredHardware = hardware(save(second));
        for(size_t part = 0; part < 32; ++part)
            for(const auto base : {0xc660, 0xc661, 0xc6a0, 0xc6a1, 0xc6e0, 0xc6e1,
                                  0xc720, 0xc721, 0xc760, 0xc761, 0xc820, 0xc860,
                                  0xc861, 0xc8a0, 0xc8e0, 0xc8e1, 0xc920, 0xc921})
                CHECK_EQ(restoredHardware.memory[base + part * 2], savedHardware.memory[base + part * 2]);
        juce::MidiBuffer note;
        note.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        double energy = render(second, 128, note);
        for(int block = 0; block < 64; ++block) energy += render(second, 128);
        CHECK(energy > 0.0); // The very first new note must be accepted.
        const auto sounding = save(second);
        load(second, sounding);
        CHECK(render(second, 2048) == 0.0); // Held notes are not restored.
        second.setOutputGain(0.125f);
        CHECK_EQ(first.outputGain(), 0.375f);
        auto corrupt = saved;
        static_cast<uint8_t*>(corrupt.getData())[corrupt.getSize() / 2] ^= 1;
        const auto revision = first.restoredStateRevision();
        reject(first, corrupt.getData(), static_cast<int>(corrupt.getSize()));
        CHECK(!first.lastStateOperationSucceeded());
        CHECK_EQ(first.restoredStateRevision(), revision);
        CHECK_EQ(first.outputGain(), 0.375f);
        CHECK(save(first) == saved);
        // Every truncation must reject before replacing a valid instance.
        for(size_t length = 0; length < saved.getSize(); length += 997)
        {
            reject(first, saved.getData(), static_cast<int>(length));
            CHECK(!first.lastStateOperationSucceeded());
            CHECK_EQ(first.restoredStateRevision(), revision);
        }
        juce::MidiBuffer zeroFrame;
        zeroFrame.addEvent(juce::MidiMessage::programChange(1, 42), 0);
        zeroFrame.addEvent(juce::MidiMessage::controllerEvent(1, 11, 31), 0);
        render(first, 0, zeroFrame);
        const auto zeroFrameSaved = save(first);
        const auto zeroFrameHardware = hardware(zeroFrameSaved);
        // Primary handler41B5/probe: expression is the low byte C6E1 of each part
        // word; firmware slot0 is not necessarily MIDI channel1 (fixture uses slot1).
        std::vector<size_t> editedExpression;
        for(size_t part = 0; part < 32; ++part)
        {
            const auto address = 0xc6e1 + part * 2;
            if(savedHardware.memory[address] == 71)
            {
                editedExpression.push_back(address);
                CHECK_EQ(zeroFrameHardware.memory[address], 31);
            }
        }
        CHECK(!editedExpression.empty());
        load(second, zeroFrameSaved);
        render(second, 128);
        const auto zeroFrameRestored = hardware(save(second));
        for(const auto address : editedExpression) CHECK_EQ(zeroFrameRestored.memory[address], 31);
        load(first, saved);
        load(second, saved);
        const auto levelRight = uint32_t{1} << static_cast<unsigned>(emu88Lib::Sc88ProButton::LevelR);
        first.clickPanelButton(levelRight, 0);
        first.clickPanelButton(levelRight, 0);
        const auto clicked = save(first); // Completed clicks, no audio callback since acceptance.
        second.clickPanelButton(levelRight, 0);
        second.clickPanelButton(levelRight, 0);
        render(second, 16384); // Independent live completion uses normal Processor::processBlock.
        const auto liveClicked = save(second);
        CHECK_EQ(selectedLevel(hardware(clicked)), selectedLevel(savedHardware) + 2);
        CHECK_EQ(selectedLevel(hardware(clicked)), selectedLevel(hardware(liveClicked)));
        load(first, clicked);
        render(first, 128);
        CHECK_EQ(selectedLevel(hardware(save(first))), selectedLevel(hardware(clicked)));
        load(first, saved);
        const auto emptyRoms = juce::File(emu88Player::defaultDataFolder()).getChildFile("empty-roms");
        CHECK(emptyRoms.createDirectory().wasOk());
        synthLib::RomLoader::setSearchPath(emptyRoms.getFullPathName().toStdString());
        (void)emu88Lib::RomLoader::rescan();
        load(first, saved);
        CHECK(!first.hasValidRom());
        CHECK(!first.stateDiagnostic().empty());
        CHECK(save(first) == saved);
        CHECK(render(first, 128) == 0.0);
        first.setOutputGain(0.625f);
        const auto missingEdited = save(first);
        CHECK(hardware(missingEdited).memory == savedHardware.memory);
        load(second, missingEdited);
        CHECK_EQ(second.outputGain(), 0.625f);
        CHECK(!second.hasValidRom());
        CHECK(save(second) == missingEdited);
        synthLib::RomLoader::setSearchPath(std::getenv("TUS_TEST_ROM_DIR"));
        (void)emu88Lib::RomLoader::rescan();
        CHECK(first.restartDevice());
        CHECK(first.hasValidRom());
        CHECK_EQ(first.outputGain(), 0.625f);
        CHECK(hardware(save(first)).memory == savedHardware.memory);
        // Accepted selection survives even before the player's next audio callback, and
        // unavailable playlist entries keep their positions without rereading their paths.
        const auto absentA = juce::File(emu88Player::defaultDataFolder()).getChildFile("absent-a.mid").getFullPathName().toStdString();
        const auto absentB = juce::File(emu88Player::defaultDataFolder()).getChildFile("absent-b.mid").getFullPathName().toStdString();
        first.midiPlayer().replaceFiles({absentA, absentB});
        first.midiPlayer().play(1);
        const auto selected = save(first); // No explicit processor notify: revision guard.
        load(second, selected);
        CHECK_EQ(second.midiPlayer().entries().size(), size_t{2});
        CHECK_EQ(second.midiPlayer().status().currentIndex, 1);
        CHECK(second.midiPlayer().status().state == jucePlayer::MidiPlayer::State::Stopped);
        CHECK(save(second) == selected);
        CHECK(second.midiPlayer().move(1, 0));
        const auto moved = save(second); // Selected B moved; no player callback remapped index.
        load(first, moved);
        CHECK_EQ(first.midiPlayer().status().currentIndex, 0);
        CHECK(first.midiPlayer().entries().front().path == absentB);
        CHECK(first.midiPlayer().move(0, 2));
        CHECK(first.midiPlayer().remove(0)); // Remove A before selected B, still no Process.
        const auto removedBefore = save(first);
        load(second, removedBefore);
        CHECK_EQ(second.midiPlayer().status().currentIndex, 0);
        CHECK(second.midiPlayer().entries().front().path == absentB);
        // Supply off is distinct from missing ROM and survives load/save without Prepare.
        first.setPower(false);
        const auto off = save(first);
        load(second, off);
        CHECK(!second.isPoweredOn());
        CHECK(save(second) == off);
        std::cout << "Real callback program/controller/gain/analog, mutation, lifecycle, first-note, silence, isolation and corruption fixtures executed\n";
    }
    catch(const std::exception& error)
    {
        std::cerr << "Recall exception: " << error.what() << '\n';
        CHECK(false);
    }
    emu88Player::standaloneLaunch = nullptr;
    return test::finish("88emuRecall");
}
