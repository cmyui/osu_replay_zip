#include "orz_shared.h"
#include "orz_write.h"
#include "orz_read.h"
#include "test_utils.h"
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
	try {
		require(argc == 3, "expected fixture and output directories");
		const std::filesystem::path fixtures{ argv[1] }, output{ argv[2] };
		std::filesystem::create_directories(output);
		const auto original{ fixture_bytes(fixtures / "synthetic.osr") };
		const auto text_bytes{ fixture_bytes(fixtures / "frames.txt") };
		const std::string text(text_bytes.begin(), text_bytes.end());
		const auto restored{ (output / "restored.osr").string() };

		// Existing SDK 21.02 ORZ streams must remain readable with CRC checks on.
		require(orz_to_osr((fixtures / "sdk21.orz").string().c_str(), restored.c_str(), DECODE_OSU_STABLE_LZMA), "SDK 21 ORZ decode failed");
		require(read_file(restored.c_str()) == original, "existing ORZ stable reconstruction differs");

		const auto zipped{ (output / "synthetic.orz").string() };
		orz_state state{}; // ENCODE_CHECK_ROUND_TRIP stays disabled.
		osr_to_orz((fixtures / "synthetic.osr").string().c_str(), zipped.c_str(), ORZ_FLAG::has_osr_header, state);
		require(!state.output.empty(), "new ORZ encode failed");
		_decoded_orz decoded;
		require(orz_decode_frames(state.output, decoded), "new ORZ frame/CRC check failed");
		require(!(decoded.flags & ORZ_FLAG::replay_verbatim), "fixture unexpectedly used verbatim fallback");
		require(replay_text(decoded.frames, decoded.flags, decoded.streams) == text, "new ORZ frame text changed");
		require(orz_to_osr(zipped.c_str(), restored.c_str(), DECODE_OSU_STABLE_LZMA), "new stable reconstruction failed");
		require(read_file(restored.c_str()) == original, "new stable reconstruction differs");
		require(orz_to_osr(zipped.c_str(), restored.c_str()), "default reconstruction failed");
		const auto fast{ fixture_bytes(output / "restored.osr") };
		require(fast != original, "default unexpectedly selected stable settings");
		const auto payload_offset{ original.size() - fixture_bytes(fixtures / "frames-sdk457.lzma").size() - 8 };
		require(fast.size() >= payload_offset + 13 + 8, "default OSR payload missing");
		u32 payload_size{};
		memcpy(&payload_size, fast.data() + payload_offset - 4, 4);
		require(payload_size <= fast.size() - payload_offset - 8, "default OSR payload length invalid");
		std::string fast_text;
		require(decompress_osr_string(fast.data() + payload_offset, payload_size, fast_text) && fast_text == text, "default OSR text changed");
		std::cout << "Existing ORZ compatibility and new byte-identical stable round trip passed\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << e.what() << '\n';
		return 1;
	}
}
