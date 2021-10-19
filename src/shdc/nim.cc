/*
    Generate metadata output in text or binary format
*/
#include "shdc.h"
#include "fmt/format.h"
#include "pystring.h"
#include <stdio.h>

namespace shdc {

using namespace util;

static std::string file_content;

#if defined(_MSC_VER)
#define L(str, ...) file_content.append(fmt::format(str, __VA_ARGS__))
#else
#define L(str, ...) file_content.append(fmt::format(str, ##__VA_ARGS__))
#endif

#define METADATAVER 1

static void write_programs(const args_t& args, const input_t& inp, const spirvcross_t& spirvcross) {
    for (const auto& item: inp.programs) {
        const program_t& prog = item.second;

        const spirvcross_source_t* vs_src = find_spirvcross_source_by_shader_name(prog.vs_name, inp, spirvcross);
        const spirvcross_source_t* fs_src = find_spirvcross_source_by_shader_name(prog.fs_name, inp, spirvcross);
        assert(vs_src && fs_src);
        L("program {}:\n", prog.name);
        L("  vertex {}, {}:\n", prog.vs_name, vs_src->refl.entry_point);
        const snippet_t& vs_snippet = inp.snippets[vs_src->snippet_index];
        for (const attr_t& attr: vs_src->refl.inputs) {
            if (attr.slot >= 0) {
                L("    attribute {}, {}, {}, {}\n", attr.name, attr.slot, attr.sem_name, attr.sem_index);
            }
        }
        for (const uniform_block_t& ub: vs_src->refl.uniform_blocks) {
            L("    uniform {}, {}\n", ub.name, ub.slot);
        }
        for (const image_t& img: vs_src->refl.images) {
            L("    image {}, {}, {}, {}\n", img.name, img.slot, img.type, img.base_type);
        }
        L("    discard\n");
        L("  fragment {}, {}:\n", prog.fs_name, fs_src->refl.entry_point);
        for (const uniform_block_t& ub: fs_src->refl.uniform_blocks) {
            L("    uniform {}, {}\n", ub.name, ub.slot);
        }
        for (const image_t& img: fs_src->refl.images) {
            L("    image {}, {}, {}, {}\n", img.name, img.slot, img.type, img.base_type);
        }
        L("    discard\n");
    }
}

static void write_uniform_blocks(const input_t& inp, const spirvcross_t& spirvcross, slang_t::type_t slang) {
    for (const uniform_block_t& ub: spirvcross.unique_uniform_blocks) {
        L("struct {}, {}:\n", ub.name, roundup(ub.size, 16));
        int cur_offset = 0;
        for (const uniform_t& uniform: ub.uniforms) {
            int next_offset = uniform.offset;
            if (next_offset > cur_offset) {
                L("  padding {}, {}\n", cur_offset, next_offset - cur_offset);
                cur_offset = next_offset;
            }
            if (inp.type_map.count(uniform_type_str(uniform.type)) > 0) {
                L("  field {}, {}, {}\n", uniform.name, inp.type_map.at(uniform_type_str(uniform.type)), uniform.array_count);
            }
            else {
                // default type names (float)
                int base_count = 1;
                switch (uniform.type) {
                    case uniform_t::FLOAT:   base_count = 1; break;
                    case uniform_t::FLOAT2:  base_count = 2; break;
                    case uniform_t::FLOAT3:  base_count = 3; break;
                    case uniform_t::FLOAT4:  base_count = 4; break;
                    case uniform_t::MAT4:    base_count = 16; break;
                    default:                 L("  #invalidfield\n"); break;
                }
                int count = uniform.array_count * base_count;
                L("  field {}, float32, {}\n", uniform.name, count);
            }
            cur_offset += uniform_type_size(uniform.type) * uniform.array_count;
        }
        const int round16 = roundup(cur_offset, 16);
        if (cur_offset != round16) {
            L("  padding {}, {}\n", cur_offset, round16-cur_offset);
        }
    }
}

static void write_common_decls(slang_t::type_t slang, const args_t& args, const input_t& inp, const spirvcross_t& spirvcross) {
    write_uniform_blocks(inp, spirvcross, slang);
}

template <typename T>
static void hexdump(T data) {
    std::string hex;
    int i = 0;
    for (auto ch : data) {
        hex += fmt::format("{:02x}", ch);
        if ((i & 15) == 15) {
            L("  put \"{}\"\n", hex);
            hex.clear();
        }
        i++;
    }
    if (!hex.empty()) {
        L("  put \"{}\"\n", hex);
    }
}

static void write_shader_sources_and_blobs(const input_t& inp,
                                           const spirvcross_t& spirvcross,
                                           const bytecode_t& bytecode,
                                           slang_t::type_t slang)
{
    for (int snippet_index = 0; snippet_index < (int)inp.snippets.size(); snippet_index++) {
        const snippet_t& snippet = inp.snippets[snippet_index];
        if ((snippet.type != snippet_t::VS) && (snippet.type != snippet_t::FS)) {
            continue;
        }
        int src_index = spirvcross.find_source_by_snippet_index(snippet_index);
        assert(src_index >= 0);
        const spirvcross_source_t& src = spirvcross.sources[src_index];
        int blob_index = bytecode.find_blob_by_snippet_index(snippet_index);
        const bytecode_blob_t* blob = 0;
        if (blob_index != -1) {
            blob = &bytecode.blobs[blob_index];
        }
        if (blob) {
            L("bytecode {}, {}:\n", snippet.name, slang_t::to_str(slang), blob->data.size());
            hexdump(blob->data);
        }
        else {
            L("source {}, {}:\n", snippet.name, slang_t::to_str(slang), src.source_code.length());
            hexdump(src.source_code);
        }
    }
}

errmsg_t nim_t::gen(const args_t& args, const input_t& inp,
                     const std::array<spirvcross_t,slang_t::NUM>& spirvcross,
                     const std::array<bytecode_t,slang_t::NUM>& bytecode)
{
    // first write everything into a string, and only when no errors occur,
    // dump this into a file (so we don't have half-written files lying around)
    file_content.clear();

    L("metadata {}\n", METADATAVER);
    L("version {}\n", args.gen_version);
    L("# generated by sokol-shdc (https://github.com/floooh/sokol-tools)\n");
    L("# cmdline: {}\n", args.cmdline);

    errmsg_t err;
    bool program_decls_written = false;
    bool common_decls_written = false;
    for (int i = 0; i < slang_t::NUM; i++) {
        slang_t::type_t slang = (slang_t::type_t) i;
        if (args.slang & slang_t::bit(slang)) {
            errmsg_t err = check_errors(inp, spirvcross[i], slang);
            if (err.valid) {
                return err;
            }
            if (!common_decls_written) {
                common_decls_written = true;
                write_common_decls(slang, args, inp, spirvcross[i]);
            }
            if (!program_decls_written) {
                program_decls_written = true;
                write_programs(args, inp, spirvcross[i]);
            }
            write_shader_sources_and_blobs(inp, spirvcross[i], bytecode[i], slang);
        }
    }

    // write result into output file
    if (util::is_special_filename(args.output)) {
        fwrite(file_content.c_str(), file_content.length(), 1, stdout);
    } else {
        FILE* f = fopen(args.output.c_str(), "w");
        if (!f) {
            return errmsg_t::error(inp.base_path, 0, fmt::format("failed to open output file '{}'", args.output));
        }
        fwrite(file_content.c_str(), file_content.length(), 1, f);
        fclose(f);
    }
    return errmsg_t();
}

} // namespace shdc
