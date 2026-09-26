#include "88lib/boards/sc88Settings.h"
#include "88lib/boards/sc88.h"

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
		// Existing SC-88/VL real-ROM diagnostic boots for this interval. This
		// foundation needs a model-specific readiness predicate before host use.
		constexpr unsigned g_bootSamples = g_sampleRate * 10;
		constexpr size_t g_parts = 32; // Both ROM boot loops address two groups of 16.
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
		board.m_sram = image;
		board.m_sram[g_batteryPreference] = g_preserveSettings;
		for(unsigned sample{}; sample < g_bootSamples; ++sample) board.renderSample();
		if(board.m_sram[g_bootGate] != g_preserveSettings) return Result::InvalidImage;
		board.m_sram[g_batteryPreference] = image[g_batteryPreference];
		// The boot routines still reset receive controllers with C092 set.
		// Restore only source-verified per-part receiver bytes, then reproduce
		// the native handlers' dirty-mask ORs so dependent DSP state recomputes.
		// Voice-event queues and per-key history are deliberately fresh.
		for(size_t part{}; part < g_parts; ++part)
		{
			const auto stride = part * g_partStride;
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
