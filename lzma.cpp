#include "lzma.h"

#include "Lib/CPP/7zip/Compress/LZMA/LZMADecoder.h"
#include "Lib/CPP/7zip/Compress/LZMA/LZMAEncoder.h"

extern "C" {
#include "Lib/C/7zCrc.h"
}

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace {

class memory_input : public ISequentialInStream, public CMyUnknownImp {
	std::span<const uint8_t> data;
	size_t position{};
public:
	MY_UNKNOWN_IMP

	explicit memory_input(std::span<const uint8_t> bytes) : data(bytes) {}

	STDMETHOD(Read)(void* output, UInt32 size, UInt32* processed) {
		const auto count{ std::min<size_t>(size, data.size() - position) };
		if (count)
			memcpy(output, data.data() + position, count);
		position += count;
		if (processed)
			*processed = static_cast<UInt32>(count);
		return S_OK;
	}
};

class memory_output : public ISequentialOutStream, public CMyUnknownImp {
	std::vector<uint8_t>& data;
	size_t limit;
public:
	MY_UNKNOWN_IMP

	explicit memory_output(std::vector<uint8_t>& bytes,
		size_t max_size = std::numeric_limits<size_t>::max()) : data(bytes), limit(max_size) {}

	STDMETHOD(Write)(const void* input, UInt32 size, UInt32* processed) {
		if (size > limit - data.size()) {
			if (processed)
				*processed = 0;
			return E_FAIL;
		}
		if (size) {
			const auto* bytes{ static_cast<const uint8_t*>(input) };
			data.insert(data.end(), bytes, bytes + size);
		}
		if (processed)
			*processed = size;
		return S_OK;
	}
};

}

namespace lzma {

std::vector<uint8_t> encode(std::span<const uint8_t> input, const properties& p,
	std::array<uint8_t, 5>& encoded_properties) {

	static std::once_flag crc_table;
	std::call_once(crc_table, CrcGenerateTable);

	auto* impl{ new NCompress::NLZMA::CEncoder };
	CMyComPtr<ICompressCoder> encoder{ impl };

	const PROPID ids[]{
		NCoderPropID::kDictionarySize, NCoderPropID::kPosStateBits,
		NCoderPropID::kLitContextBits, NCoderPropID::kLitPosBits,
		NCoderPropID::kAlgorithm, NCoderPropID::kNumFastBytes,
		NCoderPropID::kMatchFinderCycles, NCoderPropID::kMatchFinder,
		NCoderPropID::kEndMarker, NCoderPropID::kNumThreads
	};
	PROPVARIANT values[10]{};
	for (size_t i{}; i < 7; ++i)
		values[i].vt = VT_UI4;
	// SDK 4.57 requires at least a 4 KiB encoder dictionary.
	values[0].ulVal = std::max<uint32_t>(p.dictSize, 1u << 12);
	values[1].ulVal = p.pb;
	values[2].ulVal = p.lc;
	values[3].ulVal = p.lp;
	values[4].ulVal = p.algo;
	values[5].ulVal = p.fb;
	values[6].ulVal = p.mc;
	values[7].vt = VT_BSTR;
	values[7].bstrVal = const_cast<wchar_t*>(p.btMode ? L"BT4" : L"HC4");
	values[8].vt = VT_BOOL;
	values[8].boolVal = VARIANT_FALSE;
	values[9].vt = VT_UI4;
	values[9].ulVal = 1;

	if (impl->SetCoderProperties(ids, values, 10) != S_OK)
		throw std::runtime_error("invalid LZMA encoder properties");

	std::vector<uint8_t> property_bytes;
	CMyComPtr<ISequentialOutStream> property_output{ new memory_output(property_bytes, 5) };
	if (impl->WriteCoderProperties(property_output) != S_OK || property_bytes.size() != 5)
		throw std::runtime_error("failed to write LZMA properties");
	std::copy(property_bytes.begin(), property_bytes.end(), encoded_properties.begin());

	std::vector<uint8_t> output;
	CMyComPtr<ISequentialInStream> in{ new memory_input(input) };
	CMyComPtr<ISequentialOutStream> out{ new memory_output(output) };
	if (encoder->Code(in, out, nullptr, nullptr, nullptr) != S_OK)
		throw std::runtime_error("LZMA encode failed");
	return output;
}

bool decode(std::span<const uint8_t> input, std::span<const uint8_t, 5> properties,
	std::vector<uint8_t>& output, size_t expected_size) {

	output.clear();
	auto* impl{ new NCompress::NLZMA::CDecoder };
	CMyComPtr<ICompressCoder> decoder{ impl };
	std::array<uint8_t, 5> decode_properties;
	std::copy(properties.begin(), properties.end(), decode_properties.begin());
	uint32_t dictionary{};
	memcpy(&dictionary, decode_properties.data() + 1, 4);
	// A known-size stream cannot refer farther back than its total output size.
	// Unlike the newer memory decoder, this SDK allocates a separate dictionary.
	dictionary = static_cast<uint32_t>(std::min<size_t>(dictionary, expected_size));
	memcpy(decode_properties.data() + 1, &dictionary, 4);
	if (impl->SetDecoderProperties2(decode_properties.data(), 5) != S_OK)
		return false;

	CMyComPtr<ISequentialInStream> in{ new memory_input(input) };
	CMyComPtr<ISequentialOutStream> out{ new memory_output(output, expected_size) };
	const UInt64 input_size{ input.size() }, output_size{ expected_size };
	UInt64 consumed{};
	if (decoder->Code(in, out, &input_size, &output_size, nullptr) != S_OK ||
		output.size() != expected_size ||
		impl->GetInStreamProcessedSize(&consumed) != S_OK || consumed > input.size()) {
		output.clear();
		return false;
	}
	return true;
}

}
