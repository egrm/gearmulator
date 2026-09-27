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
	// sc88pro_state_probe --system-menu-recall: A2 / page-1 Prevw Velo.
	inline constexpr size_t proSystemCursorAddress = 0x4c68;
	// Firmware 1.02 0D:9CA8 descriptor 4BFD, read by 0C:9293..929C.
	inline constexpr size_t proPreviewVelocityAddress = 0x4bfd;
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
	inline constexpr size_t sc88EqLowFrequencyAddress = 0x8788;
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

	inline void requireSc88AllContext(const MemoryBlock& actual, const MemoryBlock& seed,
		int model, const char* description)
	{
		const auto context = model == 0 ? sc88EqContextAddress : sc88VlEqContextAddress;
		const auto allMode = model == 0 ? sc88AllEqPage : sc88VlAllEqPage;
		const auto requestedMode = model == 0 ? uint8_t{0} : uint8_t{1};
		require(sc88HardwareByte(seed, context) == allMode &&
			sc88HardwareByte(seed, context + 1) == requestedMode &&
			sc88HardwareByte(seed, context + 2) == 0 &&
			sc88HardwareByte(seed, context + eqSelectorOffset) == 0,
			"Native SC-88 ALL seed is on its normal page");
		for(const auto offset : {size_t{0}, size_t{1}, size_t{2}, eqSelectorOffset})
			require(sc88HardwareByte(actual, context + offset) ==
				sc88HardwareByte(seed, context + offset), description);
		requireSc88PanelLevel(actual, seed, model, description);
	}

	inline void requireSc88FrequencyContext(const MemoryBlock& actual, const MemoryBlock& seed,
		int model, const char* description)
	{
		const auto context = model == 0 ? sc88EqContextAddress : sc88VlEqContextAddress;
		const auto allMode = model == 0 ? sc88AllEqPage : sc88VlAllEqPage;
		const auto title = model == 0 ? size_t{0x5354} : size_t{0x5358};
		require(sc88HardwareByte(seed, context) == allMode &&
			sc88HardwareByte(seed, context + 1) == allMode &&
			sc88HardwareByte(seed, context + 2) == 0 &&
			sc88HardwareByte(seed, context + eqSelectorOffset) == 0,
			"Native SC-88 EQ Frequency seed is on the expected ALL page");
		for(size_t index = 0; index < 4; ++index)
		{
			require(sc88HardwareByte(seed, title + index) == static_cast<uint8_t>("Freq"[index]),
				"Native SC-88 EQ Frequency seed retains its caption");
			require(sc88HardwareByte(actual, title + index) == sc88HardwareByte(seed, title + index),
				description);
		}
		for(const auto offset : {size_t{0}, size_t{1}, size_t{2}, eqSelectorOffset})
			require(sc88HardwareByte(actual, context + offset) ==
				sc88HardwareByte(seed, context + offset), description);
		require(sc88HardwareByte(actual, sc88EqLowFrequencyAddress) ==
			sc88HardwareByte(seed, sc88EqLowFrequencyAddress), description);
	}

	inline void requireSc88NativeSeed(const MemoryBlock& actual, const MemoryBlock& seed,
		int model, const String& seedKind, const char* description)
	{
		if(seedKind == "producer-bank")
		{
			// SC-88/VL manuals: St.Soft EP is CC0 8, CC32 2, PC005.
			// D820 is the accepted CC32 latch, not a proven effective map.
			const auto part = sc88SelectedPart(seed, model);
			require(sc88SelectedPart(actual, model) == part, description);
			const auto primary = (part < sc88PartsPerGroup ? sc88FirstGroupBase : sc88SecondGroupBase) +
				(part % sc88PartsPerGroup) * sc88PartRecordBytes;
			constexpr size_t pendingBankMsbBase = 0xd860;
			constexpr size_t pendingMapLsbBase = 0xd820;
			require(sc88HardwareByte(seed, primary) == 8 &&
				sc88HardwareByte(seed, primary + 1) == 4 &&
				sc88HardwareByte(seed, pendingBankMsbBase + 2 * part) == 8 &&
				sc88HardwareByte(seed, pendingMapLsbBase + 2 * part) == 2,
				"Native producer bank seed committed St.Soft EP and accepted CC32 2");
			for(const auto address : {primary, primary + 1,
				pendingBankMsbBase + 2 * part, pendingMapLsbBase + 2 * part})
				require(sc88HardwareByte(actual, address) == sc88HardwareByte(seed, address),
					description);
		}
		else if(seedKind == "producer-wet" || seedKind == "producer-dry")
		{
			// docs/research/88emu-userinst-panel-procedure.md: selected
			// secondary envelope offsets A/B/C, primary program and CC91 send.
			const auto part = sc88SelectedPart(seed, model);
			require(sc88SelectedPart(actual, model) == part,
				"VST3 retains the producer's selected internal part");
			const auto primary = (part < sc88PartsPerGroup ? sc88FirstGroupBase : sc88SecondGroupBase) +
				(part % sc88PartsPerGroup) * sc88PartRecordBytes;
			const auto secondary = (part < sc88PartsPerGroup ? size_t{0x87c8} : size_t{0x9cc8}) +
				(part % sc88PartsPerGroup) * size_t{0x20};
			require(sc88HardwareByte(seed, primary + 1) != 0,
				"Native producer seed has a nondefault selected program");
			const auto send = sc88HardwareByte(seed, primary + 0x0f);
			require(seedKind == "producer-dry" ? send == 0 : send != 0,
				"Native producer seed has the requested dry or wet reverb send");
			for(const auto address : {primary, primary + 1, primary + 0x0f,
				secondary + 0x0a, secondary + 0x0b, secondary + 0x0c})
				require(sc88HardwareByte(actual, address) == sc88HardwareByte(seed, address),
					description);
		}
		else if(seedKind == "eq") requireSc88EqContext(actual, seed, model, description);
		else if(seedKind == "all") requireSc88AllContext(actual, seed, model, description);
		else if(seedKind == "eq-frequency") requireSc88FrequencyContext(actual, seed, model, description);
		else requireSc88PanelLevel(actual, seed, model, description);
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

	inline void requireProSystemContext(const MemoryBlock& actual, const MemoryBlock& seed,
		const char* description)
	{
		require(sc88HardwareByte(seed, proPanelPageAddress) == 1 &&
			hardwareWord(seed, proSystemCursorAddress) == 0x10 &&
			hardwareWord(seed, proSelectedPartAddress) == 1 &&
			sc88HardwareByte(seed, proPreviewVelocityAddress) == 101,
			"Native Pro seed selects A2 System Prevw Velo with edited value 101");
		require(sc88HardwareByte(actual, proPanelPageAddress) == 1 &&
			hardwareWord(actual, proSystemCursorAddress) == 0x10 &&
			hardwareWord(actual, proSelectedPartAddress) == 1 &&
			sc88HardwareByte(actual, proPreviewVelocityAddress) == 101, description);
	}

	inline size_t proProducerDescriptor(const MemoryBlock& component)
	{
		require(hardwareWord(component, proSelectedPartAddress) == 0,
			"Native Pro producer seed selects A1");
		const auto descriptor = hardwareWord(component, 0xcf7a);
		require(descriptor > 0 && descriptor + 0x16 < 0x20000,
			"Native Pro producer descriptor pointer is valid");
		return descriptor;
	}

	inline void requireProProducerSound(const MemoryBlock& actual, const MemoryBlock& seed,
		bool dry, const char* description)
	{
		// sc88proSoundState_probe.cpp: native A1 descriptor and envelope edits.
		const auto descriptor = proProducerDescriptor(seed);
		require(proProducerDescriptor(actual) == descriptor, description);
		Logger::writeToLog("Pro producer seed kind=" + String(dry ? "dry" : "wet") +
			" program=" + String(sc88HardwareByte(seed, descriptor + 1)) +
			" envelope=" + String(sc88HardwareByte(seed, descriptor + 0x14)) + "," +
			String(sc88HardwareByte(seed, descriptor + 0x15)) + "," +
			String(sc88HardwareByte(seed, descriptor + 0x16)) +
			" macro=" + String(sc88HardwareByte(seed, 0x506a)) +
			" send=" + String(sc88HardwareByte(seed, descriptor + 0x0f)));
		require(sc88HardwareByte(seed, descriptor + 1) == 4 &&
			sc88HardwareByte(seed, descriptor + 0x14) == 0x41 &&
			sc88HardwareByte(seed, descriptor + 0x15) == 0x41 &&
			sc88HardwareByte(seed, descriptor + 0x16) == 0x41 &&
			sc88HardwareByte(seed, 0x506a) == 2 &&
			sc88HardwareByte(seed, descriptor + 0x0f) == (dry ? 0 : 1),
			"Native Pro producer seed has edited preset, envelope and reverb settings");
		for(const auto address : {descriptor, descriptor + 1, descriptor + 0x0f,
			descriptor + 0x14, descriptor + 0x15, descriptor + 0x16, size_t{0x506a}})
			require(sc88HardwareByte(actual, address) == sc88HardwareByte(seed, address),
				description);
	}

	inline void requireProProducerBank(const MemoryBlock& actual, const MemoryBlock& seed,
		const char* description)
	{
		// Emu88ProducerRecall_test.cpp selects Piano3w through CC0=8,
		// CC32=3, PC=2, then stages different bank selectors without PC.
		constexpr size_t partOneBankMsb = 0xc862;
		constexpr size_t partOneMapLsb = 0xc822;
		const auto descriptor = proProducerDescriptor(seed);
		require(proProducerDescriptor(actual) == descriptor, description);
		require(sc88HardwareByte(seed, descriptor) == 8 &&
			sc88HardwareByte(seed, descriptor + 1) == 2 &&
			sc88HardwareByte(seed, partOneBankMsb) == 0 &&
			sc88HardwareByte(seed, partOneMapLsb) == 1,
			"Native Pro bank seed commits Piano3w and retains different pending selectors");
		for(const auto address : {descriptor, descriptor + 1,
			partOneBankMsb, partOneMapLsb})
			require(sc88HardwareByte(actual, address) == sc88HardwareByte(seed, address),
				description);
	}

	// SettingsChunk::encode writes a 40-byte prefix, followed by the 261-byte
	// LaSettings layout-1 image and a 16-byte digest. Decode JUCE's base64
	// once per component so the paired comparison stays bounded.
	inline constexpr size_t mt32ChunkMemoryOffset = 40;
	inline constexpr size_t mt32ImageBytes = 261;
	inline constexpr size_t mt32ChunkDigestBytes = 16;
	inline constexpr size_t mt32ReverbLevelOffset = 3;

	inline bool isLaModel(int model)
	{
		return model == 18 || model == 21 || model == 22;
	}

	inline bool isLaProducerSeed(const String& seedKind)
	{
		return seedKind == "producer-mt32old-dry" || seedKind == "producer-mt32old-wet" ||
			seedKind == "producer-mt32new-dry" || seedKind == "producer-mt32new-wet" ||
			seedKind == "producer-cm32l-dry" || seedKind == "producer-cm32l-wet";
	}

	inline MemoryBlock mt32HardwareChunk(const MemoryBlock& component)
	{
		auto envelope = parseXML(String::fromUTF8(
			static_cast<const char*>(component.getData()), static_cast<int>(component.getSize())));
		const auto* payload = envelope ? envelope->getChildByName("Payload") : nullptr;
		const auto* hardware = payload ? payload->getChildByName("Hardware") : nullptr;
		require(hardware != nullptr, "MT-32 component contains Hardware");
		MemoryBlock chunk;
		require(chunk.fromBase64Encoding(hardware->getAllSubText()) &&
			chunk.getSize() == mt32ChunkMemoryOffset + mt32ImageBytes + mt32ChunkDigestBytes,
			"MT-32 component contains complete LaSettings layout-1 image");
		return chunk;
	}

	inline void requireAdditionalFamilySeed(const MemoryBlock& actual,
		const MemoryBlock& seed, int model, const String& seedKind,
		const char* description)
	{
		// sc55Settings_test.cpp and the native LevelR/PC producer fixture:
		// selected A1 program at 185, level at 192 for firmware 1.21.
		if(model == 5 && seedKind == "producer-sc55mk1")
		{
			constexpr size_t programAddress = 185;
			constexpr size_t levelAddress = 192;
			require(sc88HardwareByte(seed, programAddress) == 17 &&
				sc88HardwareByte(seed, levelAddress) == 102,
				"Native SC-55mk1 seed has selected program 17 and LevelR volume 102");
			for(const auto address : {programAddress, levelAddress})
				require(sc88HardwareByte(actual, address) == sc88HardwareByte(seed, address),
					description);
			return;
		}
		if(model == 10 && (seedKind == "producer-sc155-wet" || seedKind == "producer-sc155-dry"))
		{
			// SC-155 Rev1 native selected-panel InstR/ReverbR fixture.
			// These two offsets must first pass its fail-closed processor oracle.
			constexpr size_t programAddress = 0xb9;
			constexpr size_t reverbAddress = 0xc7;
			require(sc88HardwareByte(seed, programAddress) == 1 &&
				sc88HardwareByte(seed, reverbAddress) ==
					(seedKind == "producer-sc155-dry" ? 0 : 41),
				"Native SC-155 selected panel part has program 1 and requested reverb send");
			for(const auto address : {programAddress, reverbAddress})
				require(sc88HardwareByte(actual, address) == sc88HardwareByte(seed, address),
					description);
			return;
		}
		if(isLaModel(model) && isLaProducerSeed(seedKind))
		{
			// LaSettings.h layout 1: system 4, patch head 7, patch tail 2,
			// timbre temp 246, physical knob 2. The old-model native
			// panel probe selects Sitar; new and CM fixtures use part-one PC17.
			constexpr size_t patchGroup = 4;
			constexpr size_t patchTimbre = 5;
			const auto sourceChunk = mt32HardwareChunk(seed);
			const auto actualChunk = mt32HardwareChunk(actual);
			const auto* sourceMemory = static_cast<const uint8_t*>(sourceChunk.getData()) + mt32ChunkMemoryOffset;
			if(model == 21)
				require(sourceMemory[patchGroup] == 0 && sourceMemory[patchTimbre] == 63,
					"Native MT-32 old producer seed selects Sitar");
			require(sourceMemory[mt32ReverbLevelOffset] == (seedKind.endsWith("dry") ? 0 : 1),
				"LA producer seed retains requested reverb level");
			require(actualChunk == sourceChunk, description);
			return;
		}
		throw std::runtime_error("Additional-family VST3 seed has no verified model-specific oracle");
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

	inline int hostMidiChannel(int model)
	{
		// deviceModel.h::firstMidiChannel: LA part one receives on MIDI
		// channel two. JUCE MidiMessage channels are one-based.
		return isLaModel(model) ? 2 : 1;
	}

	inline MidiBuffer auditionNotes(int model = -1)
	{
		MidiBuffer notes;
		notes.addEvent(MidiMessage::noteOn(hostMidiChannel(model), 60, static_cast<uint8>(96)), 13);
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
		const auto audio = render(*restored, auditionNotes(expectedModel), auditionBlocks);
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
		int expectedModel = -1, const String& nativeComponentPath = {}, const String& seedKind = {})
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
					expectedModel, seedKind, "Fresh process retains the native SC-88 panel context");
		}
		if(nativeComponentPath.isNotEmpty() && expectedModel == 2)
		{
			MemoryBlock nativeComponent;
			require(File(nativeComponentPath).loadFileAsData(nativeComponent),
				"Fresh process reads the native Pro panel source image");
			if(seedKind == "producer-pro-bank")
				requireProProducerBank(componentStateFromHost(save(*restored)), nativeComponent,
					"Fresh process retains committed Pro bank and pending selectors");
			else if(seedKind == "producer-pro-wet" || seedKind == "producer-pro-dry")
				requireProProducerSound(componentStateFromHost(save(*restored)), nativeComponent,
					seedKind == "producer-pro-dry", "Fresh process retains native Pro producer sound");
			else if(seedKind == "pro-system")
				requireProSystemContext(componentStateFromHost(save(*restored)), nativeComponent,
					"Fresh process retains the native Pro System menu context");
			else
				requireProPanelContext(componentStateFromHost(save(*restored)), nativeComponent,
					"Fresh process retains the native Pro Fine Tune menu context");
		}
		if(nativeComponentPath.isNotEmpty() && (expectedModel == 5 || isLaModel(expectedModel)))
		{
			MemoryBlock nativeComponent;
			require(File(nativeComponentPath).loadFileAsData(nativeComponent),
				"Fresh process reads the additional-family source image");
			requireAdditionalFamilySeed(componentStateFromHost(save(*restored)), nativeComponent,
				expectedModel, seedKind, "Fresh process retains additional-family sound edits");
		}
		const auto audio = render(*restored, auditionNotes(expectedModel), auditionBlocks);
		require(MemoryBlock(audio.data(), audio.size() * sizeof(float)) == expectedAudio,
			"Fresh process produces identical recalled audio");
		restored->releaseResources();
		return 0;
	}

	inline int run(const PluginDescription& description, const String& fixturePath,
		int expectedModel = -1, const String& nativeComponentPath = {}, const String& seedKind = {})
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
					expectedModel, seedKind, "Native SC-88 panel context survives a fresh VST3 capture");
			if(expectedModel == 2)
			{
				if(seedKind == "producer-pro-bank")
					requireProProducerBank(componentStateFromHost(saved), nativeComponent,
						"Native Pro bank survives a fresh VST3 capture");
				else if(seedKind == "producer-pro-wet" || seedKind == "producer-pro-dry")
					requireProProducerSound(componentStateFromHost(saved), nativeComponent,
						seedKind == "producer-pro-dry", "Native Pro producer sound survives a fresh VST3 capture");
				else if(seedKind == "pro-system")
					requireProSystemContext(componentStateFromHost(saved), nativeComponent,
						"Native Pro System menu survives a fresh VST3 capture");
				else
					requireProPanelContext(componentStateFromHost(saved), nativeComponent,
						"Native Pro Fine Tune menu survives a fresh VST3 capture");
			}
			if(expectedModel == 5 || isLaModel(expectedModel))
				requireAdditionalFamilySeed(componentStateFromHost(saved), nativeComponent,
					expectedModel, seedKind, "Additional-family sound edits survive VST3 capture");
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
			const auto immediateAudio = render(*immediateRestored, auditionNotes(expectedModel), auditionBlocks);
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
		auto captureNote = auditionNotes(expectedModel);
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
		const auto quietNextNote = render(*quietReference, auditionNotes(expectedModel), auditionBlocks);
		const auto activeNextNote = render(*activeRestored, auditionNotes(expectedModel), auditionBlocks);
		require(quietNextNote == activeNextNote,
			"Active-note history leaves the immediate next note unchanged");
		quietReference->releaseResources();
		activeRestored->releaseResources();

		auto restored = create(manager, description);
		restored->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		require(save(*restored) == saved, "Load then Save preserves state before any audio callback");
		if(nativeComponentPath.isNotEmpty() && (expectedModel == 0 || expectedModel == 1))
			requireSc88NativeSeed(componentStateFromHost(save(*restored)), nativeComponent,
				expectedModel, seedKind, "Fresh VST3 instance restores the native SC-88 panel context");
		if(nativeComponentPath.isNotEmpty() && expectedModel == 2)
		{
			if(seedKind == "producer-pro-bank")
				requireProProducerBank(componentStateFromHost(save(*restored)), nativeComponent,
					"Fresh VST3 instance restores committed Pro bank and pending selectors");
			else if(seedKind == "producer-pro-wet" || seedKind == "producer-pro-dry")
				requireProProducerSound(componentStateFromHost(save(*restored)), nativeComponent,
					seedKind == "producer-pro-dry", "Fresh VST3 instance restores native Pro producer sound");
			else if(seedKind == "pro-system")
				requireProSystemContext(componentStateFromHost(save(*restored)), nativeComponent,
					"Fresh VST3 instance restores the native Pro System menu context");
			else
				requireProPanelContext(componentStateFromHost(save(*restored)), nativeComponent,
					"Fresh VST3 instance restores the native Pro Fine Tune menu context");
		}
		if(nativeComponentPath.isNotEmpty() && (expectedModel == 5 || isLaModel(expectedModel)))
			requireAdditionalFamilySeed(componentStateFromHost(save(*restored)), nativeComponent,
				expectedModel, seedKind, "Fresh VST3 instance restores additional-family sound edits");
		auto duplicate = create(manager, description);
		duplicate->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		require(save(*duplicate) == saved, "A second fresh instance restores the same state");

		const auto notes = auditionNotes(expectedModel);
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
		if(seedKind == "producer-wet" || seedKind == "producer-dry" ||
			seedKind == "producer-pro-wet" || seedKind == "producer-pro-dry" ||
			isLaProducerSeed(seedKind))
		{
			const auto otherKind = seedKind.replace("wet", "dry") == seedKind ?
				seedKind.replace("dry", "wet") : seedKind.replace("wet", "dry");
			const auto otherPath = isLaModel(expectedModel) ?
				nativeComponentPath.replace(seedKind.endsWith("dry") ? "-dry-model-" : "-wet-model-",
					seedKind.endsWith("dry") ? "-wet-model-" : "-dry-model-") :
				nativeComponentPath.replace(seedKind, otherKind);
			MemoryBlock otherNative;
			require(otherPath != nativeComponentPath && File(otherPath).loadFileAsData(otherNative),
				"Read paired native wet/dry producer component");
			XmlElement wrapper("VST3PluginState");
			wrapper.createNewChildElement("IComponent")->addTextElement(otherNative.toBase64Encoding());
			MemoryBlock wrapped;
			AudioProcessor::copyXmlToBinary(wrapper, wrapped);
			auto otherSource = create(manager, description);
			otherSource->setStateInformation(wrapped.getData(), static_cast<int>(wrapped.getSize()));
			(void)render(*otherSource, {}, 1);
			const auto otherState = save(*otherSource);
			requireModel(otherState, expectedModel);
			if(isLaModel(expectedModel))
				requireAdditionalFamilySeed(componentStateFromHost(otherState), otherNative,
					expectedModel, otherKind, "Paired MT-32 sound survives VST3 capture");
			else if(expectedModel == 2)
				requireProProducerSound(componentStateFromHost(otherState), otherNative,
					otherKind == "producer-pro-dry", "Paired Pro producer sound survives VST3 capture");
			else
				requireSc88NativeSeed(componentStateFromHost(otherState), otherNative,
					expectedModel, otherKind, "Paired producer sound survives VST3 capture");
			const auto thisComponent = componentStateFromHost(saved);
			const auto otherComponent = componentStateFromHost(otherState);
			if(isLaModel(expectedModel))
			{
				const auto thisChunk = mt32HardwareChunk(thisComponent);
				const auto otherChunk = mt32HardwareChunk(otherComponent);
				const auto* thisMemory = static_cast<const uint8_t*>(thisChunk.getData()) + mt32ChunkMemoryOffset;
				const auto* otherMemory = static_cast<const uint8_t*>(otherChunk.getData()) + mt32ChunkMemoryOffset;
				require(std::memcmp(thisMemory, otherMemory, mt32ReverbLevelOffset) == 0 &&
					std::memcmp(thisMemory + mt32ReverbLevelOffset + 1,
						otherMemory + mt32ReverbLevelOffset + 1,
						mt32ImageBytes - mt32ReverbLevelOffset - 1) == 0,
					"Wet and dry MT-32 states retain identical patch, timbre and knob bytes");
			}
			else if(expectedModel == 2)
			{
				const auto descriptor = proProducerDescriptor(thisComponent);
				require(proProducerDescriptor(otherComponent) == descriptor,
					"Wet and dry Pro producer states select the same A1 descriptor");
				for(const auto address : {descriptor, descriptor + 1, descriptor + 0x14,
					descriptor + 0x15, descriptor + 0x16, size_t{0x506a}})
					require(sc88HardwareByte(thisComponent, address) ==
						sc88HardwareByte(otherComponent, address),
						"Wet and dry Pro producer states retain identical preset and envelope bytes");
			}
			else
			{
				const auto part = sc88SelectedPart(thisComponent, expectedModel);
				require(sc88SelectedPart(otherComponent, expectedModel) == part,
					"Wet and dry producer states select the same internal part");
				const auto primary = (part < sc88PartsPerGroup ? sc88FirstGroupBase : sc88SecondGroupBase) +
					(part % sc88PartsPerGroup) * sc88PartRecordBytes;
				const auto secondary = (part < sc88PartsPerGroup ? size_t{0x87c8} : size_t{0x9cc8}) +
					(part % sc88PartsPerGroup) * size_t{0x20};
				for(const auto address : {primary, primary + 1, secondary + 0x0a,
					secondary + 0x0b, secondary + 0x0c})
					require(sc88HardwareByte(thisComponent, address) ==
						sc88HardwareByte(otherComponent, address),
						"Wet and dry producer states retain identical preset and envelope bytes");
			}
			auto thisSound = create(manager, description);
			thisSound->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
			auto otherSound = create(manager, description);
			otherSound->setStateInformation(otherState.getData(), static_cast<int>(otherState.getSize()));
			(void)render(*thisSound, auditionNotes(expectedModel), auditionBlocks / 2);
			(void)render(*otherSound, auditionNotes(expectedModel), auditionBlocks / 2);
			MidiBuffer noteOff;
			noteOff.addEvent(MidiMessage::noteOff(hostMidiChannel(expectedModel), 60), 0);
			const auto thisTail = render(*thisSound, noteOff, auditionBlocks * 2);
			const auto otherTail = render(*otherSound, noteOff, auditionBlocks * 2);
			float maximumDelta{};
			for(size_t sample{}; sample < thisTail.size(); ++sample)
				maximumDelta = std::max(maximumDelta, std::abs(thisTail[sample] - otherTail[sample]));
			Logger::writeToLog("Producer wet/dry post-note tail maximum delta=" + String(maximumDelta, 9));
			require(maximumDelta > 0.f, isLaModel(expectedModel) ?
				"MT-32 system reverb level changes actual VST3 note-off tail" :
				"Native reverb send zero changes actual VST3 note-off tail");
			thisSound->releaseResources();
			otherSound->releaseResources();
			otherSource->releaseResources();
		}
		const auto beforeIndependentEdit = save(*duplicate);
		MidiBuffer independentEdit;
		if(isLaModel(expectedModel))
			independentEdit.addEvent(MidiMessage::programChange(hostMidiChannel(expectedModel), 18), 0);
		else
			independentEdit.addEvent(MidiMessage::controllerEvent(hostMidiChannel(expectedModel), 7, 0), 0);
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
