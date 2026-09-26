#include "88lib/settingsChunk.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace emu88Lib
{
	namespace
	{
		// Version 2 defines little-endian uint32 fields. Version 1 was emitted by the
		// Windows prototype's native-endian ChunkWriter and remains readable unchanged.
		constexpr char g_chunkId[] = "88HW";
		constexpr uint32_t g_chunkVersion = 2;
		constexpr uint32_t g_legacyChunkVersion = 1;
		constexpr size_t g_wordBytes = sizeof(uint32_t);
		constexpr size_t g_byteBits = std::numeric_limits<uint8_t>::digits;
		constexpr size_t g_digestWords = std::tuple_size_v<SettingsChunk::Digest>;
		constexpr size_t g_digestBytes = g_digestWords * g_wordBytes;
		constexpr size_t g_fourCcBytes = sizeof(g_chunkId) - sizeof(char);
		// Preserve the existing ChunkWriter field order and uint32 length widths.
		constexpr size_t g_lengthOffset = g_fourCcBytes + g_wordBytes;
		constexpr size_t g_headerBytes = g_lengthOffset + g_wordBytes;
		constexpr size_t g_layoutOffset = g_headerBytes + g_wordBytes;
		constexpr size_t g_firmwareOffset = g_layoutOffset + g_wordBytes;
		constexpr size_t g_memoryLengthOffset = g_firmwareOffset + g_digestBytes;
		constexpr size_t g_memoryOffset = g_memoryLengthOffset + g_wordBytes;
		constexpr size_t g_fixedBytes = g_memoryOffset + g_digestBytes;
		// baseLib/md5.cpp stores message bit length in uint32_t. Bound its input
		// before hashing; this also keeps its uint32 padding arithmetic from wrapping.
		constexpr size_t g_maximumHashedBytes = std::numeric_limits<uint32_t>::max() / g_byteBits;
		constexpr size_t g_maximumChunkBytes = g_headerBytes + g_maximumHashedBytes + g_digestBytes;

		uint32_t readSize(const uint8_t* _data, const size_t _offset)
		{
			uint32_t value{};
			for(size_t byte{}; byte < g_wordBytes; ++byte)
				value |= uint32_t{_data[_offset + byte]} << (byte * g_byteBits);
			return value;
		}

		void appendWord(std::vector<uint8_t>& bytes, const uint32_t value)
		{
			for(size_t byte{}; byte < g_wordBytes; ++byte)
				bytes.push_back(static_cast<uint8_t>(value >> (byte * g_byteBits)));
		}

		SettingsChunk::Digest readDigest(const uint8_t* bytes, const size_t offset)
		{
			SettingsChunk::Digest result{};
			for(size_t word{}; word < result.size(); ++word)
				result[word] = readSize(bytes, offset + word * g_wordBytes);
			return result;
		}

		bool knownFingerprint(const SettingsChunk::Digest& _digest)
		{
			return std::any_of(_digest.begin(), _digest.end(), [](const auto value) { return value != uint32_t{}; });
		}
	}

	std::vector<uint8_t> SettingsChunk::encode() const
	{
		if(!isDeviceModelValue(static_cast<uint32_t>(model)) || !layout || !knownFingerprint(firmware) ||
		   memory.empty() || memory.size() > g_maximumChunkBytes - g_fixedBytes)
			return {};
		std::vector<uint8_t> result;
		result.reserve(g_fixedBytes + memory.size());
		result.insert(result.end(), g_chunkId, g_chunkId + g_fourCcBytes);
		appendWord(result, g_chunkVersion);
		appendWord(result, static_cast<uint32_t>(g_fixedBytes + memory.size() - g_headerBytes));
		appendWord(result, static_cast<uint32_t>(model));
		appendWord(result, layout);
		for(const auto word : firmware) appendWord(result, word);
		appendWord(result, static_cast<uint32_t>(memory.size()));
		result.insert(result.end(), memory.begin(), memory.end());
		// Covers identity and memory together; this detects corruption, not authenticity.
		const auto checksum = baseLib::MD5(result.data() + g_headerBytes,
			static_cast<uint32_t>(result.size() - g_headerBytes)).getWords();
		for(const auto word : checksum) appendWord(result, word);
		return result;
	}

	std::optional<SettingsChunk> SettingsChunk::decode(const void* _data, const size_t _size)
	{
		// Validate all extents before allocating, including historical version-1 images.
		if(!_data || _size <= g_fixedBytes || _size > g_maximumChunkBytes)
			return {};
		const auto* bytes = static_cast<const uint8_t*>(_data);
		const auto version = readSize(bytes, g_fourCcBytes);
		if(std::memcmp(bytes, g_chunkId, g_fourCcBytes) != int{} ||
		   (version != g_chunkVersion && version != g_legacyChunkVersion) ||
		   readSize(bytes, g_lengthOffset) != _size - g_headerBytes ||
		   readSize(bytes, g_memoryLengthOffset) != _size - g_fixedBytes)
			return {};
		const auto bodyBytes = _size - g_headerBytes - g_digestBytes;
		const auto checksum = baseLib::MD5(bytes + g_headerBytes, static_cast<uint32_t>(bodyBytes)).getWords();
		const auto storedChecksum = readDigest(bytes, _size - g_digestBytes);
		if(checksum != storedChecksum) return {};

		const auto modelValue = readSize(bytes, g_headerBytes);
		if(!isDeviceModelValue(modelValue)) return {};
		SettingsChunk result;
		result.model = static_cast<DeviceModel>(modelValue);
		result.layout = readSize(bytes, g_layoutOffset);
		result.firmware = readDigest(bytes, g_firmwareOffset);
		if(!result.layout || !knownFingerprint(result.firmware)) return {};
		result.memory.assign(bytes + g_memoryOffset, bytes + _size - g_digestBytes);
		return result;
	}
}
