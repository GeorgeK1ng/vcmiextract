#include "vcmiextract.h"

#include <array>
#include <vector>

#include <lzma.h>

// HotA 1.8 uses LZMA1 raw stream at offset 1 with specific options.
static void decompress_file_hota_lzma(memory_file & source, memory_file & target)
{
    // Skip first byte (custom wrapper), Python uses offset = 1
    size_t total_size = source.size();
    assert(total_size > 1);

    source.skip(1);
    size_t remaining_size = total_size - 1;

    lzma_stream strm = LZMA_STREAM_INIT;

    lzma_options_lzma options;
    memset(&options, 0, sizeof(options));
    options.dict_size = 262144; // 256 KiB
    options.lc = 3;
    options.lp = 0;
    options.pb = 2;

    lzma_filter filters[2];
    filters[0].id = LZMA_FILTER_LZMA1;
    filters[0].options = &options;
    filters[1].id = LZMA_VLI_UNKNOWN;
    filters[1].options = nullptr;

    lzma_ret ret = lzma_raw_decoder(&strm, filters);
    assert(ret == LZMA_OK);

    strm.next_in = source.ptr();
    strm.avail_in = remaining_size;

    strm.next_out = target.ptr();
    strm.avail_out = target.size();

    while (true)
    {
        ret = lzma_code(&strm, LZMA_FINISH);

        if (ret == LZMA_STREAM_END)
            break;

        // If this fails, buffer size or data are wrong.
        assert(ret == LZMA_OK);
    }

    lzma_end(&strm);
}

// XOR helper for HotA encrypted headers (16 bytes, 4-byte key)
static void xor_decrypt_16(uint8_t * data, const uint8_t * key)
{
    for (size_t i = 0; i < 16; ++i)
        data[i] ^= key[i % 4];
}

// Convert 16 raw bytes to lowercase hex string (Python .hex() style)
static std::string bytes_to_hex_string(const uint8_t * data, size_t size)
{
    static const char * hex = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);

    for (size_t i = 0; i < size; ++i)
    {
        uint8_t v = data[i];
        out.push_back(hex[v >> 4]);
        out.push_back(hex[v & 0x0F]);
    }
    return out;
}

