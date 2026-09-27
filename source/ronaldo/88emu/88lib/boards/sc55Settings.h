#pragma once

#include <cstdint>
#include <vector>

namespace emu88Lib
{
	class Sc55Board;

	// First SC-55 1.21 settings slice. This adapter uses the firmware's
	// battery-backed RAM boot path on a native firmware input boundary.
	class Sc55Settings final
	{
	public:
		static constexpr uint32_t LayoutVersion = 1;
		enum class Result { Success, UnsupportedFirmware, InvalidImage, RequiresFreshBoard, PendingInput };
		static bool supported(const Sc55Board& board);
		static bool isPanelInputBoundary(Sc55Board& board);
		static bool isCaptureBoundary(Sc55Board& board);
		static Result capture(Sc55Board& board, std::vector<uint8_t>& image);
		static Result restore(Sc55Board& board, const std::vector<uint8_t>& image);
		// Diagnostic gate for the exact stock mkII ROM pair. These deliberately
		// bypass the unproved mkII input boundary and are never host adapters.
		// The focused ROM test uses them to establish or reject SRAM boot retention.
		static bool probeMk2Image(const Sc55Board& board, std::vector<uint8_t>& image);
		static bool reopenMk2Probe(Sc55Board& board, const std::vector<uint8_t>& image);
		static bool probeMk2InputDrained(const Sc55Board& board);
	};
}
