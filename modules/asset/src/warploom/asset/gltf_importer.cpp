//! @file gltf_importer.cpp
//! @brief Deterministic glTF 2.0 static-mesh ingestion (see gltf_importer.hpp).

#include "warploom/asset/gltf_importer.hpp"
#include "warploom/asset/image_decode.hpp"
#include "gltf_json.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>

namespace warploom::asset {

using namespace gltf_detail;


::warploom::core::Result<GltfMeshImport> import_gltf_mesh(
    const char* json_bytes, std::size_t json_len, const std::uint8_t* bin_bytes,
    std::size_t bin_len, std::size_t mesh_index, std::string* error_detail,
    const ExternalFileLoader* loader) {
  std::string error;
  GltfMeshImport import;

  auto finish = [&](bool ok) -> ::warploom::core::Result<GltfMeshImport> {
    if (!ok) {
      if (error_detail != nullptr) *error_detail = error;
      return ::warploom::core::Result<GltfMeshImport>::error(
          ::warploom::core::RuntimeError::malformed_asset);
    }
    return ::warploom::core::Result<GltfMeshImport>::ok(std::move(import));
  };

  if (json_bytes == nullptr && json_len != 0U) {
    return finish(fail_asset(error, "json_bytes is null"));
  }
  if (bin_bytes == nullptr && bin_len != 0U) {
    return finish(fail_asset(error, "bin_bytes is null"));
  }
  if (json_len == 0U) {
    return finish(fail_asset(error, "empty glTF document"));
  }

  // Auto-detect the container: raw JSON or a GLB 2.0 container.
  std::string container_json;
  DocumentPrologue prologue;
  if (!parse_gltf_document_prologue(json_bytes, json_len, bin_bytes, bin_len,
                                    container_json, prologue, error)) {
    return finish(false);
  }

  JsonParser parser(prologue.json_bytes, prologue.json_len, error);
  Json document;
  if (!parser.parse(document) || document.kind != Json::Kind::Object) {
    return finish(false);
  }

  // asset.version is required and must be glTF 2.x.
  const Json* asset = find_member(document, "asset");
  if (asset == nullptr) {
    return finish(fail_asset(error, "document is missing asset"));
  }
  const Json* version = find_member(*asset, "version");
  if (version == nullptr || version->kind != Json::Kind::String ||
      version->string.size() < 3U || version->string[0] != '2' ||
      version->string[1] != '.') {
    return finish(fail_asset(error, "asset.version must be a glTF 2.x string"));
  }

  std::vector<BufferSource> sources;
  std::vector<View> views;
  std::vector<AccessorInfo> accessors;
  // Owns embedded data: buffers; `sources` points into it for the whole
  // import scope, so it must live as long as the decode below.
  std::vector<std::vector<std::uint8_t>> embedded_storage;
  if (!parse_gltf_buffers_views_accessors(document, prologue.bin_bytes,
                                          prologue.bin_len, sources, views,
                                          accessors, embedded_storage, error,
                                          prologue.allow_uriless_buffer0)) {
    return finish(false);
  }

  const Json* meshes = find_member(document, "meshes");
  const Json* materials = find_member(document, "materials");
  if (meshes == nullptr || meshes->kind != Json::Kind::Array ||
      meshes->items.empty()) {
    return finish(fail_asset(error, "document is missing meshes array"));
  }
  if (mesh_index >= meshes->items.size()) {
    return finish(fail_asset(
        error, "mesh index " + std::to_string(mesh_index) +
                   " is out of range (document declares " +
                   std::to_string(meshes->items.size()) + " meshes)"));
  }

  // ------------------------------------------ samplers / textures / images
  // Sampler enums and structural references are validated eagerly; image
  // payloads decode lazily, only when the imported material actually
  // references them (so unrelated images never fail the import).
  struct SamplerRef {
    bool exists{false};
    std::uint32_t mag_filter{0};
    std::uint32_t min_filter{0};
    std::uint32_t wrap_s{10497};
    std::uint32_t wrap_t{10497};
  };
  struct TextureRef {
    std::size_t image{0};
    bool has_sampler{false};
    std::size_t sampler_index{0};
  };
  struct ImageRef {
    enum class Kind { DataUri, View, External };
    Kind kind{Kind::External};
    std::size_t storage_index{0};  //!< DataUri -> image_raw_storage
    std::size_t view_index{0};     //!< View
    //! External file URI as written in the document (resolved lazily through
    //! the optional ExternalFileLoader, only when a material references it).
    std::string uri{};
  };
  std::vector<SamplerRef> sampler_refs;
  std::vector<TextureRef> texture_refs;
  std::vector<ImageRef> image_refs;
  std::vector<std::vector<std::uint8_t>> image_raw_storage;

  const auto is_sampler_filter = [](std::int64_t value) {
    return value == 9728 || value == 9729 || value == 9984 || value == 9985 ||
           value == 9986 || value == 9987;
  };
  const auto is_sampler_wrap = [](std::int64_t value) {
    return value == 33071 || value == 33648 || value == 10497;
  };

  const Json* samplers_json = find_member(document, "samplers");
  if (samplers_json != nullptr) {
    if (samplers_json->kind != Json::Kind::Array) {
      return finish(fail_asset(error, "samplers must be an array"));
    }
    sampler_refs.reserve(samplers_json->items.size());
    for (std::size_t i = 0; i < samplers_json->items.size(); ++i) {
      const Json& sampler_json = samplers_json->items[i];
      const std::string sampler_context =
          "samplers[" + std::to_string(i) + "]";
      if (sampler_json.kind != Json::Kind::Object) {
        return finish(fail_asset(error, sampler_context + " must be an object"));
      }
      SamplerRef ref;
      ref.exists = true;
      const auto read_optional_enum =
          [&](const char* key, std::uint32_t default_value,
              bool (*valid)(std::int64_t), std::uint32_t& out,
              const std::string& kind_name) -> bool {
        out = default_value;
        const Json* member = find_member(sampler_json, key);
        if (member == nullptr) return true;
        std::int64_t value = 0;
        if (!as_int(*member, value, error,
                    sampler_context + "." + key)) {
          return false;
        }
        if (!valid(value)) {
          return fail_asset(
              error, sampler_context + "." + key + " " +
                         std::to_string(value) +
                         " is not a supported " + kind_name + " enum");
        }
        out = static_cast<std::uint32_t>(value);
        return true;
      };
      if (!read_optional_enum("magFilter", 0, is_sampler_filter,
                              ref.mag_filter, "filter") ||
          !read_optional_enum("minFilter", 0, is_sampler_filter,
                              ref.min_filter, "filter") ||
          !read_optional_enum("wrapS", 10497, is_sampler_wrap,
                              ref.wrap_s, "wrap") ||
          !read_optional_enum("wrapT", 10497, is_sampler_wrap,
                              ref.wrap_t, "wrap")) {
        return finish(false);
      }
      sampler_refs.push_back(ref);
    }
  }

  const Json* images_json = find_member(document, "images");
  if (images_json != nullptr) {
    if (images_json->kind != Json::Kind::Array) {
      return finish(fail_asset(error, "images must be an array"));
    }
    image_refs.reserve(images_json->items.size());
    image_raw_storage.reserve(images_json->items.size());
    for (std::size_t i = 0; i < images_json->items.size(); ++i) {
      const Json& image_json = images_json->items[i];
      const std::string image_context = "images[" + std::to_string(i) + "]";
      if (image_json.kind != Json::Kind::Object) {
        return finish(fail_asset(error, image_context + " must be an object"));
      }
      const Json* uri_member = find_member(image_json, "uri");
      const Json* view_member = find_member(image_json, "bufferView");
      if (uri_member != nullptr && view_member != nullptr) {
        return finish(fail_asset(
            error, image_context + " must not define both uri and bufferView"));
      }
      if (uri_member == nullptr && view_member == nullptr) {
        return finish(fail_asset(
            error, image_context + " must define a uri or a bufferView"));
      }
      if (uri_member != nullptr) {
        if (uri_member->kind != Json::Kind::String) {
          return finish(fail_asset(error, image_context + ".uri must be a string"));
        }
        const std::string_view uri = uri_member->string;
        if (uri.size() < 5U || uri.substr(0, 5U) != "data:") {
          // External image file. Resolution is lazy: the bytes are only
          // fetched through the optional loader when an imported material
          // actually references this image, so documents whose materials
          // never touch it import without any filesystem access.
          ImageRef ref;
          ref.kind = ImageRef::Kind::External;
          ref.uri.assign(uri);
          image_refs.push_back(ref);
          continue;
        }
        const std::size_t comma = uri.find(',');
        if (comma == std::string_view::npos) {
          return finish(fail_asset(error, image_context +
                                            " has a malformed data: URI"));
        }
        const std::string_view meta = uri.substr(5, comma - 5);
        if (meta.find(";base64") == std::string_view::npos) {
          return finish(fail_asset(
              error, image_context +
                         ": only base64 data: URIs are supported"));
        }
        image_raw_storage.emplace_back();
        if (!decode_base64(uri.substr(comma + 1),
                           image_raw_storage.back(), error)) {
          return finish(false);
        }
        ImageRef ref;
        ref.kind = ImageRef::Kind::DataUri;
        ref.storage_index = image_raw_storage.size() - 1U;
        image_refs.push_back(ref);
        continue;
      }
      std::int64_t view_index = 0;
      if (!as_int(*view_member, view_index, error,
                  image_context + ".bufferView")) {
        return finish(false);
      }
      if (view_index < 0 ||
          static_cast<std::size_t>(view_index) >= views.size()) {
        return finish(fail_asset(
            error, image_context + ".bufferView " +
                       std::to_string(view_index) +
                       " is out of range (declared " +
                       std::to_string(views.size()) + " views)"));
      }
      if (views[static_cast<std::size_t>(view_index)].has_stride) {
        return finish(fail_asset(
            error, image_context +
                       " references a bufferView with byteStride; image "
                       "bufferViews must be tightly packed"));
      }
      ImageRef ref;
      ref.kind = ImageRef::Kind::View;
      ref.view_index = static_cast<std::size_t>(view_index);
      image_refs.push_back(ref);
    }
  }

  const Json* textures_json = find_member(document, "textures");
  if (textures_json != nullptr) {
    if (textures_json->kind != Json::Kind::Array) {
      return finish(fail_asset(error, "textures must be an array"));
    }
    texture_refs.reserve(textures_json->items.size());
    for (std::size_t i = 0; i < textures_json->items.size(); ++i) {
      const Json& texture_json = textures_json->items[i];
      const std::string texture_context =
          "textures[" + std::to_string(i) + "]";
      if (texture_json.kind != Json::Kind::Object) {
        return finish(fail_asset(error, texture_context + " must be an object"));
      }
      TextureRef ref;
      const Json* source_member = find_member(texture_json, "source");
      if (source_member == nullptr) {
        return finish(fail_asset(
            error, texture_context + " is missing source"));
      }
      std::int64_t source_index = 0;
      if (!as_int(*source_member, source_index, error,
                  texture_context + ".source")) {
        return finish(false);
      }
      if (source_index < 0 ||
          static_cast<std::size_t>(source_index) >= image_refs.size()) {
        return finish(fail_asset(
            error, texture_context + ".source " +
                       std::to_string(source_index) +
                       " is out of range (declared " +
                       std::to_string(image_refs.size()) + " images)"));
      }
      ref.image = static_cast<std::size_t>(source_index);
      const Json* sampler_member = find_member(texture_json, "sampler");
      if (sampler_member != nullptr) {
        std::int64_t sampler_index = 0;
        if (!as_int(*sampler_member, sampler_index, error,
                    texture_context + ".sampler")) {
          return finish(false);
        }
        if (sampler_index < 0 ||
            static_cast<std::size_t>(sampler_index) >= sampler_refs.size()) {
          return finish(fail_asset(
              error, texture_context + ".sampler " +
                         std::to_string(sampler_index) +
                         " is out of range (declared " +
                         std::to_string(sampler_refs.size()) + " samplers)"));
        }
        ref.has_sampler = true;
        ref.sampler_index = static_cast<std::size_t>(sampler_index);
      }
      texture_refs.push_back(ref);
    }
  }

  // ================================================================ meshes
  const Json& mesh_json = meshes->items[mesh_index];
  if (const Json* name = find_member(mesh_json, "name");
      name != nullptr && name->kind == Json::Kind::String) {
    import.name = name->string;
  }
  const Json* primitives = find_member(mesh_json, "primitives");
  if (primitives == nullptr || primitives->kind != Json::Kind::Array ||
      primitives->items.empty()) {
    return finish(fail_asset(
        error, "meshes[" + std::to_string(mesh_index) +
                   "] must declare at least one primitive"));
  }

  // Mesh-level base color / albedo alias the first primitive that binds a
  // material (see the per-primitive records below).
  bool any_binder = false;
  std::size_t first_binder = 0;

  // The accessor region check needs element size of the accessor that
  // references a view; we decode each accessor exactly once per primitive so
  // validation is repeated — cheap and local.
  auto resolve_accessor = [&](std::size_t accessor_index,
                              const std::string& context)
      -> const AccessorInfo* {
    if (accessor_index >= accessors.size()) {
      (void)fail_asset(error, context + " references accessor " +
                                  std::to_string(accessor_index) +
                                  " which is out of range (declared " +
                                  std::to_string(accessors.size()) + ")");
      return nullptr;
    }
    const AccessorInfo& accessor = accessors[accessor_index];
    if (!accessor.has_view) {
      (void)fail_asset(error, context + " references an accessor without a "
                                     "bufferView");
      return nullptr;
    }
    return &accessor;
  };

  //! Validate that one accessor's region lies fully inside its bufferView,
  //! honoring byteStride. Returns element size in bytes.
  auto accessor_element_size = [&](const AccessorInfo& accessor,
                                   const std::string& context)
      -> bool {
    if (accessor.components >
        std::numeric_limits<std::size_t>::max() / accessor.component_size) {
      return fail_asset(error, context + " element size overflows");
    }
    const std::size_t element_size =
        accessor.components * accessor.component_size;
    const View& view = views[accessor.view_index];
    if (view.has_stride && view.byte_stride < element_size) {
      return fail_asset(error, context + " element (" +
                                   std::to_string(element_size) +
                                   " bytes) exceeds bufferView byteStride (" +
                                   std::to_string(view.byte_stride) + ")");
    }
    const std::size_t stride =
        view.has_stride ? view.byte_stride : element_size;
    // Total bytes spanned by `count` elements starting at byte_offset.
    if (accessor.count == 0U) {
      if (accessor.byte_offset > view.byte_length) {
        return fail_asset(error, context + " offset exceeds bufferView");
      }
      return true;
    }
    if (stride != 0U &&
        accessor.count > (std::numeric_limits<std::size_t>::max() - element_size) / stride) {
      return fail_asset(error, context + " region size overflows");
    }
    const std::size_t span =
        (accessor.count - 1U) * stride + element_size;
    if (accessor.byte_offset > view.byte_length ||
        span > view.byte_length - accessor.byte_offset) {
      return fail_asset(error, context + " region extends past bufferView "
                                         "byteLength " +
                                     std::to_string(view.byte_length));
    }
    return true;
  };

  //! Extract accessor min/max. Both must be present and hold exactly
  //! `components` numbers; otherwise the box is reported as absent (a
  //! primitive that declares only one side is malformed and rejected).
  auto parse_accessor_bounds = [&](std::size_t accessor_index,
                                   std::size_t components, float* mn,
                                   float* mx, bool& present,
                                   const std::string& context) -> bool {
    present = false;
    const Json* accessors_json = find_member(document, "accessors");
    if (accessors_json == nullptr ||
        accessor_index >= accessors_json->items.size()) {
      return fail_asset(error, context + " accessor is out of range");
    }
    const Json& accessor_json = accessors_json->items[accessor_index];
    const Json* min_member = find_member(accessor_json, "min");
    const Json* max_member = find_member(accessor_json, "max");
    if (min_member == nullptr && max_member == nullptr) return true;
    if (min_member == nullptr || max_member == nullptr) {
      return fail_asset(error, context + " must declare both min and max");
    }
    auto read_bound = [&](const Json& member, float* out,
                          const char* name) -> bool {
      if (member.kind != Json::Kind::Array ||
          member.items.size() != components) {
        return fail_asset(error, context + " accessor " + name +
                                       " must be an array of " +
                                       std::to_string(components) +
                                       " numbers");
      }
      for (std::size_t c = 0; c < components; ++c) {
        double value = 0.0;
        if (!as_real(member.items[c], value, error, context + "." + name)) {
          return false;
        }
        out[c] = static_cast<float>(value);
      }
      return true;
    };
    if (!read_bound(*min_member, mn, "min")) return false;
    if (!read_bound(*max_member, mx, "max")) return false;
    present = true;
    return true;
  };

  // Mesh-level box union across primitives. Frustum culling uses it only when
  // every primitive declares a valid POSITION box (glTF spec conformance);
  // otherwise bounds stay invalid and extraction never culls the mesh.
  bool all_primitive_bounds = true;
  bool any_primitive_bounds = false;
  float box_min[3]{0.0f, 0.0f, 0.0f};
  float box_max[3]{0.0f, 0.0f, 0.0f};

  //! Decode a material's baseColorTexture into `out`. Referenced payloads are
  //! PNG or JPEG (data: URI or tightly packed bufferView) and decode lazily;
  //! failures
  //! carry the primitive context in the diagnostic. A texture shared by
  //! several primitives of this mesh decodes exactly once.
  //! glTF texture index -> image index in import.images for shared decodes.
  std::vector<std::pair<std::size_t, std::size_t>> albedo_image_by_texture{};
  auto parse_material_albedo = [&](const Json& base_tex,
                                   const std::string& context,
                                   GltfMaterialTexture& out) -> bool {
    // glTF 2.0 defines baseColorTexture as an sRGB-encoded colour texture;
    // consumers sample it through an sRGB image format so the hardware
    // linearises before lighting (see GltfMaterialTexture::encoded_srgb).
    out = GltfMaterialTexture{};
    out.encoded_srgb = true;
    if (base_tex.kind != Json::Kind::Object) {
      return fail_asset(error,
                        context + " baseColorTexture must be an object");
    }
    if (const Json* tex_coord = find_member(base_tex, "texCoord");
        tex_coord != nullptr) {
      std::int64_t set = 0;
      if (!as_int(*tex_coord, set, error,
                  context + ".baseColorTexture.texCoord")) {
        return false;
      }
      if (set != 0) {
        return fail_asset(error,
                          context + ".baseColorTexture.texCoord " +
                              std::to_string(set) +
                              " is not supported (only TEXCOORD_0 is imported)");
      }
    }
    const Json* index_member = find_member(base_tex, "index");
    if (index_member == nullptr) {
      return fail_asset(error,
                        context + " baseColorTexture is missing index");
    }
    std::int64_t texture_index = 0;
    if (!as_int(*index_member, texture_index, error,
                context + ".baseColorTexture.index")) {
      return false;
    }
    const std::size_t texture_index_u = static_cast<std::size_t>(texture_index);
    if (texture_index < 0 || texture_index_u >= texture_refs.size()) {
      return fail_asset(error,
                        context + ".baseColorTexture.index " +
                            std::to_string(texture_index) +
                            " is out of range (declared " +
                            std::to_string(texture_refs.size()) +
                            " textures)");
    }
    const TextureRef& texture = texture_refs[texture_index_u];
    for (const auto& shared : albedo_image_by_texture) {
      if (shared.first == texture_index_u) {
        out.image_index = shared.second;
        if (texture.has_sampler) {
          const SamplerRef& sampler = sampler_refs[texture.sampler_index];
          out.mag_filter = sampler.mag_filter;
          out.min_filter = sampler.min_filter;
          out.wrap_s = sampler.wrap_s;
          out.wrap_t = sampler.wrap_t;
        }
        out.present = true;
        return true;
      }
    }
    const ImageRef& image_ref = image_refs[texture.image];
    const std::uint8_t* encoded_ptr = nullptr;
    std::size_t encoded_size = 0;
    std::string image_error;
    std::vector<std::uint8_t> loaded_bytes;
    switch (image_ref.kind) {
      case ImageRef::Kind::DataUri:
        encoded_ptr = image_raw_storage[image_ref.storage_index].data();
        encoded_size = image_raw_storage[image_ref.storage_index].size();
        break;
      case ImageRef::Kind::View: {
        const View& view = views[image_ref.view_index];
        encoded_ptr = sources[view.buffer_index].data + view.byte_offset;
        encoded_size = view.byte_length;
        break;
      }
      case ImageRef::Kind::External: {
        if (loader == nullptr) {
          return fail_asset(
              error, context + " baseColorTexture image " +
                         std::to_string(texture.image) +
                         " references external file '" + image_ref.uri +
                         "' but no ExternalFileLoader was provided; embed it "
                         "as a data: URI, store it in a bufferView, or pass a "
                         "loader that resolves '" +
                         image_ref.uri + "'");
        }
        if (!(*loader)(image_ref.uri, image_error, loaded_bytes)) {
          return fail_asset(
              error, context + " baseColorTexture image " +
                         std::to_string(texture.image) +
                         " could not load external file '" + image_ref.uri +
                         "': " +
                         (image_error.empty() ? "loader returned failure"
                                              : image_error));
        }
        if (loaded_bytes.empty()) {
          return fail_asset(
              error, context + " baseColorTexture image " +
                         std::to_string(texture.image) +
                         " external file '" + image_ref.uri + "' is empty");
        }
        encoded_ptr = loaded_bytes.data();
        encoded_size = loaded_bytes.size();
        break;
      }
    }
    auto decoded = decode_image(encoded_ptr, encoded_size, &image_error);
    if (!decoded.is_ok()) {
      return fail_asset(
          error, context + " baseColorTexture image " +
                     std::to_string(texture.image) +
                     " is not a decodable PNG or JPEG image: " +
                     image_error);
    }
    GltfImage image;
    image.width = decoded.value().width;
    image.height = decoded.value().height;
    image.rgba = std::move(decoded.value().rgba);
    out.image_index = import.images.size();
    albedo_image_by_texture.push_back(
        {texture_index_u, out.image_index});
    import.images.push_back(std::move(image));
    if (texture.has_sampler) {
      const SamplerRef& sampler = sampler_refs[texture.sampler_index];
      out.mag_filter = sampler.mag_filter;
      out.min_filter = sampler.min_filter;
      out.wrap_s = sampler.wrap_s;
      out.wrap_t = sampler.wrap_t;
    }  // No sampler: keep glTF defaults (filters 0 = caller choice, REPEAT).
    out.present = true;
    return true;
  };

  //! Read one element of a float accessor into `destination[components]`.
  //! All float accessors are vec3/vec4 float32 in the supported subset and
  //! every region was validated against its buffer earlier.
  auto read_float_element = [&](const AccessorInfo& accessor,
                                std::size_t element_index,
                                float* destination) {
    const View& view = views[accessor.view_index];
    const BufferSource& buffer = sources[view.buffer_index];
    const std::size_t element_size = accessor.components * accessor.component_size;
    const std::size_t stride =
        view.has_stride ? view.byte_stride : element_size;
    const std::size_t absolute =
        view.byte_offset + accessor.byte_offset + element_index * stride;
    for (std::size_t c = 0; c < accessor.components; ++c) {
      float value = 0.0f;
      std::memcpy(&value, buffer.data + absolute + c * accessor.component_size,
                  sizeof(value));
      destination[c] = value;
    }
  };

  for (std::size_t p = 0; p < primitives->items.size(); ++p) {
    const Json& primitive = primitives->items[p];
    const std::string primitive_context =
        "meshes[" + std::to_string(mesh_index) + "].primitives[" +
        std::to_string(p) + "]";

    // Mode must be TRIANGLES (default when absent).
    if (const Json* mode = find_member(primitive, "mode"); mode != nullptr) {
      std::int64_t mode_value = 0;
      if (!as_int(*mode, mode_value, error, primitive_context + ".mode")) {
        return finish(false);
      }
      if (mode_value != kPrimitiveTriangles) {
        return finish(fail_asset(
            error, primitive_context + ".mode " + std::to_string(mode_value) +
                       " is not supported (only TRIANGLES=4)"));
      }
    }

    const Json* attributes = find_member(primitive, "attributes");
    if (attributes == nullptr || attributes->kind != Json::Kind::Object) {
      return finish(fail_asset(error, primitive_context +
                                          " is missing attributes"));
    }
    const Json* position_ref = find_member(*attributes, "POSITION");
    if (position_ref == nullptr) {
      return finish(fail_asset(error, primitive_context +
                                          " has no POSITION attribute"));
    }
    std::int64_t position_index = 0;
    if (!as_int(*position_ref, position_index, error,
                primitive_context + ".attributes.POSITION")) {
      return finish(false);
    }
    if (position_index < 0) {
      return finish(fail_asset(error, primitive_context +
                                          ".attributes.POSITION is negative"));
    }
    const AccessorInfo* position = resolve_accessor(
        static_cast<std::size_t>(position_index),
        primitive_context + ".attributes.POSITION");
    if (position == nullptr) return finish(false);
    if (position->component_type != kComponentFloat ||
        position->components != 3U) {
      return finish(fail_asset(
          error, primitive_context +
                     ".attributes.POSITION must be a VEC3 float32 accessor"));
    }
    if (!accessor_element_size(*position,
                               primitive_context + ".attributes.POSITION")) {
      return finish(false);
    }

    // Per-primitive box from the POSITION accessor min/max; merge into the
    // mesh union after decoding.
    float primitive_min[3]{0.0f, 0.0f, 0.0f};
    float primitive_max[3]{0.0f, 0.0f, 0.0f};
    bool primitive_has_bounds = false;
    if (!parse_accessor_bounds(
            static_cast<std::size_t>(position_index), position->components,
            primitive_min, primitive_max, primitive_has_bounds,
            primitive_context + ".attributes.POSITION")) {
      return finish(false);
    }
    if (!primitive_has_bounds) {
      all_primitive_bounds = false;
    } else if (!any_primitive_bounds) {
      // First box seeds the union.
      std::memcpy(box_min, primitive_min, sizeof(box_min));
      std::memcpy(box_max, primitive_max, sizeof(box_max));
      any_primitive_bounds = true;
    } else {
      for (std::size_t c = 0; c < 3U; ++c) {
        box_min[c] = box_min[c] < primitive_min[c] ? box_min[c] : primitive_min[c];
        box_max[c] = box_max[c] > primitive_max[c] ? box_max[c] : primitive_max[c];
      }
    }

    // Optional color: VEC3/VEC4 float32.
    const AccessorInfo* color = nullptr;
    if (const Json* color_ref = find_member(*attributes, "COLOR_0");
        color_ref != nullptr) {
      std::int64_t color_index = 0;
      if (!as_int(*color_ref, color_index, error,
                  primitive_context + ".attributes.COLOR_0")) {
        return finish(false);
      }
      if (color_index < 0) {
        return finish(fail_asset(error, primitive_context +
                                            ".attributes.COLOR_0 is negative"));
      }
      color = resolve_accessor(
          static_cast<std::size_t>(color_index),
          primitive_context + ".attributes.COLOR_0");
      if (color == nullptr) return finish(false);
      if (color->component_type != kComponentFloat ||
          (color->components != 3U && color->components != 4U)) {
        return finish(fail_asset(
            error, primitive_context +
                       ".attributes.COLOR_0 must be a VEC3/VEC4 float32 "
                       "accessor"));
      }
      if (!accessor_element_size(*color,
                                 primitive_context + ".attributes.COLOR_0")) {
        return finish(false);
      }
      if (color->count != position->count) {
        return finish(fail_asset(
            error, primitive_context + ".attributes.COLOR_0 count " +
                       std::to_string(color->count) +
                       " does not match POSITION count " +
                       std::to_string(position->count)));
      }
    }

    // Optional normal: VEC3 float32.
    const AccessorInfo* normal = nullptr;
    if (const Json* normal_ref = find_member(*attributes, "NORMAL");
        normal_ref != nullptr) {
      std::int64_t normal_index = 0;
      if (!as_int(*normal_ref, normal_index, error,
                  primitive_context + ".attributes.NORMAL")) {
        return finish(false);
      }
      if (normal_index < 0) {
        return finish(
            fail_asset(error, primitive_context +
                                  ".attributes.NORMAL is negative"));
      }
      normal = resolve_accessor(static_cast<std::size_t>(normal_index),
                                primitive_context + ".attributes.NORMAL");
      if (normal == nullptr) return finish(false);
      if (normal->component_type != kComponentFloat ||
          normal->components != 3U) {
        return finish(fail_asset(
            error, primitive_context +
                       ".attributes.NORMAL must be a VEC3 float32 accessor"));
      }
      if (!accessor_element_size(*normal,
                                 primitive_context + ".attributes.NORMAL")) {
        return finish(false);
      }
      if (normal->count != position->count) {
        return finish(fail_asset(
            error, primitive_context + ".attributes.NORMAL count " +
                       std::to_string(normal->count) +
                       " does not match POSITION count " +
                       std::to_string(position->count)));
      }
    }

    // Optional texcoord: VEC2 float32.
    const AccessorInfo* uv = nullptr;
    if (const Json* uv_ref = find_member(*attributes, "TEXCOORD_0");
        uv_ref != nullptr) {
      std::int64_t uv_index = 0;
      if (!as_int(*uv_ref, uv_index, error,
                  primitive_context + ".attributes.TEXCOORD_0")) {
        return finish(false);
      }
      if (uv_index < 0) {
        return finish(
            fail_asset(error, primitive_context +
                                  ".attributes.TEXCOORD_0 is negative"));
      }
      uv = resolve_accessor(static_cast<std::size_t>(uv_index),
                            primitive_context + ".attributes.TEXCOORD_0");
      if (uv == nullptr) return finish(false);
      if (uv->component_type != kComponentFloat || uv->components != 2U) {
        return finish(fail_asset(
            error, primitive_context +
                       ".attributes.TEXCOORD_0 must be a VEC2 float32 "
                       "accessor"));
      }
      if (!accessor_element_size(*uv,
                                 primitive_context + ".attributes.TEXCOORD_0")) {
        return finish(false);
      }
      if (uv->count != position->count) {
        return finish(fail_asset(
            error, primitive_context + ".attributes.TEXCOORD_0 count " +
                       std::to_string(uv->count) +
                       " does not match POSITION count " +
                       std::to_string(position->count)));
      }
    }

    // Indices: optional SCALAR uint16/uint32.
    const AccessorInfo* indices = nullptr;
    bool non_indexed = false;
    if (const Json* indices_ref = find_member(primitive, "indices");
        indices_ref != nullptr) {
      std::int64_t indices_index = 0;
      if (!as_int(*indices_ref, indices_index, error,
                  primitive_context + ".indices")) {
        return finish(false);
      }
      if (indices_index < 0) {
        return finish(
            fail_asset(error, primitive_context + ".indices is negative"));
      }
      indices = resolve_accessor(static_cast<std::size_t>(indices_index),
                                 primitive_context + ".indices");
      if (indices == nullptr) return finish(false);
      if (indices->component_type != kComponentUShort &&
          indices->component_type != kComponentUInt) {
        return finish(fail_asset(
            error, primitive_context +
                       ".indices must be a SCALAR uint16/uint32 accessor"));
      }
      if (indices->components != 1U) {
        return finish(fail_asset(
            error, primitive_context + ".indices must be a SCALAR accessor"));
      }
      if (!accessor_element_size(*indices, primitive_context + ".indices")) {
        return finish(false);
      }
      const View& index_view = views[indices->view_index];
      if (index_view.has_stride) {
        return finish(fail_asset(
            error, primitive_context +
                       ".indices bufferView must not define byteStride"));
      }
    } else {
      non_indexed = true;
    }

    if (non_indexed && position->count % 3U != 0U) {
      return finish(fail_asset(
          error, primitive_context +
                     " is not indexed but has " +
                     std::to_string(position->count) +
                     " vertices (not a multiple of 3 for TRIANGLES)"));
    }
    if (indices != nullptr && indices->count % 3U != 0U) {
      return finish(fail_asset(
          error, primitive_context + " has " +
                     std::to_string(indices->count) +
                     " indices (not a multiple of 3 for TRIANGLES)"));
    }

    // First vertex this primitive will occupy in the merged stream.
    const std::size_t base_vertex = import.vertices.size() / kSceneVertexFloats;

    // ---------------------------------------------------- material binding
    // Every primitive records its own material (base color factor + optional
    // albedo texture). The mesh-level convenience fields alias the first
    // primitive that binds a material, preserving the single-material import
    // contract; multi-material meshes keep every binding in `primitives`.
    GltfPrimitiveMaterial prim;
    prim.vertex_offset = base_vertex;
    prim.vertex_count = position->count;
    prim.index_offset = import.indices.size();
    prim.index_count = indices != nullptr ? indices->count : position->count;
    if (const Json* material_ref = find_member(primitive, "material");
        material_ref != nullptr) {
      std::int64_t material_index = 0;
      if (!as_int(*material_ref, material_index, error,
                  primitive_context + ".material")) {
        return finish(false);
      }
      if (material_index < 0 || materials == nullptr ||
          static_cast<std::size_t>(material_index) >= materials->items.size()) {
        return finish(fail_asset(
            error, primitive_context + ".material " +
                       std::to_string(material_index) + " is out of range"));
      }
      const Json& material = materials->items[static_cast<std::size_t>(
          material_index)];
      if (const Json* name_member = find_member(material, "name");
          name_member != nullptr && name_member->kind == Json::Kind::String) {
        prim.material_name = name_member->string;
      }
      const Json* pbr = find_member(material, "pbrMetallicRoughness");
      const Json* factor =
          pbr != nullptr ? find_member(*pbr, "baseColorFactor") : nullptr;
      if (factor != nullptr) {
        if (factor->kind != Json::Kind::Array || factor->items.size() != 4U) {
          return finish(fail_asset(
              error, primitive_context +
                         ".material baseColorFactor must be [r,g,b,a]"));
        }
        for (std::size_t c = 0; c < 4U; ++c) {
          double component = 0.0;
          if (!as_real(factor->items[c], component, error,
                       primitive_context + ".material.baseColorFactor")) {
            return finish(false);
          }
          prim.base_color[c] = static_cast<float>(component);
        }
      }
      // Optional albedo texture of the same material, read whenever the
      // material binds one (a texture import does not depend on the material
      // declaring a factor).
      const Json* base_tex =
          pbr != nullptr ? find_member(*pbr, "baseColorTexture") : nullptr;
      if (base_tex != nullptr) {
        if (!parse_material_albedo(*base_tex,
                                   primitive_context + ".material",
                                   prim.albedo)) {
          return finish(false);
        }
      }
      if (!any_binder) {
        any_binder = true;
        first_binder = p;
      }
    }
    import.primitives.push_back(std::move(prim));

    // ------------------------------------------------------ decode vertices
    import.vertices.reserve(import.vertices.size() +
                            position->count * kSceneVertexFloats);
    for (std::size_t v = 0; v < position->count; ++v) {
      float element[4]{};
      read_float_element(*position, v, element);
      import.vertices.push_back(element[0]);
      import.vertices.push_back(element[1]);
      import.vertices.push_back(element[2]);
      if (color != nullptr) {
        float rgb[4]{};
        read_float_element(*color, v, rgb);
        import.vertices.push_back(rgb[0]);
        import.vertices.push_back(rgb[1]);
        import.vertices.push_back(rgb[2]);
      } else {
        import.vertices.push_back(1.0f);
        import.vertices.push_back(1.0f);
        import.vertices.push_back(1.0f);
      }
      if (normal != nullptr) {
        float nrm[4]{};
        read_float_element(*normal, v, nrm);
        import.vertices.push_back(nrm[0]);
        import.vertices.push_back(nrm[1]);
        import.vertices.push_back(nrm[2]);
      } else {
        // Default normal points toward +Z (the viewer's forward in the
        // canonical view-projection convention used by the scene path).
        import.vertices.push_back(0.0f);
        import.vertices.push_back(0.0f);
        import.vertices.push_back(1.0f);
      }
      if (uv != nullptr) {
        float tex[2]{};
        read_float_element(*uv, v, tex);
        import.vertices.push_back(tex[0]);
        import.vertices.push_back(tex[1]);
      } else {
        import.vertices.push_back(0.0f);
        import.vertices.push_back(0.0f);
      }
    }

    // ------------------------------------------------------- decode indices
    const std::size_t index_count =
        indices != nullptr ? indices->count : position->count;
    import.indices.reserve(import.indices.size() + index_count);
    if (indices == nullptr) {
      for (std::size_t i = 0; i < index_count; ++i) {
        import.indices.push_back(
            static_cast<std::uint32_t>(base_vertex + i));
      }
    } else {
      const View& index_view = views[indices->view_index];
      const BufferSource& index_buffer = sources[index_view.buffer_index];
      const std::size_t index_stride =
          indices->component_type == kComponentUShort ? 2U : 4U;
      for (std::size_t i = 0; i < index_count; ++i) {
        const std::size_t absolute = index_view.byte_offset +
                                     indices->byte_offset + i * index_stride;
        const std::uint64_t raw =
            read_le_unsigned(index_buffer.data + absolute, index_stride);
        if (raw >= position->count) {
          return finish(fail_asset(
              error, primitive_context + " index " + std::to_string(raw) +
                         " at position " + std::to_string(i) +
                         " is out of range for " +
                         std::to_string(position->count) + " vertices"));
        }
        import.indices.push_back(static_cast<std::uint32_t>(base_vertex + raw));
      }
    }
  }

  // Publish the merged local-space box only when every primitive declared one.
  if (all_primitive_bounds && any_primitive_bounds) {
    std::memcpy(import.bounds.min, box_min, sizeof(box_min));
    std::memcpy(import.bounds.max, box_max, sizeof(box_max));
    import.bounds.valid = true;
  }

  // Top-level convenience fields alias the first material-binding primitive.
  if (any_binder) {
    const GltfPrimitiveMaterial& first = import.primitives[first_binder];
    import.base_color = first.base_color;
    import.albedo = first.albedo;
  }

  if (import.empty()) {
    return finish(fail_asset(error, "imported mesh is empty"));
  }
  return finish(true);
}

// ============================================================================
// Scene graph import (nodes/scenes -> flattened world-space instances)
// ============================================================================


::warploom::core::Result<GltfSceneImport> import_gltf_scene(
    const char* json_bytes, std::size_t json_len,
    const std::uint8_t* bin_bytes, std::size_t bin_len,
    std::size_t scene_index, std::string* error_detail,
    const ExternalFileLoader* loader) {
  std::string error;
  GltfSceneImport scene;

  auto finish = [&](bool ok) -> ::warploom::core::Result<GltfSceneImport> {
    if (!ok) {
      if (error_detail != nullptr) *error_detail = error;
      return ::warploom::core::Result<GltfSceneImport>::error(
          ::warploom::core::RuntimeError::malformed_asset);
    }
    return ::warploom::core::Result<GltfSceneImport>::ok(std::move(scene));
  };

  if (json_bytes == nullptr && json_len != 0U) {
    return finish(fail_asset(error, "json_bytes is null"));
  }
  if (bin_bytes == nullptr && bin_len != 0U) {
    return finish(fail_asset(error, "bin_bytes is null"));
  }
  if (json_len == 0U) {
    return finish(fail_asset(error, "empty glTF document"));
  }

  // Auto-detect the container: raw JSON or a GLB 2.0 container.
  std::string container_json;
  DocumentPrologue prologue;
  if (!parse_gltf_document_prologue(json_bytes, json_len, bin_bytes, bin_len,
                                    container_json, prologue, error)) {
    return finish(false);
  }

  JsonParser parser(prologue.json_bytes, prologue.json_len, error);
  Json document;
  if (!parser.parse(document) || document.kind != Json::Kind::Object) {
    return finish(false);
  }

  const Json* asset = find_member(document, "asset");
  if (asset == nullptr) {
    return finish(fail_asset(error, "document is missing asset"));
  }
  const Json* version = find_member(*asset, "version");
  if (version == nullptr || version->kind != Json::Kind::String ||
      version->string.size() < 3U || version->string[0] != '2' ||
      version->string[1] != '.') {
    return finish(fail_asset(error, "asset.version must be a glTF 2.x string"));
  }

  const Json* nodes_json = find_member(document, "nodes");
  const Json* scenes_json = find_member(document, "scenes");
  const Json* meshes_json = find_member(document, "meshes");
  const std::size_t declared_meshes =
      (meshes_json != nullptr && meshes_json->kind == Json::Kind::Array)
          ? meshes_json->items.size()
          : 0U;
  if (nodes_json == nullptr) {
    return finish(fail_asset(error, "document is missing nodes array"));
  }
  if (nodes_json->kind != Json::Kind::Array) {
    return finish(fail_asset(error, "nodes must be an array"));
  }
  if (scenes_json == nullptr || scenes_json->kind != Json::Kind::Array ||
      scenes_json->items.empty()) {
    return finish(fail_asset(
        error, "document must declare a non-empty scenes array"));
  }
  if (scene_index >= scenes_json->items.size()) {
    return finish(fail_asset(
        error, "scene index " + std::to_string(scene_index) +
                   " is out of range (document declares " +
                   std::to_string(scenes_json->items.size()) + " scenes)"));
  }

  // ------------------------------------------------------------- parse nodes
  struct SceneNode {
    std::string name{};
    std::vector<std::size_t> children{};
    float matrix[16]{0.0f};
    float translation[3]{0.0f};
    float rotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float scale[3]{1.0f, 1.0f, 1.0f};
    GltfTransform local{gltf_identity_transform()};
    bool has_matrix{false};
    bool has_mesh{false};
    bool mesh_referenced{false};
    std::size_t mesh_index{0};
  };
  std::vector<SceneNode> nodes;
  nodes.reserve(nodes_json->items.size());
  if (nodes_json->items.size() > 65536U) {
    return finish(fail_asset(
        error, "node count " + std::to_string(nodes_json->items.size()) +
                   " exceeds the supported limit of 65536"));
  }
  for (std::size_t i = 0; i < nodes_json->items.size(); ++i) {
    const Json& node_json = nodes_json->items[i];
    const std::string node_context = "nodes[" + std::to_string(i) + "]";
    if (node_json.kind != Json::Kind::Object) {
      return finish(fail_asset(error, node_context + " must be an object"));
    }
    SceneNode node;
    if (const Json* name = find_member(node_json, "name");
        name != nullptr && name->kind == Json::Kind::String) {
      node.name = name->string;
    }
    // Children indices are range-checked here so traversal can trust them.
    if (const Json* children = find_member(node_json, "children");
        children != nullptr) {
      if (children->kind != Json::Kind::Array) {
        return finish(fail_asset(error, node_context +
                                            ".children must be an array"));
      }
      node.children.reserve(children->items.size());
      for (std::size_t c = 0; c < children->items.size(); ++c) {
        std::int64_t child_index = 0;
        if (!as_int(children->items[c], child_index, error,
                    node_context + ".children[" + std::to_string(c) + "]")) {
          return finish(false);
        }
        if (child_index < 0 ||
            static_cast<std::size_t>(child_index) >= nodes_json->items.size()) {
          return finish(fail_asset(
              error, node_context + ".children[" + std::to_string(c) +
                         "] node " + std::to_string(child_index) +
                         " is out of range (declared " +
                         std::to_string(nodes_json->items.size()) + " nodes)"));
        }
        node.children.push_back(static_cast<std::size_t>(child_index));
      }
    }
    // Mesh + optional skin. Skinned nodes are rejected outright.
    if (const Json* mesh_ref = find_member(node_json, "mesh"); mesh_ref != nullptr) {
      std::int64_t mesh_index = 0;
      if (!as_int(*mesh_ref, mesh_index, error, node_context + ".mesh")) {
        return finish(false);
      }
      if (mesh_index < 0 ||
          static_cast<std::size_t>(mesh_index) >= declared_meshes) {
        return finish(fail_asset(
            error, node_context + ".mesh " + std::to_string(mesh_index) +
                       " is out of range (declared " +
                       std::to_string(declared_meshes) + " meshes)"));
      }
      node.has_mesh = true;
      node.mesh_referenced = true;
      node.mesh_index = static_cast<std::size_t>(mesh_index);
    }
    if (find_member(node_json, "skin") != nullptr) {
      return finish(fail_asset(
          error, node_context + " references a skin; skinned meshes are not "
                 "supported (bake the deformation or strip the skin)"));
    }
    // Transform: matrix XOR (translation + rotation + scale).
    bool has_trs = false;
    bool has_matrix = false;
    if (const Json* matrix = find_member(node_json, "matrix");
        matrix != nullptr) {
      has_matrix = true;
      if (matrix->kind != Json::Kind::Array ||
          matrix->items.size() != 16U) {
        return finish(fail_asset(
            error, node_context +
                       ".matrix must be an array of 16 numbers"));
      }
      for (std::size_t c = 0; c < 16U; ++c) {
        double value = 0.0;
        if (!as_real(matrix->items[c], value, error,
                     node_context + ".matrix")) {
          return finish(false);
        }
        node.matrix[c] = static_cast<float>(value);
      }
    }
    const auto read_vec = [&](const char* key, std::size_t count, float* out,
                              bool& present) -> bool {
      const Json* member = find_member(node_json, key);
      if (member == nullptr) return true;
      present = true;
      if (member->kind != Json::Kind::Array ||
          member->items.size() != count) {
        return fail_asset(error, node_context + "." + key +
                                     " must be an array of " +
                                     std::to_string(count) + " numbers");
      }
      for (std::size_t c = 0; c < count; ++c) {
        double value = 0.0;
        if (!as_real(member->items[c], value, error,
                     node_context + "." + key)) {
          return false;
        }
        out[c] = static_cast<float>(value);
      }
      return true;
    };
    bool t = false, r = false, s = false;
    if (!read_vec("translation", 3U, node.translation, t) ||
        !read_vec("rotation", 4U, node.rotation, r) ||
        !read_vec("scale", 3U, node.scale, s)) {
      return finish(false);
    }
    has_trs = t || r || s;
    if (has_matrix && has_trs) {
      return finish(fail_asset(
          error, node_context + " must not define both matrix and TRS"));
    }
    if (has_matrix) {
      for (int c = 0; c < 16; ++c) node.local[c] = node.matrix[c];
    } else {
      trs_matrix(t ? node.translation : nullptr,
                 r ? node.rotation : nullptr,
                 s ? node.scale : nullptr, node.local.data());
    }
    nodes.push_back(std::move(node));
  }

  // ------------------------------------------------------------- traverse
  constexpr std::size_t kMaxTraversalDepth = 512;
  //! 0 = unvisited, 1 = on the current DFS path, 2 = fully expanded.
  std::vector<std::uint8_t> state(nodes.size(), 0U);
  std::vector<std::size_t> doc_to_scene_mesh(
      declared_meshes, std::numeric_limits<std::size_t>::max());

  // Recursion depth is capped, so the recursion itself is safe.
  std::function<bool(std::size_t, const GltfTransform&, std::size_t)>
      visit = [&](std::size_t node_index, const GltfTransform& parent_world,
                  std::size_t depth) -> bool {
    if (depth > kMaxTraversalDepth) {
      return fail_asset(error, "node hierarchy exceeds the maximum traversal "
                               "depth of " +
                                   std::to_string(kMaxTraversalDepth));
    }
    SceneNode& node = nodes[node_index];
    if (state[node_index] == 1U) {
      return fail_asset(error, "node cycle detected at nodes[" +
                                   std::to_string(node_index) + "]");
    }
    if (state[node_index] == 2U) {
      // Shared subtree already expanded under another path: revisit it as a
      // distinct instance with this path's world transform.
    }
    state[node_index] = 1U;
    GltfTransform world{};
    mat_mul(parent_world.data(), node.local.data(), world.data());
    if (node.mesh_referenced) {
      std::size_t& scene_mesh_index =
          doc_to_scene_mesh[node.mesh_index];
      if (scene_mesh_index == std::numeric_limits<std::size_t>::max()) {
        std::string mesh_error;
        auto imported = import_gltf_mesh(
            json_bytes, json_len, bin_bytes, bin_len, node.mesh_index,
            &mesh_error, loader);
        if (!imported.is_ok()) {
          if (error.empty()) error = mesh_error;
          return false;
        }
        scene_mesh_index = scene.meshes.size();
        scene.meshes.push_back(std::move(imported).value());
      }
      GltfSceneImport::Node instance;
      instance.name = node.name;
      instance.has_mesh = true;
      instance.mesh_index = scene_mesh_index;
      instance.model = world;
      scene.nodes.push_back(std::move(instance));
    }
    for (const std::size_t child : node.children) {
      if (!visit(child, world, depth + 1U)) return false;
    }
    state[node_index] = 2U;
    return true;
  };

  const Json& scene_json = scenes_json->items[scene_index];
  const Json* roots = find_member(scene_json, "nodes");
  if (roots != nullptr) {
    if (roots->kind != Json::Kind::Array) {
      return finish(fail_asset(
          error, "scenes[" + std::to_string(scene_index) +
                     "].nodes must be an array"));
    }
    const GltfTransform identity = gltf_identity_transform();
    for (std::size_t j = 0; j < roots->items.size(); ++j) {
      std::int64_t root_index = 0;
      if (!as_int(roots->items[j], root_index, error,
                  "scenes[" + std::to_string(scene_index) +
                      "].nodes[" + std::to_string(j) + "]")) {
        return finish(false);
      }
      if (root_index < 0 ||
          static_cast<std::size_t>(root_index) >= nodes.size()) {
        return finish(fail_asset(
            error, "scenes[" + std::to_string(scene_index) +
                       "].nodes[" + std::to_string(j) + "] node " +
                       std::to_string(root_index) +
                       " is out of range (declared " +
                       std::to_string(nodes.size()) + " nodes)"));
      }
      if (!visit(static_cast<std::size_t>(root_index), identity, 0U)) {
        return finish(false);
      }
    }
  }
  return finish(true);
}

} // namespace warploom::asset
