// grinfo: prints a human-readable summary of a .grm capture file.
//
// Usage: grinfo <file.grm> [more.grm ...]
#include "ghost/attribute.h"
#include "ghost/capture_reader.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace {

void print_file(const char* path) {
    ghost::MeshFile mesh;
    std::string err;
    if (!mesh.load(path, &err)) {
        std::fprintf(stderr, "grinfo: %s\n", err.c_str());
        return;
    }

    const auto& fh = mesh.file_header();
    const auto& mh = mesh.header();

    std::printf("== %s ==\n", path);
    std::printf("  source app     : %s\n", fh.source_app);
    std::printf("  frame index    : %u\n", fh.frame_index);
    std::printf("  timestamp      : %" PRIu64 "\n", fh.timestamp);
    std::printf("  name           : %s\n", mh.name);
    std::printf("  vertices       : %u (stride %u bytes)\n", mh.vertex_count, mh.vertex_stride);
    if (mh.index_count)
        std::printf("  indices        : %u (%s)\n", mh.index_count,
                    mh.index_format == static_cast<uint32_t>(ghost::fmt::IndexFormat::UInt16)
                        ? "16-bit" : "32-bit");
    else
        std::printf("  indices        : none (non-indexed)\n");
    std::printf("  triangles      : %u\n", mesh.triangle_count());
    std::printf("  topology       : %s\n",
                ghost::topology_name(static_cast<ghost::fmt::PrimitiveTopology>(mh.topology)));
    std::printf("  draw params    : start_index=%u base_vertex=%d\n", mh.start_index,
                mh.base_vertex);

    std::printf("  attributes (%u):\n", mh.attribute_count);
    for (const auto& a : mesh.attributes())
        std::printf("    %-12s idx=%u  %-9s offset=%u\n",
                    ghost::semantic_name(static_cast<ghost::fmt::Semantic>(a.semantic)),
                    a.semantic_index,
                    ghost::attribute_format_name(static_cast<ghost::fmt::AttributeFormat>(a.format)),
                    a.offset);

    if (mh.flags & ghost::fmt::kFlagAabbValid)
        std::printf("  aabb           : min(%.3f %.3f %.3f) max(%.3f %.3f %.3f)\n",
                    mh.aabb_min[0], mh.aabb_min[1], mh.aabb_min[2],
                    mh.aabb_max[0], mh.aabb_max[1], mh.aabb_max[2]);

    if (mh.texture_count) {
        std::printf("  textures (%u):\n", mh.texture_count);
        for (const auto& t : mesh.textures())
            std::printf("    slot=%u usage=%u hash=%016" PRIx64 " file=%s\n", t.slot,
                        t.usage, t.content_hash, t.file);
    }
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: grinfo <file.grm> [more.grm ...]\n");
        return 1;
    }
    for (int i = 1; i < argc; ++i)
        print_file(argv[i]);
    return 0;
}
