#include "88lib/boards/sc88Settings.h"
#include "88lib/boards/sc88.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace emu88Lib
{
	namespace
	{
		// CPU-order control ROM fingerprints from romRegistry.h and the
		// independently audited SC-88/VL real-ROM boot diagnostic.
		constexpr baseLib::MD5 g_sc88Firmware("0ac771782ea58a53af590ebdf140d517");
		constexpr baseLib::MD5 g_sc88vlFirmware("25e016e93c8a44ba3c35584462b56d72");
		// SC-88 01:0B0C and VL 01:0C45 copy C072 to the C092 boot gate.
		// Their native C072 writers accept only 0 or 1. Preserve the saved value
		// after the private candidate has booted through the preservation branch.
		constexpr size_t g_batteryPreference = 0xc072;
		constexpr size_t g_bootGate = 0xc092;
		constexpr uint8_t g_preserveSettings = 1;
		// SC-88 01:BBCC..BC33 and VL 01:AF94..AFFB write the 32
		// panel-activity countdowns; 01:A6EE..A706 and 01:9A00..9A18
		// decrement them. They are not saved sound parameters.
		constexpr size_t g_meterCount = 32;
		constexpr size_t g_sc88MeterStart = 0x5444;
		constexpr size_t g_sc88vlMeterStart = 0x5448;
		// SC-88 00:2903/290B and VL 00:2ABB/2AC3 read/update this
		// synthesis overflow accumulator, then write a per-voice value.
		constexpr size_t g_sc88Accumulator = 0xf5ea;
		constexpr size_t g_sc88vlAccumulator = 0xf5fe;
		constexpr size_t g_accumulatorBytes = sizeof(uint16_t);
		// Voice-start handlers SC 00:220C/2210 and VL 00:23BC/23C0 write
		// owner and note to adjacent bytes. Their 01:913B/837F meter loops
		// scan 64 even owner offsets; neither byte is a saved part setting.
		constexpr size_t g_sc88VoiceOwnerStart = 0xeade;
		constexpr size_t g_sc88vlVoiceOwnerStart = 0xeaf2;
		constexpr size_t g_voiceOwnerCount = 64;
		constexpr size_t g_voiceOwnerStride = sizeof(uint16_t);
		// Existing SC-88/VL real-ROM diagnostic boots for this interval. This
		// foundation needs a model-specific readiness predicate before host use.
		constexpr unsigned g_bootSamples = g_sampleRate * 10;
		constexpr size_t g_parts = 32; // Both ROM boot loops address two groups of 16.
		// SC 00:30C4/3585 and VL 00:3195/3663 write/reset 128 per-key
		// pressure bytes per part. These receive values are separate from voices.
		constexpr size_t g_polyPressureBase = 0xc660;
		constexpr size_t g_polyPressureKeys = 128;
		// Native event consumers SC 1EB0/1EF0/1F58 and VL 2060/20A0/2108
		// retain portamento, hold and sostenuto in bits 5, 7 and 6. The
		// separate active-key membership arrays are not restored.
		constexpr size_t g_sc88SwitchBase = 0xe59e;
		constexpr size_t g_sc88vlSwitchBase = 0xe5b2;
		constexpr uint8_t g_switchMask = 0xe0;
		// PartR writers SC 01:C81F and VL 01:BF6F use a 0..31 UI index;
		// native initialization resets it (SC 01:D38A, VL 07:C605).
		constexpr size_t g_sc88SelectedIndex = 0x56f2;
		constexpr size_t g_sc88vlSelectedIndex = 0x56fc;
		// Match the native panel gesture timing used by the Pro adapter and
		// Sc88::runFactoryReset, allowing the scanner to consume each edge.
		constexpr unsigned g_panelPressSamples = g_sampleRate / 10;
		constexpr unsigned g_panelReleaseSamples = g_sampleRate / 4;
		constexpr size_t g_partStride = sizeof(uint16_t); // R3 is doubled part index.
		constexpr auto g_byteBits = std::numeric_limits<uint8_t>::digits;
		// SC-88 handlers 30EC..326E and VL 31BD..333F. Controller values are
		// per-part receiver state; part-record volume/pan/send settings are already
		// retained by the firmware's C092 branch and are deliberately excluded.
		constexpr std::array g_receiverBytes{
			0xd6a0, 0xd660, 0xd661, 0xd6a1, 0xd6e0, 0xd6e1,
			0xd720, 0xd721, 0xd760, 0xd761, 0xd820, 0xd860,
			0xd861, 0xd8a0, 0xd8e0, 0xd8e1, 0xd920, 0xd921};
		// The six MIDI writers OR their recalculation source masks into these
		// dirty targets. Both ROMs independently use the same pairs:
		// SC-88 30C4/30EC/3108/318B/313B/3150 and
		// VL 3195/31BD/31D9/325C/320C/3221.
		constexpr std::array g_dirtyMasks{
			std::pair{0xc420, 0xc5a0}, std::pair{0xc3e0, 0xc560},
			std::pair{0xc3a0, 0xc520}, std::pair{0xc360, 0xc4e0},
			std::pair{0xc460, 0xc5e0}, std::pair{0xc4a0, 0xc620}};

		uint16_t readWord(const std::vector<uint8_t>& bytes, size_t address)
		{
			return static_cast<uint16_t>((uint16_t{bytes[address]} << g_byteBits) |
			                             bytes[address + sizeof(uint8_t)]);
		}

	}
	bool Sc88Settings::supported(const Sc88& board)
	{
		return board.isValid() &&
		       ((board.m_model == Model::Sc88 && board.m_firmwareHash == g_sc88Firmware) ||
		        (board.m_model == Model::Sc88VL && board.m_firmwareHash == g_sc88vlFirmware));
	}

	Sc88Settings::Result Sc88Settings::capture(const Sc88& board, std::vector<uint8_t>& image)
	{
		if(!supported(board)) return Result::UnsupportedFirmware;
		image = board.m_sram;
		return Result::Success;
	}

	Sc88Settings::Result Sc88Settings::restore(Sc88& board, const std::vector<uint8_t>& image)
	{
		if(!supported(board)) return Result::UnsupportedFirmware;
		if(image.size() != Sc88::SramSize || image[g_batteryPreference] > g_preserveSettings)
			return Result::InvalidImage;
		if(board.m_samplesRendered != 0) return Result::RequiresFreshBoard;
		const auto meterStart = board.m_model == Model::Sc88 ? g_sc88MeterStart : g_sc88vlMeterStart;
		const auto accumulator = board.m_model == Model::Sc88 ? g_sc88Accumulator : g_sc88vlAccumulator;
		const auto voiceOwner = board.m_model == Model::Sc88 ? g_sc88VoiceOwnerStart : g_sc88vlVoiceOwnerStart;
		const auto switches = board.m_model == Model::Sc88 ? g_sc88SwitchBase : g_sc88vlSwitchBase;
		const auto selectedIndex = board.m_model == Model::Sc88 ? g_sc88SelectedIndex : g_sc88vlSelectedIndex;
		const auto savedSelection = readWord(image,selectedIndex);
		if(savedSelection >= g_parts) return Result::InvalidImage;
		std::array<uint8_t,g_meterCount> freshMeters{};
		std::array<uint8_t,g_accumulatorBytes> freshAccumulator{};
		std::array<uint8_t,g_voiceOwnerCount> freshVoiceOwners{};
		std::copy_n(board.m_sram.begin()+meterStart,g_meterCount,freshMeters.begin());
		std::copy_n(board.m_sram.begin()+accumulator,g_accumulatorBytes,freshAccumulator.begin());
		for(size_t voice{};voice<g_voiceOwnerCount;++voice)
			freshVoiceOwners[voice] = board.m_sram[voiceOwner+voice*g_voiceOwnerStride];
		board.m_sram = image;
		std::copy(freshMeters.begin(),freshMeters.end(),board.m_sram.begin()+meterStart);
		std::copy(freshAccumulator.begin(),freshAccumulator.end(),board.m_sram.begin()+accumulator);
		for(size_t voice{};voice<g_voiceOwnerCount;++voice)
			board.m_sram[voiceOwner+voice*g_voiceOwnerStride] = freshVoiceOwners[voice];
		board.m_sram[g_batteryPreference] = g_preserveSettings;
		for(unsigned sample{}; sample < g_bootSamples; ++sample) board.renderSample();
		if(board.m_sram[g_bootGate] != g_preserveSettings) return Result::InvalidImage;
		board.m_sram[g_batteryPreference] = image[g_batteryPreference];
		// Let firmware rebuild the selected part's descriptors and LCD rather
		// than transplanting UI pointers into a fresh machine.
		for(size_t move{};move<g_parts && readWord(board.m_sram,selectedIndex)!=savedSelection;++move)
		{
			board.setButton(Button::PartR,true);
			for(unsigned sample{};sample<g_panelPressSamples;++sample) board.renderSample();
			board.setButtons(uint32_t{});
			for(unsigned sample{};sample<g_panelReleaseSamples;++sample) board.renderSample();
		}
		if(readWord(board.m_sram,selectedIndex)!=savedSelection) return Result::InvalidImage;
		std::copy_n(image.begin()+g_polyPressureBase,g_parts*g_polyPressureKeys,
		            board.m_sram.begin()+g_polyPressureBase);
		// The boot routines still reset receive controllers with C092 set.
		// Restore only source-verified per-part receiver bytes, then reproduce
		// the native handlers' dirty-mask ORs so dependent DSP state recomputes.
		// Voice-event queues and voice/key ownership history remain fresh;
		// the source-verified poly-pressure receive array is restored above.
		for(size_t part{}; part < g_parts; ++part)
		{
			const auto stride = part * g_partStride;
			board.m_sram[switches+stride] = static_cast<uint8_t>(
				(board.m_sram[switches+stride] & ~g_switchMask) |
				(image[switches+stride] & g_switchMask));
			for(const auto address : g_receiverBytes)
				board.m_sram[address + stride] = image[address + stride];
			for(const auto& [source, target] : g_dirtyMasks)
			{
				const auto value = readWord(board.m_sram, source + stride) |
				                   readWord(board.m_sram, target + stride);
				board.m_sram[target + stride] = static_cast<uint8_t>(value >> g_byteBits);
				board.m_sram[target + stride + sizeof(uint8_t)] = static_cast<uint8_t>(value);
			}
		}
		return Result::Success;
	}
}