void vcmiextract::extract_lod(memory_file & file, const std::filesystem::path & destination)
{
    // Read total_files and key to detect HotA 1.8, but do not disturb original logic.
    file.set(8);
    uint32_t total_files = file.read<uint32_t>();

    file.set(0x0c);
    std::array<uint8_t, 4> key{};
    file.read(key.data(), key.size());

    bool is_hota_18 = (key[0] == 135); // same detection as Python: key[0] == 135

    if (is_hota_18)
    {
        // ------------ HotA 1.8 LOD branch ------------
        struct archive_entry
        {
            std::array<uint8_t, 16> name{};
            uint32_t offset = 0;
            uint32_t full_size = 0;
            uint32_t compressed_size = 0;
            uint8_t compression_method = 0; // 0 = stored, 3 = zlib, 2 = custom LZMA
            std::array<uint8_t, 3> unknown{};
        };

        std::vector<archive_entry> entries;
        entries.reserve(total_files);

        // In HotA 1.8 table starts at offset 80 (0x50)
        file.set(80);

        for (uint32_t i = 0; i < total_files; ++i)
        {
            archive_entry entry;

            // name is actually 16-byte unique ID, not real filename
            file.read(entry.name.data(), entry.name.size());

            std::array<uint8_t, 16> encrypted;
            file.read(encrypted.data(), encrypted.size());

            std::array<uint8_t, 16> decrypted = encrypted;
            xor_decrypt_16(decrypted.data(), key.data());

            entry.offset = *reinterpret_cast<uint32_t *>(&decrypted[0]);
            entry.full_size = *reinterpret_cast<uint32_t *>(&decrypted[4]);
            entry.compressed_size = *reinterpret_cast<uint32_t *>(&decrypted[8]);

            entry.compression_method = encrypted[12];
            entry.unknown[0] = encrypted[13];
            entry.unknown[1] = encrypted[14];
            entry.unknown[2] = encrypted[15];

            entries.push_back(entry);
        }

        for (const auto & entry : entries)
        {
            file.set(entry.offset);

            // HotA 1.8 has no real filenames – use hex of 16-byte ID
            std::string filename = bytes_to_hex_string(entry.name.data(), entry.name.size());

            if (entry.compressed_size != 0)
            {
                memory_file compressed(file.ptr(), entry.compressed_size);
                memory_file file_data(entry.full_size);

                if (entry.compression_method == 2)
                {
                    // custom wrapper + LZMA-family (our raw LZMA decoder)
                    decompress_file_hota_lzma(compressed, file_data);
                }
                else
                {
                    // 0x03 = zlib / deflate (same as classic LOD)
                    vcmiextract::decompress_file(compressed, file_data);
                }

                vcmiextract::save_file(file_data, destination, filename);
            }
            else
            {
                // Stored (no compression)
                memory_file file_data(file.ptr(), entry.full_size);
                vcmiextract::save_file(file_data, destination, filename);
            }
        }
    }
    else
    {
        // ------------ ORIGINAL H3 LOD BRANCH (UNCHANGED) ------------
        struct archive_entry
        {
            std::array<char, 16> name{};

            uint32_t offset = 0;
            uint32_t full_size = 0;
            uint32_t unused = 0;
            uint32_t compressed_size = 0;
        };

        file.set(8);

        uint32_t total_files = file.read<uint32_t>();

        file.set(0x5c);

        std::vector<archive_entry> entries;

        for(uint32_t i = 0; i < total_files; ++i)
        {
            archive_entry entry;

            file.read(entry.name.data(), entry.name.size());
            file.read(entry.offset);
            file.read(entry.full_size);
            file.read(entry.unused);
            file.read(entry.compressed_size);

            entries.push_back(entry);
        }

        for(const auto & entry : entries)
        {
            file.set(entry.offset);

            if(entry.compressed_size != 0)
            {
                memory_file compressed(file.ptr(), entry.compressed_size);
                memory_file file_data(entry.full_size);

                vcmiextract::decompress_file(compressed, file_data);
                vcmiextract::save_file(file_data, destination, entry.name.data());
            }
            else
            {
                memory_file file_data(file.ptr(), entry.full_size);
                vcmiextract::save_file(file_data, destination, entry.name.data());
            }
        }
    }
}


void vcmiextract::extract_snd(memory_file & file, const std::filesystem::path & destination)
{
	struct archive_entry
	{
		std::array<char, 40> name{};

		uint32_t offset = 0;
		uint32_t full_size = 0;
	};

	uint32_t total_files = file.read<uint32_t>();

	std::vector<archive_entry> entries;

	for(uint32_t i = 0; i < total_files; ++i)
	{
		archive_entry entry;

		file.read(entry.name.data(), entry.name.size());
		file.read(entry.offset);
		file.read(entry.full_size);

		entries.push_back(entry);
	}

	for(const auto & entry : entries)
	{
		file.set(entry.offset);
		memory_file file_data(file.ptr(), entry.full_size);
		vcmiextract::save_file(file_data, destination, std::string(entry.name.data()) + ".wav");
	}
}

void vcmiextract::extract_vid(memory_file & file, const std::filesystem::path & destination)
{
	struct archive_entry
	{
		std::array<char, 40> name{};

		uint32_t begin = 0;
		uint32_t end = 0;
	};

	uint32_t total_files = file.read<uint32_t>();

	std::vector<archive_entry> entries;

	for(uint32_t i = 0; i < total_files; ++i)
	{
		archive_entry entry;

		file.read(entry.name.data(), entry.name.size());
		file.read(entry.begin);

		if(!entries.empty())
			entries.back().end = entry.begin;
		entries.push_back(entry);
	}

	if(!entries.empty())
		entries.back().end = file.size();

	for(const auto & entry : entries)
	{
		file.set(entry.begin);
		memory_file file_data(file.ptr(), entry.end - entry.begin);
		vcmiextract::save_file(file_data, destination, std::string(entry.name.data()));
	}
}
