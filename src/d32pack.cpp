
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <filesystem>
#include <stdexcept>
#include <iostream>
#include <fstream>

#if __has_include(<libpng16/png.h>)
#include <libpng16/png.h>
#else
#include <libpng/png.h>
#endif

struct FrameRec {
    uint32_t group = 0;
    uint32_t frame = 0;
    std::filesystem::path file; // PNG path (on disk)
    std::string internal_name;  // 13-byte name written into D32F (e.g., "frame000.pcx")
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;  // width*height*4, top-down
};

static std::string to_internal_name(std::string base)
{
    // Replace extension with .pcx and truncate to 12 chars + null (13 total)
    auto p = std::filesystem::path(base);
    p.replace_extension(".pcx");
    std::string s = p.filename().string();
    if (s.size() > 12) s = s.substr(0, 12);
    return s;
}

static FrameRec load_png_rgba(const std::filesystem::path& path)
{
    FrameRec fr;
    fr.file = path;
    FILE* fp = fopen(path.string().c_str(), "rb");
    if (!fp) throw std::runtime_error("Failed to open PNG: " + path.string());

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { fclose(fp); throw std::runtime_error("png_create_read_struct failed"); }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, nullptr, nullptr); fclose(fp); throw std::runtime_error("png_create_info_struct failed"); }
    if (setjmp(png_jmpbuf(png))) { png_destroy_read_struct(&png, &info, nullptr); fclose(fp); throw std::runtime_error("libpng read error"); }

    png_init_io(png, fp);
    png_read_info(png, info);

    png_uint_32 w, h;
    int bit_depth, color_type;
    png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, nullptr, nullptr, nullptr);

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (!(color_type & PNG_COLOR_MASK_ALPHA)) png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);

    png_read_update_info(png, info);
    png_size_t rowbytes = png_get_rowbytes(png, info);
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    std::vector<png_bytep> rows(h);
    for (png_uint_32 y = 0; y < h; ++y) {
        rows[y] = reinterpret_cast<png_bytep>(buf.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4);
    }

    png_read_image(png, rows.data());
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(fp);

    fr.width = static_cast<uint32_t>(w);
    fr.height = static_cast<uint32_t>(h);
    fr.rgba = std::move(buf);
    fr.internal_name = to_internal_name(path.filename().string());
    return fr;
}

static void write_u32_le(FILE* fp, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v), (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) };
    if (fwrite(b,1,4,fp)!=4) throw std::runtime_error("Write error");
}

static void write_bytes(FILE* fp, const void* data, size_t n) {
    if (fwrite(data,1,n,fp)!=n) throw std::runtime_error("Write error");
}

struct EntryDir {
    std::array<char,13> name{};
    uint32_t offset{};
};

static std::vector<FrameRec> parse_animation_json(const std::filesystem::path& dir)
{
    std::vector<FrameRec> frames;
    auto json_path = dir / "animation.json";
    if (!std::filesystem::exists(json_path)) {
        // Fallback: take all *.png sorted by name, single group 0, frame order by name
        std::vector<std::filesystem::path> files;
        for (auto& it : std::filesystem::directory_iterator(dir)) {
            if (it.is_regular_file() && it.path().extension()==".png") files.push_back(it.path());
        }
        std::sort(files.begin(), files.end());
        uint32_t idx = 0;
        for (auto& f : files) {
            FrameRec fr = load_png_rgba(f);
            fr.group = 0;
            fr.frame = idx++;
            frames.push_back(std::move(fr));
        }
        return frames;
    }

    // Minimal/robust ad-hoc parser: scan lines for "group", "frame", "file"
    std::ifstream ifs(json_path);
    if (!ifs) throw std::runtime_error("Failed to open " + json_path.string());
    std::string line;
    uint32_t cur_group = 0;
    uint32_t cur_frame = 0;
    std::string cur_file;
    bool have_group = false, have_frame = false, have_file = false;
    auto flush = [&]() {
        if (have_frame && have_file) {
            FrameRec fr = load_png_rgba(dir / cur_file);
            fr.group = have_group ? cur_group : 0;
            fr.frame = cur_frame;
            frames.push_back(std::move(fr));
        }
        have_group = have_frame = have_file = false;
        cur_group = 0;
        cur_frame = 0;
        cur_file.clear();
    };

    while (std::getline(ifs, line)) {
        auto s = line;
        // crude strip
        s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c){ return c=='\\r' || c=='\\n' || c=='\\t'; }), s.end());
        auto find_val = [&](const char* key)->std::string {
            auto pos = s.find(key);
            if (pos == std::string::npos) return {};
            pos = s.find(':', pos);
            if (pos == std::string::npos) return {};
            pos++;
            while (pos < s.size() && (s[pos]==' ')) pos++;
            return s.substr(pos);
        };
        if (s.find("\"group\"") != std::string::npos) {
            std::string v = find_val("\"group\"");
            cur_group = (uint32_t)std::stoul(v);
            have_group = true;
        }
        if (s.find("\"frame\"") != std::string::npos) {
            std::string v = find_val("\"frame\"");
            cur_frame = (uint32_t)std::stoul(v);
            have_frame = true;
        }
        if (s.find("\"file\"") != std::string::npos) {
            std::string v = find_val("\"file\"");
            // remove quotes and trailing commas/braces
            size_t q1 = v.find('\"');
            size_t q2 = v.find('\"', q1+1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                cur_file = v.substr(q1+1, q2-(q1+1));
                have_file = true;
                flush();
            }
        }
    }
    return frames;
}

