#pragma once

/**
 * @file clip_fixtures.h
 * @brief Audio fixture files for sfxcoopa's suites: hermetic copies of the committed ex/ clips,
 *        a synthetic smpl-looped wav writer, and a raw `data`-chunk reader for bit-exact checks.
 *
 * Every clip a test decodes is a copy in the test's scratch_dir(), never the file in ex/
 * directly. Decoding is sidecar-aware (`<clip>.import`), so a sidecar left next to a committed
 * fixture -- the sfxcoopa_playback demo writes them there, and a killed run can leave one
 * behind -- would otherwise silently change how a test decodes it (a stray force_mono once
 * broke every stereo-clip assumption in the old suite). A fresh copy has no sidecar unless the
 * test writes one, and a sidecar a test writes lands in scratch, not the repo.
 */

#include <coopa/testing/test.h>
#include <coopa/util/file.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace sfx_test {

/** @brief The 44.1 kHz stereo float32 whoosh the decoder/clip/engine tests use (31903 frames). */
inline constexpr const char* k_whoosh = "SEFE_Whoosh03.wav";
/** @brief The same whoosh, mp3-encoded. */
inline constexpr const char* k_whoosh_mp3 = "SEFE_Whoosh03.mp3";
/** @brief Frames in k_whoosh at its own 44.1 kHz rate. */
inline constexpr uint64_t k_whoosh_frames = 31903;

/**
 * @brief Copies `ex/<name>` into this test's scratch dir (as `<copy_as>` when given) and
 *        returns the copy's path. See the file comment for why tests never decode ex/ directly.
 */
inline std::string fixture_copy(const std::string& name, const std::string& copy_as = {}) {
    const std::filesystem::path dst = coopa::test::scratch_dir("fixtures") / (copy_as.empty() ? name : copy_as);
    std::filesystem::copy_file(FileUtil::get_root_path("ex/" + name), dst,
                               std::filesystem::copy_options::overwrite_existing);
    return dst.string();
}

namespace detail {
inline void append_u32(std::string& buf, uint32_t v) {
    buf.append(reinterpret_cast<const char*>(&v), sizeof(v));
}
inline void append_chunk(std::string& buf, const char id[4], const std::string& payload) {
    buf.append(id, 4);
    append_u32(buf, static_cast<uint32_t>(payload.size()));
    buf += payload;
    if (payload.size() & 1) buf.push_back('\0');  // RIFF chunks pad to even size
}
}  // namespace detail

/**
 * @brief Writes a minimal synthetic wav into scratch with a leading JUNK chunk (the classic Pro
 *        Tools/Reaper padding chunk a naive reader trips on) and a `smpl` chunk holding one loop.
 *        Returns its path.
 */
inline std::string write_synthetic_wav_with_loop(uint32_t loop_start, uint32_t loop_end,
                                                 const std::string& filename = "synthetic.wav") {
    using detail::append_chunk;
    using detail::append_u32;
    std::string body;  // everything after "RIFF"<size>, i.e. starting at "WAVE"
    body += "WAVE";

    append_chunk(body, "JUNK", std::string(4, '\0'));

    std::string fmt;
    uint16_t format_tag = 1, channels = 1, block_align = 2, bits_per_sample = 16;
    uint32_t sample_rate = 44100, byte_rate = 44100 * 2;
    fmt.append(reinterpret_cast<const char*>(&format_tag), 2);
    fmt.append(reinterpret_cast<const char*>(&channels), 2);
    fmt.append(reinterpret_cast<const char*>(&sample_rate), 4);
    fmt.append(reinterpret_cast<const char*>(&byte_rate), 4);
    fmt.append(reinterpret_cast<const char*>(&block_align), 2);
    fmt.append(reinterpret_cast<const char*>(&bits_per_sample), 2);
    append_chunk(body, "fmt ", fmt);

    append_chunk(body, "data", std::string(8, '\0'));  // 4 silent int16 samples

    std::string smpl;
    uint32_t header[9] = {0, 0, 0, 60, 0, 0, 0, 1, 0};  // num_sample_loops = 1 at index 7
    smpl.append(reinterpret_cast<const char*>(header), sizeof(header));
    uint32_t loop_entry[6] = {0, 0, loop_start, loop_end, 0, 0};
    smpl.append(reinterpret_cast<const char*>(loop_entry), sizeof(loop_entry));
    append_chunk(body, "smpl", smpl);

    std::string riff = "RIFF";
    append_u32(riff, static_cast<uint32_t>(body.size()));
    riff += body;

    const std::filesystem::path path = coopa::test::scratch_dir("fixtures") / filename;
    std::ofstream out(path, std::ios::binary);
    out.write(riff.data(), static_cast<std::streamsize>(riff.size()));
    return path.string();
}

/**
 * @brief Test-only raw chunk reader: the raw bytes of a wav's `data` chunk -- an independent
 *        path from the decoder under test, for bit-exact comparisons.
 */
inline std::vector<uint8_t> read_wav_data_chunk_raw(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    char riff_id[4], wave_id[4];
    uint32_t riff_size = 0;
    file.read(riff_id, 4);
    file.read(reinterpret_cast<char*>(&riff_size), 4);
    file.read(wave_id, 4);
    while (file.good()) {
        char chunk_id[4];
        uint32_t chunk_size = 0;
        file.read(chunk_id, 4);
        if (!file) break;
        file.read(reinterpret_cast<char*>(&chunk_size), 4);
        if (!file) break;
        std::streampos data_start = file.tellg();
        if (std::string(chunk_id, 4) == "data") {
            std::vector<uint8_t> bytes(chunk_size);
            file.read(reinterpret_cast<char*>(bytes.data()), chunk_size);
            return bytes;
        }
        std::streamoff advance = static_cast<std::streamoff>(chunk_size) + (chunk_size & 1u);
        file.seekg(data_start + advance);
    }
    return {};
}

}  // namespace sfx_test
