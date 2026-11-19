// SPDX-License-Identifier: Apache-2.0

// Tests of the SHA-256 that names an asset (issue #172, M1-47), against the FIPS 180-4 examples
// NIST publishes (csrc.nist.gov, "Cryptographic Standards and Guidelines: Examples with
// Intermediate Values", SHA-256) and, at the lengths where the padding takes one block or two,
// against Python's hashlib, an implementation of its own.

// The hash is private to libs/assets (src/ is not on the tests' include path), so its header is
// included by its place in the tree.
#include "../src/sha256.hpp"

#include <cs/core/random.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using cs::assets::Sha256;
using cs::assets::sha256;

namespace fs = std::filesystem;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::string sha256_of(std::string_view text) {
    return sha256(bytes_of(text));
}

} // namespace

TEST_CASE("sha256: the NIST examples of FIPS 180-4", "[assets][sha256]") {
    CHECK(sha256_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 448 bits: the padding no longer fits in the message's block and takes a second one.
    CHECK(sha256_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string million_a(1'000'000, 'a');
    CHECK(sha256_of(million_a) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256: messages either side of a block boundary, against Python's hashlib",
          "[assets][sha256]") {
    // 55 bytes leave room in the block for the 0x80 byte and the 8-byte length; 56 do not. 64 and
    // 120 end a block exactly or leave no room in the second.
    struct Case {
        std::size_t length;
        std::string_view digest;
    };
    const Case cases[] = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
    };
    for (const Case& c : cases) {
        INFO(c.length << " bytes of 'a'");
        CHECK(sha256_of(std::string(c.length, 'a')) == c.digest);
    }
}

TEST_CASE("sha256: the digest has no state of its own: it can be read and the message go on",
          "[assets][sha256]") {
    Sha256 hash;
    hash.update(bytes_of("ab"));
    CHECK(hash.digest() == sha256_of("ab"));
    CHECK(hash.digest() == sha256_of("ab"));
    hash.update(bytes_of("c"));
    CHECK(hash.digest() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256: a file hashed in chunks gives the same digest as its bytes at once",
          "[assets][sha256]") {
    // 100003 seeded random bytes: not a whole number of blocks, nor of any chunk size below.
    const fs::path path = fs::current_path() / "cs_assets_test_sha256_chunks.bin";
    std::vector<std::byte> bytes(100'003);
    cs::Rng rng(cs::hash_seed(172, 0, 0, 0, 0));
    for (std::byte& b : bytes) {
        b = static_cast<std::byte>(rng.next_u32() >> 24);
    }
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        REQUIRE(file);
    }
    const std::string at_once = sha256(bytes);

    for (const std::size_t chunk_bytes :
         {std::size_t{1}, std::size_t{7}, std::size_t{63}, std::size_t{64}, std::size_t{65},
          std::size_t{4096}, std::size_t{1} << 20}) {
        INFO("chunks of " << chunk_bytes << " bytes");
        std::ifstream file(path, std::ios::binary);
        REQUIRE(file);
        Sha256 hash;
        std::vector<std::byte> chunk(chunk_bytes);
        std::size_t read = 0;
        while (file) {
            file.read(reinterpret_cast<char*>(chunk.data()),
                      static_cast<std::streamsize>(chunk.size()));
            const auto got = static_cast<std::size_t>(file.gcount());
            hash.update(std::span(chunk).first(got));
            read += got;
        }
        CHECK(read == bytes.size());
        CHECK(hash.digest() == at_once);
    }

    std::error_code ignored;
    fs::remove(path, ignored);
}
