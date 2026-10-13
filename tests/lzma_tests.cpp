#include "lzma.h"
#include "test_utils.h"
#include <iostream>

int main(int argc, char** argv) {
	try {
		require(argc == 2, "expected fixture directory");
		const std::filesystem::path fixtures{ argv[1] };
		const auto text{ fixture_bytes(fixtures / "frames.txt") };
		const auto golden{ fixture_bytes(fixtures / "frames-sdk457.lzma") };
		lzma::properties stable{};
		stable.dictSize = 1u << 21;
		stable.fb = 255;
		std::array<uint8_t, 5> props;
		const auto encoded{ lzma::encode(text, stable, props) };
		require(std::equal(props.begin(), props.end(), golden.begin()), "stable LZMA properties changed");
		require(encoded == std::vector<uint8_t>(golden.begin() + 13, golden.end()), "SDK 4.57 compressed bytes changed");
		std::vector<uint8_t> decoded;
		require(lzma::decode(encoded, props, decoded, text.size()) && decoded == text, "stable decode changed text");

		for (uint32_t lp : { 0u, 1u }) {
			lzma::properties internal{};
			internal.dictSize = static_cast<uint32_t>(text.size());
			internal.lc = 0;
			internal.lp = lp;
			internal.pb = 0;
			internal.mc = 16;
			const auto stream{ lzma::encode(text, internal, props) };
			require(lzma::decode(stream, props, decoded, text.size()) && decoded == text, "ORZ stream round trip failed");
		}

		lzma::properties fast{};
		fast.algo = 0;
		fast.btMode = false;
		fast.mc = 16;
		const auto stream{ lzma::encode(text, fast, props) };
		require(lzma::decode(stream, props, decoded, text.size()) && decoded == text, "fast round trip failed");
		auto large_dictionary{ props };
		std::fill(large_dictionary.begin() + 1, large_dictionary.end(), 255);
		require(lzma::decode(stream, large_dictionary, decoded, text.size()) && decoded == text, "dictionary was not bounded by output size");
		require(!lzma::decode(std::span(stream).first(stream.size() / 2), props, decoded, text.size()), "truncated stream accepted");
		props[0] = 225;
		require(!lzma::decode(stream, props, decoded, text.size()), "invalid properties accepted");
		const auto empty{ lzma::encode({}, stable, props) };
		require(lzma::decode(empty, props, decoded, 0) && decoded.empty(), "empty stream failed");
		std::cout << "SDK 4.57 golden bytes and LZMA round trips passed\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << e.what() << '\n';
		return 1;
	}
}
