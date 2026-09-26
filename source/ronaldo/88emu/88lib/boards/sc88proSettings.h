#pragma once

#include <cstdint>
#include <vector>

namespace emu88Lib
{
	class Sc88Pro;

	// Version-specific settings restoration, deliberately separate from chip emulation.
	// The owner must serialize access and establish its input/panel capture boundary.
	// A memory copy alone does not prove that pending firmware commands have completed.
	class Sc88ProSettings final
	{
	public:
		// First verified SRAM/receive-field map for SC-88Pro firmware1.02.
		static constexpr uint32_t LayoutVersion = 1;
		enum class Result { Success, UnsupportedFirmware, InvalidImage, RequiresFreshBoard };

		// Copies authoritative firmware memory without rendering or changing the live board.
		static Result capture(const Sc88Pro& _board, std::vector<uint8_t>& _image);
		// Firmware-identified completed-input boundary; a held key is never released here.
		static bool isCaptureBoundary(const Sc88Pro& _board, bool allowHeld);
		// Only for an unrendered board constructed with factoryReset=false. Boots privately,
		// restoring settings and receive controls while discarding previous voice history.
		static Result restore(Sc88Pro& _board, const std::vector<uint8_t>& _image);
	};
}
