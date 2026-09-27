#include "88emuplayer/Emu88Processor.h"
#include "88lib/settingsChunk.h"
#include "88lib/boards/sc88pro.h"
#include "88lib/boards/sc88proSettings.h"
#include "88lib/rom/romloader.h"

#include <cmath>
#include <chrono>
#include <stdexcept>

namespace emu88Player
{
    namespace
    {
        // Initial host schema. XML UTF-8 and decimal attributes have defined byte order;
        // hardware payload retains the existing SettingsChunk version independently.
        constexpr int hostStateVersion = 1;
        // Capture failure guard, not a firmware completion heuristic. Success comes
        // from HardwareDevice's model-specific boundary; use the existing 10 s boot budget.
        constexpr unsigned captureWatchdogSeconds = 10;
        constexpr size_t singleFrame = 1;
        // Ownership manifest: docs/research/88emu-complete-recall-contract.md.
        // New upstream controls must be reviewed here; machine audio/MIDI setup stays local.
        constexpr const char* instancePreferenceKeys[]{
            "outputGain", "outputLimiter", "resamplerMode", "analogOutputMode", "deviceModel", "pcmCardPath",
            "songResetMode", "songGapMs", "factoryResetOnLoad", "fastBoot", "scale", "skinDisplayName",
            "skinFile", "skinFolder", "warnRomHashMismatch", "reloadSkinViaF5", "enableRmlUiDebugger", "forceSoftwareRenderer"};

        std::unique_ptr<juce::XmlElement> instancePreferences(const juce::PropertySet& config)
        {
            const juce::ScopedLock lock(config.getLock());
            juce::PropertySet owned;
            for(const auto* key : instancePreferenceKeys)
                if(config.containsKey(key)) owned.setValue(key, config.getValue(key));
            return owned.createXml("Preferences");
        }

        void exchangeProperties(juce::PropertySet& destination, juce::PropertySet& candidate)
        {
            // Pinned JUCE PropertySet exposes mutable getAllProperties; StringPairArray
            // only exposes const array views and has an allocating copy assignment.
            // These are non-const underlying objects, so removing the view's constness is
            // defined; StringArray::swapWith exchanges owned storage without allocation.
            // Keep the PropertiesFile address stable for existing editor references.
            const juce::ScopedLock lock(destination.getLock());
            auto& live = destination.getAllProperties();
            auto& next = candidate.getAllProperties();
            const_cast<juce::StringArray&>(live.getAllKeys()).swapWith(const_cast<juce::StringArray&>(next.getAllKeys()));
            const_cast<juce::StringArray&>(live.getAllValues()).swapWith(const_cast<juce::StringArray&>(next.getAllValues()));
        }

        juce::String canonical(const juce::XmlElement& xml)
        {
            return xml.toString(juce::XmlElement::TextFormat().singleLine());
        }

        juce::String digest(const juce::String& data)
        {
            return baseLib::MD5(reinterpret_cast<const uint8_t*>(data.toRawUTF8()),
                                static_cast<uint32_t>(data.getNumBytesAsUTF8())).toString();
        }

        juce::String assetIdentity(const std::vector<emu88Lib::SettingsChunk::Digest>& assets)
        {
            juce::StringArray words;
            for(const auto& image : assets)
                for(const auto word : image) words.add(juce::String::toHexString(static_cast<juce::int64>(word)));
            return words.joinIntoString(":");
        }

        void replaceChild(juce::XmlElement& parent, const char* name, std::unique_ptr<juce::XmlElement> child)
        {
            parent.deleteAllChildElementsWithTagName(name);
            parent.addChildElement(child.release());
        }
    }

    void Processor::notifyStateChanged()
    {
        const juce::ScopedLock lock(getCallbackLock());
        m_loadedStateUnchanged = false;
        m_dirtyNotification.store(true);
        triggerAsyncUpdate();
    }

    std::string Processor::stateDiagnostic() const
    {
        const juce::ScopedLock lock(getCallbackLock());
        return m_stateDiagnostic;
    }

