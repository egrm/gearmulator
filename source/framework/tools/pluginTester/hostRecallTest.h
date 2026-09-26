#pragma once

#include "JuceHeader.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace hostRecallTest
{
	// Deliberately awkward test fixtures exercise resampling across host block boundaries.
	inline constexpr double sampleRate = 44100.0;
	inline constexpr int blockSize = 257;
	inline constexpr int settleBlocks = 400;
	inline constexpr int auditionBlocks = 120;

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

	inline MidiBuffer auditionNotes()
	{
		MidiBuffer notes;
		notes.addEvent(MidiMessage::noteOn(1, 60, static_cast<uint8>(96)), 13);
		return notes;
	}

	inline int readFreshProcess(const PluginDescription& description, const String& fixturePath)
	{
		MemoryBlock saved, expectedAudio;
		require(File(fixturePath).loadFileAsData(saved), "Read prior-process VST3 state");
		require(File(fixturePath + ".audio").loadFileAsData(expectedAudio), "Read prior-process expected audio");
		AudioPluginFormatManager manager;
		manager.addDefaultFormats();
		auto restored = create(manager, description);
		restored->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		require(save(*restored) == saved, "Fresh process preserves saved VST3 state");
		const auto audio = render(*restored, auditionNotes(), auditionBlocks);
		require(MemoryBlock(audio.data(), audio.size() * sizeof(float)) == expectedAudio,
			"Fresh process produces identical recalled audio");
		restored->releaseResources();
		return 0;
	}

	inline int run(const PluginDescription& description, const String& fixturePath)
	{
		AudioPluginFormatManager manager;
		manager.addDefaultFormats();
		auto original = create(manager, description);
		MidiBuffer edits;
		for(int channel = 1; channel <= 16; ++channel)
		{
			edits.addEvent(MidiMessage::programChange(channel, 17), 11);
			edits.addEvent(MidiMessage::controllerEvent(channel, 7, 43), 23);
			edits.addEvent(MidiMessage::controllerEvent(channel, 10, 21), 37);
			edits.addEvent(MidiMessage::controllerEvent(channel, 11, 95), 59);
		}
		(void)render(*original, edits, settleBlocks);
		const auto saved = save(*original);
		original->releaseResources();
		original.reset();

		auto restored = create(manager, description);
		restored->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		require(save(*restored) == saved, "Load then Save preserves state before any audio callback");
		auto duplicate = create(manager, description);
		duplicate->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
		require(save(*duplicate) == saved, "A second fresh instance restores the same state");

		const auto notes = auditionNotes();
		const auto firstAudio = render(*restored, notes, auditionBlocks);
		const auto secondAudio = render(*duplicate, notes, auditionBlocks);
		require(firstAudio == secondAudio, "Fresh restored instances produce identical audio");
		const auto hasSound = std::any_of(firstAudio.begin(), firstAudio.end(),
			[](float sample) { return std::abs(sample) > 0.001f; });
		require(hasSound, "Restored instrument produces audible notes");

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
