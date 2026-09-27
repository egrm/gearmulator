#pragma once

#include "JuceHeader.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace hostRecallTest
{
	// Deliberately awkward test fixtures exercise resampling across host block boundaries.
	inline constexpr double sampleRate = 44100.0;
	inline constexpr int blockSize = 257;
	inline constexpr int settleBlocks = 400;
	inline constexpr int auditionBlocks = 120;
	// Emu88Recall_test.cpp familyPanelRecall: the selected internal part is
	// stored at these firmware words; visible A1 is internal slot 1.
	inline constexpr size_t sc88SelectedPartWord = 0x56f0;
	inline constexpr size_t sc88VlSelectedPartWord = 0x56fa;
	inline constexpr size_t sc88PartsPerGroup = 16;
	inline constexpr size_t sc88PartRecordBytes = 0x70;
	inline constexpr size_t sc88FirstGroupBase = 0x8088;
	inline constexpr size_t sc88SecondGroupBase = 0x9588;
	inline constexpr size_t sc88PartLevelOffset = 8;
	// sc88Settings_test.cpp: native GS DT1 40 00 04 writes SRAM 8042.
	inline constexpr size_t sc88MasterVolumeAddress = 0x8042;
	inline constexpr uint8_t sc88VolumeFixture = 31;
	// tools/sc88pro_state_probe.cpp::testPanelMenuRecall: Fine Tune context.
	inline constexpr size_t proPanelPageAddress = 0x4b47;
	inline constexpr size_t proFineTuneCursorAddress = 0x4c60;
	inline constexpr size_t proSelectedPartAddress = 0x4d78;
	// Emu88Recall_test.cpp familyEqMenuRecall and eq-gain-final-context.log:
	// native ALL EQ Gain mode and selector context.
	inline constexpr size_t sc88EqContextAddress = 0x54d0;
	inline constexpr size_t sc88VlEqContextAddress = 0x54d8;
	inline constexpr size_t eqModeRequestBytes = 2;
	inline constexpr size_t eqSelectorOffset = 6;
	inline constexpr uint8_t sc88AllEqPage = 1;
	inline constexpr uint8_t sc88VlAllEqPage = 2;
	inline constexpr uint8_t eqGainSelector = 0;
	// Emu88Recall_test.cpp familyEqMenuRecall and
	// recall-family-processor-eq.log: native key29 edits low EQ gain 64 to 65.
	inline constexpr size_t sc88EqLowGainAddress = 0x8789;
	inline constexpr uint8_t editedEqLowGain = 65;

	inline void require(bool condition, const char* description)
	{
		if(!condition)
			throw std::runtime_error(description);
		Logger::writeToLog(String("PASS: ") + description);
	}

	inline std::unique_ptr<AudioPluginInstance> create(AudioPluginFormatManager& manager,
		const PluginDescription& description)
	{
		String error;
		auto plugin = manager.createPluginInstance(description, sampleRate, blockSize, error);
		if(!plugin)
			throw std::runtime_error("Cannot create recall test instance: " + error.toStdString());
		plugin->setNonRealtime(true);
		plugin->setRateAndBufferSizeDetails(sampleRate, blockSize);
		plugin->prepareToPlay(sampleRate, blockSize);
		return plugin;
	}

	inline std::vector<float> render(AudioProcessor& processor, MidiBuffer messages, int blocks)
	{
		AudioBuffer<float> buffer(processor.getTotalNumOutputChannels(), blockSize);
		std::vector<float> audio;
		audio.reserve(static_cast<size_t>(blocks * blockSize * buffer.getNumChannels()));
		for(int block = 0; block < blocks; ++block)
		{
			buffer.clear();
			processor.processBlock(buffer, messages);
			messages.clear();
			for(int channel = 0; channel < buffer.getNumChannels(); ++channel)
				for(int sample = 0; sample < buffer.getNumSamples(); ++sample)
				{
					const auto value = buffer.getSample(channel, sample);
					if(!std::isfinite(value))
						throw std::runtime_error("Recall produced non-finite audio");
					audio.push_back(value);
				}
		}
		return audio;
	}

	inline MemoryBlock save(AudioProcessor& processor)
	{
		MemoryBlock result;
		processor.getStateInformation(result);
		require(result.getSize() != 0, "VST3 component returns nonempty state");
		return result;
	}

	inline MemoryBlock componentStateFromHost(const MemoryBlock& state)
	{
		// JUCE's hosted VST3 instance wraps the component stream in an XML binary
		// block. A wrapper with no IComponent can be nonempty after a failed save.
		auto hostEnvelope = AudioProcessor::getXmlFromBinary(state.getData(),
			static_cast<int>(state.getSize()));
		require(hostEnvelope != nullptr && hostEnvelope->hasTagName("VST3PluginState"),
			"Hosted VST3 state has JUCE's component envelope");
		const auto* component = hostEnvelope->getChildByName("IComponent");
		require(component != nullptr, "Hosted VST3 captured an IComponent stream");
		MemoryBlock componentState;
		require(componentState.fromBase64Encoding(component->getAllSubText()),
			"Hosted VST3 state contains a decodable component stream");
		return componentState;
	}

	inline void requireModel(const MemoryBlock& state, int expectedModel)
	{
		if(expectedModel < 0) return;
		const auto componentState = componentStateFromHost(state);
		auto envelope = parseXML(String::fromUTF8(static_cast<const char*>(componentState.getData()),
			static_cast<int>(componentState.getSize())));
		require(envelope != nullptr && envelope->hasTagName("Emu88State"), "VST3 component has an 88emu envelope");
		const auto* payload = envelope->getChildByName("Payload");
		require(payload && payload->getIntAttribute("model", -1) == expectedModel,
			"VST3 state retains the selected hardware model");
		require(payload->getChildByName("Hardware") != nullptr,
			"VST3 state includes the selected model's hardware settings");
	}

	inline void requirePowerOff(const MemoryBlock& state)
	{
		const auto component = componentStateFromHost(state);
		auto envelope = parseXML(String::fromUTF8(static_cast<const char*>(component.getData()),
			static_cast<int>(component.getSize())));
		const auto* payload = envelope ? envelope->getChildByName("Payload") : nullptr;
		require(payload != nullptr && !payload->getBoolAttribute("power") &&
			payload->getChildByName("Hardware") != nullptr &&
			payload->getStringAttribute("assets").isNotEmpty(),
			"VST3 retains powered-off hardware settings and asset identity");
	}

	inline uint8_t sc88HardwareByte(const MemoryBlock& componentState, size_t address)
	{
		auto envelope = parseXML(String::fromUTF8(static_cast<const char*>(componentState.getData()),
			static_cast<int>(componentState.getSize())));
		require(envelope != nullptr && envelope->hasTagName("Emu88State"),
			"Panel seed has an 88emu component envelope");
		const auto* payload = envelope->getChildByName("Payload");
		const auto* hardware = payload ? payload->getChildByName("Hardware") : nullptr;
		require(hardware != nullptr, "Panel seed contains hardware settings");
		MemoryBlock chunk;
		require(chunk.fromBase64Encoding(hardware->getAllSubText()),
			"Panel seed hardware chunk decodes");
		// settingsChunk.cpp: 4CC, version, length, model, layout, four MD5
		// words, and memory length precede the raw SRAM image.
		constexpr size_t chunkHeaderBytes = 40;
		require(chunk.getSize() > chunkHeaderBytes + address &&
			std::memcmp(chunk.getData(), "88HW", sizeof("88HW") - 1) == 0,
			"Panel seed has a complete 88HW settings chunk");
		return static_cast<const uint8_t*>(chunk.getData())[chunkHeaderBytes + address];
	}

	inline size_t sc88SelectedPart(const MemoryBlock& componentState, int model)
	{
		const auto word = model == 0 ? sc88SelectedPartWord : sc88VlSelectedPartWord;
		const auto part = (size_t{sc88HardwareByte(componentState, word)} << 8) |
			sc88HardwareByte(componentState, word + 1);
		require(part < sc88PartsPerGroup * 2, "Native panel selects a valid internal part");
		return part;
	}

	inline void requireSc88PanelLevel(const MemoryBlock& actual, const MemoryBlock& seed,
		int model, const char* description)
	{
		const auto part = sc88SelectedPart(seed, model);
		require(sc88SelectedPart(actual, model) == part,
			"VST3 retains the native panel's selected internal part");
		const auto base = part < sc88PartsPerGroup ? sc88FirstGroupBase : sc88SecondGroupBase;
		const auto address = base + (part % sc88PartsPerGroup) * sc88PartRecordBytes +
			sc88PartLevelOffset;
		require(sc88HardwareByte(actual, address) == sc88HardwareByte(seed, address), description);
	}

	inline void requireSc88EqContext(const MemoryBlock& actual, const MemoryBlock& seed,
		int model, const char* description)
	{
		const auto context = model == 0 ? sc88EqContextAddress : sc88VlEqContextAddress;
		const auto allPage = model == 0 ? sc88AllEqPage : sc88VlAllEqPage;
		require(sc88HardwareByte(seed, context) == allPage &&
			sc88HardwareByte(seed, context + 1) == allPage &&
			sc88HardwareByte(seed, context + eqSelectorOffset) == eqGainSelector &&
			sc88HardwareByte(seed, sc88EqLowGainAddress) == editedEqLowGain,
			"Native EQ seed is on ALL EQ Gain selector zero with edited low gain");
		for(size_t offset = 0; offset < eqModeRequestBytes; ++offset)
			require(sc88HardwareByte(actual, context + offset) ==
				sc88HardwareByte(seed, context + offset), description);
		require(sc88HardwareByte(actual, context + eqSelectorOffset) == eqGainSelector,
			"VST3 retains the native ALL EQ Gain selector");
		require(sc88HardwareByte(actual, sc88EqLowGainAddress) == editedEqLowGain,
			"VST3 retains the native low EQ gain edit");
	}

	inline void requireSc88NativeSeed(const MemoryBlock& actual, const MemoryBlock& seed,
		int model, bool eqSeed, const char* levelDescription, const char* eqDescription)
	{
		if(eqSeed)
			requireSc88EqContext(actual, seed, model, eqDescription);
		else
			requireSc88PanelLevel(actual, seed, model, levelDescription);
	}

	inline unsigned hardwareWord(const MemoryBlock& componentState, size_t address)
	{
		return (unsigned{sc88HardwareByte(componentState, address)} << 8) |
			sc88HardwareByte(componentState, address + 1);
	}

	inline void requireProPanelContext(const MemoryBlock& actual, const MemoryBlock& seed,
		const char* description)
	{
		require(sc88HardwareByte(seed, proPanelPageAddress) == 2 &&
			hardwareWord(seed, proFineTuneCursorAddress) == 0x40 &&
			hardwareWord(seed, proSelectedPartAddress) == 1,
			"Native Pro seed selects A2 Fine Tune menu");
		require(sc88HardwareByte(actual, proPanelPageAddress) ==
			sc88HardwareByte(seed, proPanelPageAddress) &&
			hardwareWord(actual, proFineTuneCursorAddress) ==
			hardwareWord(seed, proFineTuneCursorAddress) &&
			hardwareWord(actual, proSelectedPartAddress) ==
			hardwareWord(seed, proSelectedPartAddress), description);
	}

	inline void requireHostOwnedRouting(const MemoryBlock& componentState)
	{
		auto envelope = parseXML(String::fromUTF8(static_cast<const char*>(componentState.getData()),
			static_cast<int>(componentState.getSize())));
		const auto* payload = envelope ? envelope->getChildByName("Payload") : nullptr;
		const auto* preferences = payload ? payload->getChildByName("Preferences") : nullptr;
		require(preferences != nullptr, "VST3 component includes instance preferences");
		for(const auto* key : {"audioSetup", "portMidiEnabled", "virtualPortName"})
			require(preferences->getChildByAttribute("name", key) == nullptr,
				"VST3 excludes host-owned audio and physical MIDI routing");
	}

	inline MidiBuffer auditionNotes()
	{
		MidiBuffer notes;
		notes.addEvent(MidiMessage::noteOn(1, 60, static_cast<uint8>(96)), 13);
		return notes;
	}

	inline int runPowerOff(const PluginDescription& description, const String& componentPath,
		const String& fixturePath, int expectedModel)
	{
		MemoryBlock component;
		require(File(componentPath).loadFileAsData(component), "Read native powered-off component state");
		XmlElement wrapper("VST3PluginState");
		wrapper.createNewChildElement("IComponent")->addTextElement(component.toBase64Encoding());
		MemoryBlock wrapped;
		AudioProcessor::copyXmlToBinary(wrapper, wrapped);
		AudioPluginFormatManager manager;
		manager.addDefaultFormats();
		auto source = create(manager, description);
		source->setStateInformation(wrapped.getData(), static_cast<int>(wrapped.getSize()));
		const auto initial = save(*source);
		requireModel(initial, expectedModel);
		requirePowerOff(initial);
		const auto silent = render(*source, auditionNotes(), auditionBlocks);
		require(std::all_of(silent.begin(), silent.end(), [](float sample) { return sample == 0.f; }),
			"Powered-off VST3 ignores notes and renders silence");
		const auto saved = save(*source);
		requirePowerOff(saved);
		source->releaseResources();
		source.reset();
		auto restored = create(manager, description);
		restored->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		require(save(*restored) == saved, "Powered-off VST3 restores before audio processing");
		const auto reopened = render(*restored, auditionNotes(), auditionBlocks);
		require(reopened == silent, "Powered-off VST3 stays silent after instance destruction");
		restored->releaseResources();
		require(File(fixturePath).replaceWithData(saved.getData(), saved.getSize()),
			"Write powered-off cross-process state fixture");
		return 0;
	}

	inline int readFreshPowerOff(const PluginDescription& description, const String& fixturePath,
		int expectedModel)
	{
		MemoryBlock saved;
		require(File(fixturePath).loadFileAsData(saved), "Read powered-off prior-process state");
		requireModel(saved, expectedModel);
		requirePowerOff(saved);
		AudioPluginFormatManager manager;
		manager.addDefaultFormats();
		auto restored = create(manager, description);
		restored->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		require(save(*restored) == saved, "Fresh process preserves powered-off VST3 state");
		const auto audio = render(*restored, auditionNotes(), auditionBlocks);
		require(std::all_of(audio.begin(), audio.end(), [](float sample) { return sample == 0.f; }),
			"Fresh process keeps powered-off VST3 silent");
		restored->releaseResources();
		return 0;
	}

	inline MidiMessage sc88MasterVolumeEdit()
	{
		// Both SC-88 ROMs' native 40 00 04 DT1 handler is covered by
		// sc88Settings_test.cpp; JUCE adds F0/F7 to this payload.
		uint8_t data[]{0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x04,
			sc88VolumeFixture, 0};
		unsigned sum{};
		for(size_t index = 4; index < std::size(data) - 1; ++index) sum += data[index];
		data[std::size(data) - 1] = static_cast<uint8_t>((128 - (sum & 127)) & 127);
		return MidiMessage::createSysExMessage(data, sizeof(data));
	}

	inline int readFreshProcess(const PluginDescription& description, const String& fixturePath,
		int expectedModel = -1, const String& nativeComponentPath = {}, bool eqSeed = false)
	{
		MemoryBlock saved, expectedAudio;
		require(File(fixturePath).loadFileAsData(saved), "Read prior-process VST3 state");
		requireModel(saved, expectedModel);
		require(File(fixturePath + ".audio").loadFileAsData(expectedAudio), "Read prior-process expected audio");
		AudioPluginFormatManager manager;
		manager.addDefaultFormats();
		auto restored = create(manager, description);
		restored->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		require(save(*restored) == saved, "Fresh process preserves saved VST3 state");
		requireHostOwnedRouting(componentStateFromHost(saved));
		if(nativeComponentPath.isNotEmpty() && (expectedModel == 0 || expectedModel == 1))
		{
			MemoryBlock nativeComponent;
			require(File(nativeComponentPath).loadFileAsData(nativeComponent),
				"Fresh process reads the native panel-edited source image");
				requireSc88NativeSeed(componentStateFromHost(save(*restored)), nativeComponent,
					expectedModel, eqSeed, "Fresh process retains the native panel LevelR value",
					"Fresh process retains the native ALL EQ Gain context");
		}
		if(nativeComponentPath.isNotEmpty() && expectedModel == 2)
		{
			MemoryBlock nativeComponent;
			require(File(nativeComponentPath).loadFileAsData(nativeComponent),
				"Fresh process reads the native Pro panel source image");
			requireProPanelContext(componentStateFromHost(save(*restored)), nativeComponent,
				"Fresh process retains the native Pro Fine Tune menu context");
		}
		const auto audio = render(*restored, auditionNotes(), auditionBlocks);
		require(MemoryBlock(audio.data(), audio.size() * sizeof(float)) == expectedAudio,
			"Fresh process produces identical recalled audio");
		restored->releaseResources();
		return 0;
	}

	inline int run(const PluginDescription& description, const String& fixturePath,
		int expectedModel = -1, const String& nativeComponentPath = {}, bool eqSeed = false)
	{
		AudioPluginFormatManager manager;
		manager.addDefaultFormats();
		auto original = create(manager, description);
		MemoryBlock nativeComponent;
		if(nativeComponentPath.isNotEmpty())
		{
			require(File(nativeComponentPath).loadFileAsData(nativeComponent),
				"Read native panel-edited component state");
			XmlElement hostEnvelope("VST3PluginState");
			hostEnvelope.createNewChildElement("IComponent")
				->addTextElement(nativeComponent.toBase64Encoding());
			MemoryBlock wrapped;
			AudioProcessor::copyXmlToBinary(hostEnvelope, wrapped);
			original->setStateInformation(wrapped.getData(), static_cast<int>(wrapped.getSize()));
		}
		const auto initialState = save(*original);
		requireModel(initialState, expectedModel);
		requireHostOwnedRouting(componentStateFromHost(initialState));
		if(nativeComponentPath.isEmpty() && (expectedModel == 0 || expectedModel == 1))
			require(sc88HardwareByte(componentStateFromHost(initialState),
				sc88MasterVolumeAddress) != sc88VolumeFixture,
				"Native DT1 test starts with a different master volume");
		if(nativeComponentPath.isNotEmpty())
		{
			const auto wrappedComponent = componentStateFromHost(initialState);
			// The JUCE VST3 client appends private bypass data after the processor
			// bytes. The original processor image must remain its exact prefix.
			require(wrappedComponent.getSize() >= nativeComponent.getSize() &&
				std::memcmp(wrappedComponent.getData(), nativeComponent.getData(),
					nativeComponent.getSize()) == 0,
				"Actual VST3 retains the native panel-edited component image");
		}
		MemoryBlock saved;
		if(nativeComponentPath.isNotEmpty())
		{
			// A no-MIDI callback invalidates the loaded-state cache and requires a
			// fresh hardware capture without overwriting the panel's chosen level.
			(void)render(*original, {}, 1);
			saved = save(*original);
			requireModel(saved, expectedModel);
			if(expectedModel == 0 || expectedModel == 1)
				requireSc88NativeSeed(componentStateFromHost(saved), nativeComponent,
					expectedModel, eqSeed, "Native panel LevelR survives a fresh VST3 capture",
					"Native ALL EQ Gain context survives a fresh VST3 capture");
			if(expectedModel == 2)
				requireProPanelContext(componentStateFromHost(saved), nativeComponent,
					"Native Pro Fine Tune menu survives a fresh VST3 capture");
		}
		else
		{
			MidiBuffer edits;
			for(int channel = 1; channel <= 16; ++channel)
			{
				edits.addEvent(MidiMessage::programChange(channel, 17), 11);
				edits.addEvent(MidiMessage::controllerEvent(channel, 7, 43), 23);
				edits.addEvent(MidiMessage::controllerEvent(channel, 10, 21), 37);
				edits.addEvent(MidiMessage::controllerEvent(channel, 11, 95), 59);
			}
			if(expectedModel == 0 || expectedModel == 1)
				edits.addEvent(sc88MasterVolumeEdit(), blockSize - 1);
			// Save on the first callback after accepted MIDI. This catches edits
			// still queued in the host converter.
			(void)render(*original, edits, 1);
			const auto immediateState = save(*original);
			requireModel(immediateState, expectedModel);
			if(expectedModel == 0 || expectedModel == 1)
				require(sc88HardwareByte(componentStateFromHost(immediateState),
					sc88MasterVolumeAddress) == sc88VolumeFixture,
					"Immediate VST3 save includes native DT1 master volume");
			auto immediateRestored = create(manager, description);
			immediateRestored->setStateInformation(immediateState.getData(),
				static_cast<int>(immediateState.getSize()));
			require(save(*immediateRestored) == immediateState,
				"Immediate MIDI save restores before any fresh audio callback");
			const auto immediateAudio = render(*immediateRestored, auditionNotes(), auditionBlocks);
			auto immediateIdleInstance = create(manager, description);
			immediateIdleInstance->setStateInformation(immediateState.getData(),
				static_cast<int>(immediateState.getSize()));
			const auto immediateIdle = render(*immediateIdleInstance, {}, auditionBlocks);
			float immediatePeak{}, immediateNoteDelta{};
			for(size_t sample{}; sample < immediateAudio.size(); ++sample)
			{
				immediatePeak = std::max(immediatePeak, std::abs(immediateAudio[sample]));
				immediateNoteDelta = std::max(immediateNoteDelta,
					std::abs(immediateAudio[sample] - immediateIdle[sample]));
			}
			Logger::writeToLog("Immediate first-note peak=" + String(immediatePeak, 9) +
				" idle-delta=" + String(immediateNoteDelta, 9));
			require(immediatePeak > 0.f && immediateNoteDelta > 0.f,
				"Immediate MIDI save restores a first note distinct from idle");
			immediateIdleInstance->releaseResources();
			immediateRestored->releaseResources();
			immediateRestored.reset();
			(void)render(*original, {}, settleBlocks);
			saved = save(*original);
			requireModel(saved, expectedModel);
		}
		original->releaseResources();
		original.reset();
		// Two fresh VST3 instances begin from the same image and run on the same
		// block timeline. Capture only after the note has produced output that
		// differs from a no-note instance; a queued note alone is insufficient.
		auto soundingSource = create(manager, description);
		soundingSource->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		auto quietSource = create(manager, description);
		quietSource->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		auto captureNote = auditionNotes();
		bool sourceNoteSounds = false;
		for(int block = 0; block < auditionBlocks; ++block)
		{
			const auto soundingBlock = render(*soundingSource, captureNote, 1);
			captureNote.clear();
			const auto quietBlock = render(*quietSource, {}, 1);
			if(soundingBlock != quietBlock)
			{
				sourceNoteSounds = true;
				break;
			}
		}
		require(sourceNoteSounds, "Active-note source produced signal distinct from idle");
		const auto activeState = save(*soundingSource);
		requireModel(activeState, expectedModel);
		soundingSource->releaseResources();
		quietSource->releaseResources();
		// Active voices are history. Their capture must reopen with the same idle
		// and next-note output as the image taken immediately before the note.
		auto quietReference = create(manager, description);
		quietReference->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		auto activeRestored = create(manager, description);
		activeRestored->setStateInformation(activeState.getData(), static_cast<int>(activeState.getSize()));
		require(save(*activeRestored) == activeState,
			"Active-note capture restores before any fresh audio callback");
		const auto quietIdle = render(*quietReference, {}, auditionBlocks);
		const auto activeIdle = render(*activeRestored, {}, auditionBlocks);
		require(quietIdle == activeIdle, "Active-note history does not sound after reopen");
		const auto quietNextNote = render(*quietReference, auditionNotes(), auditionBlocks);
		const auto activeNextNote = render(*activeRestored, auditionNotes(), auditionBlocks);
		require(quietNextNote == activeNextNote,
			"Active-note history leaves the immediate next note unchanged");
		quietReference->releaseResources();
		activeRestored->releaseResources();

		auto restored = create(manager, description);
		restored->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		require(save(*restored) == saved, "Load then Save preserves state before any audio callback");
		if(nativeComponentPath.isNotEmpty() && (expectedModel == 0 || expectedModel == 1))
			requireSc88NativeSeed(componentStateFromHost(save(*restored)), nativeComponent,
				expectedModel, eqSeed, "Fresh VST3 instance restores the native panel LevelR value",
				"Fresh VST3 instance restores the native ALL EQ Gain context");
		if(nativeComponentPath.isNotEmpty() && expectedModel == 2)
			requireProPanelContext(componentStateFromHost(save(*restored)), nativeComponent,
				"Fresh VST3 instance restores the native Pro Fine Tune menu context");
		auto duplicate = create(manager, description);
		duplicate->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		require(save(*duplicate) == saved, "A second fresh instance restores the same state");

		const auto notes = auditionNotes();
		const auto firstAudio = render(*restored, notes, auditionBlocks);
		const auto secondAudio = render(*duplicate, notes, auditionBlocks);
		require(firstAudio == secondAudio, "Fresh restored instances produce identical audio");
		auto idleReference = create(manager, description);
		idleReference->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		const auto idleAudio = render(*idleReference, {}, auditionBlocks);
		require(firstAudio != idleAudio,
			"Restored instrument produces a first note distinct from idle");
		idleReference->releaseResources();

		auto defaults = create(manager, description);
		const auto defaultAudio = render(*defaults, notes, auditionBlocks);
		require(firstAudio != defaultAudio, "Restored settings audibly differ from factory defaults");
		const auto beforeIndependentEdit = save(*duplicate);
		MidiBuffer independentEdit;
		independentEdit.addEvent(MidiMessage::controllerEvent(1, 7, 0), 0);
		(void)render(*restored, independentEdit, settleBlocks);
		const auto unchanged = save(*duplicate);
		require(unchanged == beforeIndependentEdit, "Editing one instance leaves the other state unchanged");
		const auto changed = save(*restored);
		require(changed != unchanged, "Editing one instance produces an independent state");

		for(auto* plugin : {restored.get(), duplicate.get(), defaults.get()})
			plugin->releaseResources();
		if(fixturePath.isNotEmpty())
		{
			require(File(fixturePath).replaceWithData(saved.getData(), saved.getSize()), "Write cross-process state fixture");
			require(File(fixturePath + ".audio").replaceWithData(firstAudio.data(), firstAudio.size() * sizeof(float)),
				"Write cross-process audio fixture");
		}
		Logger::writeToLog("PASS: headless VST3 fresh-instance recall suite");
		return 0;
	}
}
