#include "vcmiextract.h"

#include "hota_file_names.h"

#include <array>
#include <map>
#include <vector>

static void extract_lod_h3(memory_file & file, const std::filesystem::path & destination)
{
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

            vcmiextract::decompress_file_deflate(compressed, file_data);
			vcmiextract::save_file(file_data, destination, entry.name.data());
		}
		else
		{
			memory_file file_data(file.ptr(), entry.full_size);
			vcmiextract::save_file(file_data, destination, entry.name.data());
		}
	}
}

static void guess_type_and_save(memory_file & file_data, const std::filesystem::path & destination, const std::string & filename)
{
    switch (file_data.peek<uint32_t>())
    {
    case 0x46323344: // D32F
        vcmiextract::save_file(file_data, destination, filename + ".d32");
        return;
    case 0x46323350: // P32
        vcmiextract::save_file(file_data, destination, filename + ".p32");
        return;
    }

    if (file_data.size() > 12)
    {
        int32_t pcx_size = file_data.read<int32_t>();
        int32_t pcx_width = file_data.read<int32_t>();
        int32_t pcx_height = file_data.read<int32_t>();
        file_data.set(0);

        if (pcx_size == pcx_width * pcx_height || pcx_size == 3 * pcx_width * pcx_height)
        {
            vcmiextract::save_file(file_data, destination, filename + ".pcx");
            return;
        }
    }

    printf("unrecognized file '%s', magic %x\n", filename.c_str(), file_data.peek<uint32_t>());
    vcmiextract::save_file(file_data, destination, filename + ".bin");
    return;

}

// FNV-1a, the hash HotA archives use in place of file names
static constexpr uint32_t hash_file_name(const char * name)
{
    uint32_t hash = 0x811c9dc5;

    for(const char * it = name; *it != '\0'; ++it)
        hash = (hash ^ static_cast<uint8_t>(*it)) * 0x01000193;

    return hash;
}

static_assert(hash_file_name("4lvlxshrn.def") == 0x6E54189F);

static void extract_lod_hota(memory_file & file, const std::filesystem::path & destination)
{
    struct archive_entry
    {
        uint32_t name_hash = 0;
        uint32_t offset = 0;
        uint32_t full_size = 0;
        uint32_t compressed_size = 0;
        uint8_t compression_method = 0;
        std::array<char, 15> unknown{};
    };

    std::map<uint32_t, const char *> known_names;

    for(const char * name : hota_file_names)
        known_names[hash_file_name(name)] = name;

    file.set(8);
    uint32_t total_files = file.read<uint32_t>();
    uint32_t xor_mask = file.read<uint32_t>();

    file.set(0x5c);

    std::vector<archive_entry> entries;

    for(uint32_t i = 0; i < total_files; ++i)
    {
        archive_entry entry;

        file.read(entry.name_hash);
        file.read(entry.offset);
        file.read(entry.full_size);
        file.read(entry.compressed_size);
        file.read(entry.compression_method);
        file.read(entry.unknown.data(), entry.unknown.size());

        entry.offset ^= xor_mask;
        entry.full_size ^= xor_mask;
        entry.compressed_size ^= xor_mask;

        assert((entry.compression_method == 0) == (entry.compressed_size == 0));
        assert(entry.compression_method == 0 || entry.compression_method == 2 || entry.compression_method == 3);

        entries.push_back(entry);
    }

    for(const auto & entry : entries)
    {
        file.set(entry.offset);

        memory_file stored(file.ptr(), entry.compression_method == 0 ? entry.full_size : entry.compressed_size);
        memory_file decompressed(entry.full_size);

        if(entry.compression_method == 3)
            vcmiextract::decompress_file_deflate(stored, decompressed);
        else if(entry.compression_method == 2)
            vcmiextract::decompress_file_lzma(stored, decompressed);

        memory_file & file_data = entry.compression_method == 0 ? stored : decompressed;

        auto known_name = known_names.find(entry.name_hash);

        if(known_name != known_names.end())
        {
            vcmiextract::save_file(file_data, destination, known_name->second);
        }
        else
        {
            char fallback_name[32];
            snprintf(fallback_name, sizeof(fallback_name), "unknown_%08x", entry.name_hash);
            guess_type_and_save(file_data, destination, fallback_name);
        }
    }
}

void vcmiextract::extract_lod(memory_file & file, const std::filesystem::path & destination)
{
    int xor_mask;
    file.set(0x0c);
    file.read(xor_mask);

    if (xor_mask == 0)
        return extract_lod_h3(file, destination);
    else
        return extract_lod_hota(file, destination);
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
