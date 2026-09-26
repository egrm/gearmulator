#pragma once

#include <cstdint>
#include <vector>

namespace emu88Lib
{
	class Sc88;

	// Private SC-88/SC-88VL settings foundation. The owner must establish the
	// model-specific input/panel boundary; this does not enable host recall.
	class Sc88Settings final
	{
	public:
		static constexpr uint32_t LayoutVersion = 1;
		enum class Result { Success, UnsupportedFirmware, InvalidImage, RequiresFreshBoard };
		static Result capture(const Sc88& board, std::vector<uint8_t>& image);
		static Result restore(Sc88& board, const std::vector<uint8_t>& image);
	private:
		static bool supported(const Sc88& board);
	};
}