    void Processor::setPanelButtons(const uint32_t buttons)
    {
        const juce::ScopedLock lock(getCallbackLock());
        if(m_device) { m_device->setPanelButtons(buttons); notifyStateChanged(); }
    }

    void Processor::turnPanelEncoder(const int32_t detents)
    {
        const juce::ScopedLock lock(getCallbackLock());
        if(m_device) { m_device->turnPanelEncoder(detents); notifyStateChanged(); }
    }

    void Processor::clickPanelButton(const uint32_t pressed, const uint32_t released)
    {
        const juce::ScopedLock lock(getCallbackLock());
        if(m_device) { m_device->clickPanelButton(pressed, released); notifyStateChanged(); }
    }

    std::optional<emu88Lib::HardwareDevice::DisplaySnapshot> Processor::hardwareDisplaySnapshot() const
    {
        const juce::ScopedLock lock(getCallbackLock());
        if(!m_device) return {};
        return m_device->displaySnapshot();
    }

    void Processor::getStateInformation(juce::MemoryBlock& destination)
    {
        std::lock_guard transaction(m_stateTransaction);
        try
        {
            std::unique_ptr<emu88Lib::HardwareDevice> board;
            std::unique_ptr<synthLib::Plugin> engine;
            // Only setStateInformation replaces this envelope, under the same state
            // transaction. Its potentially large loaded playlist can be copied without
            // blocking audio; live mutable controls are captured below.
            auto payload = m_stateEnvelope ? std::make_unique<juce::XmlElement>(*m_stateEnvelope)
                                           : std::make_unique<juce::XmlElement>("Payload");
            jucePlayer::MidiPlayer::PersistentSnapshot playerSnapshot;
            std::vector<uint8_t> cardBytes;
            // Use the existing capture failure budget for continuous model changes;
            // elapsed wall time is never taken as evidence of a coherent snapshot.
            const auto preparationDeadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(captureWatchdogSeconds);
            for(;;)
            {
                const juce::ScopedLock lock(getCallbackLock());
                if(std::chrono::steady_clock::now() >= preparationDeadline)
                    throw std::runtime_error("Capture preparation could not stabilize before its watchdog expired");
                if(m_loadedStateUnchanged && m_loadedState.getSize() &&
                   m_loadedPlaylistRevision == m_midiPlayer.playlistRevision() &&
                   m_loadedPlayerStatusRevision == m_midiPlayer.status().revision &&
                   m_loadedPlayerCommandRevision == m_midiPlayer.commandRevision())
                {
                    m_lastStateSucceeded.store(true);
                    // The state transaction keeps these bytes alive. The cache
                    // decision belongs to this instant, before later audio edits.
                    const juce::ScopedUnlock unlocked(getCallbackLock());
                    destination = m_loadedState;
                    return;
                }
                // Construct buses, chip memory and private compiler workers without
                // holding up audio. Construction inputs own immutable asset copies;
                // no live board pointer escapes this lock. Recheck compatibility in
                // case the user changed models while the shell was being allocated.
                if(m_device && !m_device->isValid())
                    throw std::runtime_error("Cannot capture unavailable hardware without a loaded recovery state");
                if(m_device && (!board || !board->acceptsCaptureFrom(*m_device)))
                {
                    auto prepare = m_device->prepareCaptureClone();
                    const juce::ScopedUnlock unlocked(getCallbackLock());
                    board = prepare();
                    if(!board) throw std::runtime_error("Capture shell construction failed");
                    continue;
                }
                if(!m_device && board)
                {
                    const juce::ScopedUnlock unlocked(getCallbackLock());
                    board.reset();
                    continue;
                }
                payload->setAttribute("model", static_cast<int>(m_deviceModel));
                if(!m_unavailableState) payload->setAttribute("power", m_device != nullptr);
                payload->setAttribute("gain", outputGain());
                payload->setAttribute("limiter", outputLimiterEnabled());
                payload->setAttribute("reverse", m_reverseOutputChannels.load());
                payload->setAttribute("analog", static_cast<int>(m_analogOutputMode));
                payload->setAttribute("resampler", static_cast<int>(resamplerMode()));
                auto preferences = instancePreferences(*m_config);
                replaceChild(*payload, "Preferences", std::move(preferences));
                playerSnapshot = m_midiPlayer.capturePersistentState();
                cardBytes = m_pcmCard;
                if(m_device)
                {
                    // Hosted instances do not acquire physical input. Refuse rather than
                    // destructively draining a standalone collector at a different wall time.
                    if(m_liveMidiPending) throw std::runtime_error("Standalone physical input is pending; collector capture is not implemented");
                    if(!board->copyCaptureFrom(*m_device)) throw std::runtime_error("Exact board clone rejected");
                    engine = m_engine->cloneForCapture(board.get());
                    payload->setAttribute("assets", assetIdentity(m_device->assetDigests()));
                }
                else if(!m_unavailableState) payload->deleteAllChildElementsWithTagName("Hardware");
                break;
            }
            replaceChild(*payload, "Player", playerSnapshot.toXml());
            auto card = std::make_unique<juce::XmlElement>("Card");
            card->addTextElement(juce::MemoryBlock(cardBytes.data(), cardBytes.size()).toBase64Encoding());
            replaceChild(*payload, "Card", std::move(card));
            if(board)
            {
                // Accepted upstream input runs through the copied converter, preserving
                // native phase and block-relative offsets. Afterwards drain one native output
                // frame at a time so a completed firmware boundary cannot be skipped.
                const auto rate = engine->getHostSamplerate() > float{} ? engine->getHostSamplerate() : board->getSamplerate();
                const auto watchdog = static_cast<uint64_t>(rate) * captureWatchdogSeconds;
                uint64_t rendered{};
                while(engine->hasPendingCaptureInput() || !board->isSettingsBoundary())
                {
                    if(rendered >= watchdog) throw std::runtime_error("Capture watchdog expired before firmware acknowledgment");
                    if(engine->hasPendingCaptureInput())
                    {
                        const auto quantum = engine->captureInputQuantum();
                        engine->processCapture(quantum);
                        rendered += quantum;
                    }
                    else { board->advanceCaptureFrame(); ++rendered; }
                }
                if(engine->hasIncompleteCaptureSysex()) throw std::runtime_error("Cannot capture an incomplete accepted SysEx message");
                auto hardware = std::make_unique<juce::XmlElement>("Hardware");
                const auto bytes = board->captureSettings().encode();
                if(bytes.empty()) throw std::runtime_error("Hardware settings encoding failed");
                hardware->addTextElement(juce::MemoryBlock(bytes.data(), bytes.size()).toBase64Encoding());
                replaceChild(*payload, "Hardware", std::move(hardware));
            }
            juce::XmlElement envelope("Emu88State");
            envelope.setAttribute("version", hostStateVersion);
            envelope.setAttribute("checksum", digest(canonical(*payload)));
            envelope.addChildElement(payload.release());
            const auto bytes = canonical(envelope);
            juce::MemoryBlock completed(bytes.toRawUTF8(), bytes.getNumBytesAsUTF8());
            destination = std::move(completed);
            const juce::ScopedLock lock(getCallbackLock());
            m_lastStateSucceeded.store(true);
            m_stateDiagnostic.clear();
        }
        catch(const std::exception& error)
        {
            const juce::ScopedLock lock(getCallbackLock());
            m_lastStateSucceeded.store(false);
            m_stateDiagnostic = error.what();
            // JUCE's callback is void. Propagate rather than silently returning an empty or
            // stale state; the native wrapper must translate this into a failed host save.
            throw;
        }
    }

