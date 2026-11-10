/**
 * @file wav_chunks.h
 * @brief Standalone RIFF chunk walker for wav metadata miniaudio's decoder
 * doesn't surface -- specifically the `smpl` chunk's sample-loop markers.
 */

#ifndef SFXCOOPA_DATA_WAV_CHUNKS_H
#define SFXCOOPA_DATA_WAV_CHUNKS_H

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace coopa {
namespace sfx {
namespace data {

/** @brief One loop region as authored in a wav file's `smpl` chunk. */
struct SampleLoop {
    uint32_t start;
    uint32_t end;
};

/**
 * @brief Reads all sample-loop markers from a wav file's `smpl` chunk, if present.
 *
 * Walks the RIFF chunk list rather than assuming `fmt ` sits at a fixed
 * offset -- files exported by Pro Tools/Reaper commonly lead with a `JUNK`
 * padding chunk, which breaks naive parsers that assume `fmt ` is the
 * first chunk after the 12-byte RIFF/WAVE header. Also honours the RIFF
 * rule that every chunk is padded to an even byte count.
 *
 * @param path Filesystem path to a .wav file.
 * @return The loop regions from the `smpl` chunk, or an empty vector if
 *         the file has no `smpl` chunk (not an error) or zero loops.
 * @throws std::runtime_error if the file can't be opened or isn't RIFF/WAVE.
 */
inline std::vector<SampleLoop> read_smpl_loops(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("[sfxcoopa] read_smpl_loops: could not open '" + path + "'");
    }

    char riff_id[4];
    uint32_t riff_size = 0;
    char wave_id[4];
    file.read(riff_id, 4);
    file.read(reinterpret_cast<char*>(&riff_size), sizeof(riff_size));
    file.read(wave_id, 4);
    if (!file || std::string(riff_id, 4) != "RIFF" || std::string(wave_id, 4) != "WAVE") {
        throw std::runtime_error("[sfxcoopa] read_smpl_loops: '" + path + "' is not a RIFF/WAVE file");
    }

    std::vector<SampleLoop> loops;
    while (file.good()) {
        char chunk_id[4];
        uint32_t chunk_size = 0;
        file.read(chunk_id, 4);
        if (!file) break;  // clean EOF between chunks
        file.read(reinterpret_cast<char*>(&chunk_size), sizeof(chunk_size));
        if (!file) break;
        const std::streampos data_start = file.tellg();

        if (std::string(chunk_id, 4) == "smpl") {
            // Fixed 36-byte smpl header: manufacturer, product, sample_period,
            // midi_unity_note, midi_pitch_fraction, smpte_format, smpte_offset,
            // num_sample_loops, sampler_data -- 9 uint32_t fields.
            uint32_t header[9] = {};
            file.read(reinterpret_cast<char*>(header), sizeof(header));
            if (file) {
                const uint32_t num_loops = header[7];
                for (uint32_t i = 0; i < num_loops && file; ++i) {
                    // Each loop entry: cue_point_id, type, start, end, fraction, play_count.
                    uint32_t entry[6] = {};
                    file.read(reinterpret_cast<char*>(entry), sizeof(entry));
                    if (!file) break;
                    loops.push_back(SampleLoop{entry[2], entry[3]});
                }
            }
        }

        // Chunks are padded to an even byte count; skip past whichever of
        // this chunk's payload we didn't parse, plus that pad byte if odd.
        const std::streamoff advance = static_cast<std::streamoff>(chunk_size) + (chunk_size & 1u);
        file.seekg(data_start + advance);
    }

    return loops;
}

}  // namespace data
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_DATA_WAV_CHUNKS_H