static int pack_d32f(const std::filesystem::path& dir, const std::filesystem::path& out_path)
{
    // Load frames
    auto frames = parse_animation_json(dir);
    if (frames.empty()) {
        std::cerr << "No frames found in " << dir << "\\n";
        return 1;
    }
    // Group by group id, sort by frame index within
    std::map<uint32_t, std::vector<FrameRec>> groups;
    for (auto& fr : frames) groups[fr.group].push_back(std::move(fr));
    for (auto& kv : groups) {
        std::sort(kv.second.begin(), kv.second.end(), [](const FrameRec& a, const FrameRec& b){ return a.frame < b.frame; });
    }

    FILE* fp = fopen(out_path.string().c_str(), "wb");
    if (!fp) throw std::runtime_error("Failed to open output: " + out_path.string());

    // File header
    write_u32_le(fp, 0x46323344); // 'D32F'
    write_u32_le(fp, 1);          // unknown1
    write_u32_le(fp, 24);         // unknown2
    write_u32_le(fp, 0);          // width (unused)
    write_u32_le(fp, 0);          // height (unused)
    write_u32_le(fp, (uint32_t)groups.size()); // total_groups
    write_u32_le(fp, 8);          // unknown6
    write_u32_le(fp, groups.size()>1 ? 22u : 1u); // unknown7 as in extractor

    // Build directory tables in memory to compute offsets
    struct GroupDir { uint32_t header_size; uint32_t index; uint32_t size; uint32_t unknown2; std::vector<EntryDir> entries; uint32_t offsets_pos; };
    std::vector<GroupDir> gdirs;
    gdirs.reserve(groups.size());
    for (auto& kv : groups) {
        GroupDir gd{};
        gd.index = kv.first;
        gd.size = (uint32_t)kv.second.size();
        gd.unknown2 = 0;
        gd.entries.resize(gd.size);
        for (size_t i=0;i<kv.second.size();++i) {
            EntryDir ed{};
            std::string nm = kv.second[i].internal_name;
            std::memset(ed.name.data(), 0, ed.name.size());
            std::memcpy(ed.name.data(), nm.c_str(), (std::min)(nm.size(), ed.name.size()-1));
            ed.offset = 0; // fill later
            gd.entries[i] = ed;
        }
        gd.header_size = 17 * gd.size + 16;
        gdirs.push_back(std::move(gd));
    }

    // Write group directory blocks (names + placeholder offsets)
    for (auto& gd : gdirs) {
        write_u32_le(fp, gd.header_size);
        write_u32_le(fp, gd.index);
        write_u32_le(fp, gd.size);
        write_u32_le(fp, gd.unknown2);
        // names
        for (auto& ed : gd.entries) {
            write_bytes(fp, ed.name.data(), ed.name.size());
        }
        // remember position to patch offsets
        long offsets_pos = ftell(fp);
        for (size_t i=0;i<gd.entries.size();++i) write_u32_le(fp, 0);
        // store the offset positions for patching later
        gd.unknown2 = (uint32_t)offsets_pos; // reuse field to hold patch position
    }

    // Now write each group's frames and patch offsets
    size_t gidx = 0;
    for (auto& kv : groups) {
        GroupDir& gd = gdirs[gidx++];
        // reposition to end for sure
        fseek(fp, 0, SEEK_END);
        // Write frames
        for (size_t i=0;i<kv.second.size();++i) {
            long here = ftell(fp);
            // Patch offset
            long save = ftell(fp);
            // Patch in the directory (offset table)
            long off_tab = gd.unknown2 + (long)(i*4);
            long cur = ftell(fp);
            fseek(fp, off_tab, SEEK_SET);
            write_u32_le(fp, (uint32_t)here);
            fseek(fp, cur, SEEK_SET);

            const FrameRec& fr = kv.second[i];
            // Per-frame header
            write_u32_le(fp, 32);                          // bits_per_pixel
            write_u32_le(fp, fr.width * fr.height * 4);    // image_size
            write_u32_le(fp, fr.width);                    // full_width
            write_u32_le(fp, fr.height);                   // full_height
            write_u32_le(fp, fr.width);                    // stored_width
            write_u32_le(fp, fr.height);                   // stored_height
            write_u32_le(fp, 0);                           // margin_left
            write_u32_le(fp, 0);                           // margin_top
            write_u32_le(fp, 8);                           // entry_unknown1
            write_u32_le(fp, 0);                           // entry_unknown2

            // Pixel data: bottom-up rows, each row is stored_width * 4 bytes
            for (uint32_t y = 0; y < fr.height; ++y) {
                const uint8_t* row = fr.rgba.data() + (size_t)(fr.height - 1 - y) * (size_t)fr.width * 4;
                write_bytes(fp, row, (size_t)fr.width * 4);
            }
        }
    }

    fclose(fp);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "Usage:\\n  d32pack <folder_with_pngs_and_animation.json> <out.def>\\n";
        return 1;
    }
    std::filesystem::path in_dir = argv[1];
    std::filesystem::path out_file = argv[2];
    try {
        return pack_d32f(in_dir, out_file);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\\n";
        return 1;
    }
}