    void Processor::setStateInformation(const void* data, const int size)
    {
        std::lock_guard transaction(m_stateTransaction);
        try
        {
            if(!data || size <= int{}) throw std::runtime_error("Empty preview state contains no recoverable edits");
            auto envelope = juce::parseXML(juce::String::fromUTF8(static_cast<const char*>(data), size));
            if(!envelope || !envelope->hasTagName("Emu88State") || envelope->getIntAttribute("version") != hostStateVersion)
                throw std::runtime_error("Unsupported or malformed 88emu state envelope");
            const auto* payload = envelope->getChildByName("Payload");
            if(!payload || envelope->getStringAttribute("checksum") != digest(canonical(*payload)))
                throw std::runtime_error("88emu state checksum mismatch");
            const auto model = payload->getIntAttribute("model", -1);
            const auto analog = payload->getIntAttribute("analog", -1);
            const auto mode = payload->getIntAttribute("resampler", -1);
            const auto gain = payload->getDoubleAttribute("gain", -1);
            if(model < int{} || !emu88Lib::isDeviceModelValue(static_cast<uint32_t>(model)) ||
               analog < int{} || !emu88Lib::isAnalogOutputModeValue(static_cast<uint32_t>(analog)) ||
               mode < int{} || mode >= static_cast<int>(synthLib::Resampler::Mode::Count) ||
               !std::isfinite(gain) || gain < kMinimumOutputGain || gain > kMaximumOutputGain)
                throw std::runtime_error("Invalid software settings");
            const auto* preferences = payload->getChildByName("Preferences");
            const auto* player = payload->getChildByName("Player");
            const auto* card = payload->getChildByName("Card");
            jucePlayer::MidiPlayer candidatePlayer;
            juce::MemoryBlock cardBytes;
            if(!preferences || !player || !candidatePlayer.loadPersistentState(*player) || !card ||
               !cardBytes.fromBase64Encoding(card->getAllSubText())) throw std::runtime_error("Invalid player/preferences/card state");
            const auto* cardBegin = static_cast<const uint8_t*>(cardBytes.getData());
            std::vector<uint8_t> candidateCard;
            if(cardBytes.getSize()) candidateCard.assign(cardBegin, cardBegin + cardBytes.getSize());
            if(!candidateCard.empty() && !emu88Lib::HardwareDevice::isPcmCardImage(candidateCard))
                throw std::runtime_error("Invalid retained PCM card");
            std::unique_ptr<emu88Lib::HardwareDevice> device;
            std::unique_ptr<synthLib::Plugin> engine;
            const bool power = payload->getBoolAttribute("power");
            bool unavailable = false;
            if(power)
            {
                const auto* hardware = payload->getChildByName("Hardware");
                juce::MemoryBlock hardwareBytes;
                if(!hardware || !hardwareBytes.fromBase64Encoding(hardware->getAllSubText()))
                    throw std::runtime_error("Invalid hardware state");
                const auto settings = emu88Lib::SettingsChunk::decode(hardwareBytes.getData(), hardwareBytes.getSize());
                if(!settings || static_cast<int>(settings->model) != model)
                    throw std::runtime_error("Hardware identity mismatch");
                if(!emu88Lib::HardwareDevice::supportsSettingsImage(*settings))
                    throw std::runtime_error("Required model/firmware adapter is unsupported");
                emu88Lib::BootOptions boot;
                boot.factoryReset = false;
                boot.fastBoot = false;
                device = std::make_unique<emu88Lib::HardwareDevice>(createDeviceParams(settings->model), boot, candidateCard, &*settings);
                if(payload->getStringAttribute("assets").isEmpty()) throw std::runtime_error("Missing asset manifest");
                unavailable = device->assetDigests().empty() || payload->getStringAttribute("assets") != assetIdentity(device->assetDigests());
                if(!unavailable && !device->isValid()) throw std::runtime_error("Firmware adapter rejected the hardware image");
                if(unavailable) device.reset();
                else
                {
                    device->setAnalogOutputMode(static_cast<emu88Lib::AnalogOutputMode>(analog));
                    engine = std::make_unique<synthLib::Plugin>(device.get(), [](synthLib::Device*) { return nullptr; });
                    engine->setMidiClockEnabled(false);
                    engine->setLatencyBlocks(0);
                    engine->setResamplerMode(static_cast<synthLib::Resampler::Mode>(mode));
                }
            }
            // Allocate recovery data before publishing anything; malformed input leaves live
            // state untouched. A valid missing-asset state explicitly replaces it with silence.
            juce::MemoryBlock original(data, static_cast<size_t>(size));
            auto retained = std::make_unique<juce::XmlElement>(*payload);
            juce::PropertySet requestedPreferences;
            requestedPreferences.restoreFromXml(*preferences);
            juce::PropertySet candidateConfig;
            {
                const juce::ScopedLock lock(getCallbackLock());
                const juce::ScopedLock propertyLock(m_config->getLock());
                candidateConfig = *m_config;
            }
            for(const auto* key : instancePreferenceKeys)
            {
                if(requestedPreferences.containsKey(key)) candidateConfig.setValue(key, requestedPreferences.getValue(key));
                else candidateConfig.removeValue(key);
            }
            std::string diagnostic = unavailable ? "Required ROM assets are unavailable or have different content; original state retained" : "";
            std::unique_ptr<emu88Lib::HardwareDevice> oldDevice;
            std::unique_ptr<synthLib::Plugin> oldEngine;
            {
                const juce::ScopedLock lock(getCallbackLock());
                // Prepare can race the slow private boot. Build converter state outside the
                // callback lock, then retry against the latest lifecycle generation before
                // publishing. The final generation check and swap share the same lock.
                if(engine)
                {
                    bool prepared = false;
                    uint64_t generation{};
                    while(!prepared || generation != m_lifecycleGeneration)
                    {
                        generation = m_lifecycleGeneration;
                        const auto rate = m_preparedSampleRate;
                        const auto block = m_preparedBlockSize;
                        {
                            const juce::ScopedUnlock unlocked(getCallbackLock());
                            if(rate > double{}) engine->setHostSamplerate(static_cast<float>(rate), float{});
                            if(block > int{}) engine->setBlockSize(static_cast<uint32_t>(block));
                        }
                        prepared = true;
                    }
                }
                oldEngine = std::move(m_engine);
                oldDevice = std::move(m_device);
                m_device = std::move(device);
                m_engine = std::move(engine);
                m_deviceModel = static_cast<emu88Lib::DeviceModel>(model);
                exchangeProperties(*m_config, candidateConfig);
                m_pcmCard = std::move(candidateCard);
                m_outputGain.store(static_cast<float>(gain));
                m_limiterEnabled.store(payload->getBoolAttribute("limiter"));
                m_reverseOutputChannels.store(payload->getBoolAttribute("reverse"));
                m_analogOutputMode = static_cast<emu88Lib::AnalogOutputMode>(analog);
                m_resamplerMode.store(mode);
                m_outputLimiter.prepare(getSampleRate());
                m_midiPlayer.installPersistentState(candidatePlayer);
                m_loadedPlaylistRevision = m_midiPlayer.playlistRevision();
                m_loadedPlayerStatusRevision = m_midiPlayer.status().revision;
                m_loadedPlayerCommandRevision = m_midiPlayer.commandRevision();
                m_midiPlayer.setPortCount(midiPortCount());
                m_midiPlayer.setResetTarget(emu88Lib::resetTarget(m_deviceModel));
                for(auto& collector : m_liveMidi) collector.reset(std::max(getSampleRate(), static_cast<double>(emu88Lib::g_sampleRate)));
                m_liveMidiPending = false;
                m_loadedState = std::move(original);
                m_stateEnvelope = std::move(retained);
                m_loadedStateUnchanged = true;
                m_unavailableState = unavailable;
                m_stateDiagnostic = std::move(diagnostic);
                m_dirtyNotification.store(false);
                m_lastStateSucceeded.store(true);
                m_restoredStateRevision.fetch_add(singleFrame);
            }
            oldEngine.reset();
            oldDevice.reset();
        }
        catch(const std::exception& error)
        {
            const juce::ScopedLock lock(getCallbackLock());
            m_stateDiagnostic = error.what();
            m_lastStateSucceeded.store(false);
            throw;
        }
    }
}
