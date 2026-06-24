--- @file protobuf.lua
--- @brief Protobuf + Abseil + utf8_range 纯 xmake 编译
---
--- libprotobuf / libprotoc 源文件列表提取自 protobuf 官方 cmake file_lists.cmake
--- 不含任何测试代码。
---
--- abseil 使用白名单方式排除测试文件（remove_files 的 ** 通配不可靠）。
--- 详见: https://github.com/xmake-io/xmake/issues/5487

local pb_root = "$(projectdir)/ThirdParty/protobuf"
local absl_root = "$(projectdir)/ThirdParty/abseil-cpp"

-- libprotobuf-lite sources (26 files)
local libprotobuf_lite_srcs = {
        pb_root .. "/src/google/protobuf/any_lite.cc",
        pb_root .. "/src/google/protobuf/arena.cc",
        pb_root .. "/src/google/protobuf/arena_align.cc",
        pb_root .. "/src/google/protobuf/arenastring.cc",
        pb_root .. "/src/google/protobuf/arenaz_sampler.cc",
        pb_root .. "/src/google/protobuf/extension_set.cc",
        pb_root .. "/src/google/protobuf/generated_enum_util.cc",
        pb_root .. "/src/google/protobuf/generated_message_tctable_lite.cc",
        pb_root .. "/src/google/protobuf/generated_message_util.cc",
        pb_root .. "/src/google/protobuf/implicit_weak_message.cc",
        pb_root .. "/src/google/protobuf/inlined_string_field.cc",
        pb_root .. "/src/google/protobuf/io/coded_stream.cc",
        pb_root .. "/src/google/protobuf/io/io_win32.cc",
        pb_root .. "/src/google/protobuf/io/zero_copy_stream.cc",
        pb_root .. "/src/google/protobuf/io/zero_copy_stream_impl.cc",
        pb_root .. "/src/google/protobuf/io/zero_copy_stream_impl_lite.cc",
        pb_root .. "/src/google/protobuf/map.cc",
        pb_root .. "/src/google/protobuf/message_lite.cc",
        pb_root .. "/src/google/protobuf/micro_string.cc",
        pb_root .. "/src/google/protobuf/parse_context.cc",
        pb_root .. "/src/google/protobuf/port.cc",
        pb_root .. "/src/google/protobuf/raw_ptr.cc",
        pb_root .. "/src/google/protobuf/repeated_field.cc",
        pb_root .. "/src/google/protobuf/repeated_ptr_field.cc",
        pb_root .. "/src/google/protobuf/stubs/common.cc",
        pb_root .. "/src/google/protobuf/wire_format_lite.cc"
}

-- libprotobuf sources (79 files, includes lite)
local libprotobuf_srcs = {
        pb_root .. "/src/google/protobuf/any.pb.cc",
        pb_root .. "/src/google/protobuf/api.pb.cc",
        pb_root .. "/src/google/protobuf/duration.pb.cc",
        pb_root .. "/src/google/protobuf/empty.pb.cc",
        pb_root .. "/src/google/protobuf/field_mask.pb.cc",
        pb_root .. "/src/google/protobuf/source_context.pb.cc",
        pb_root .. "/src/google/protobuf/struct.pb.cc",
        pb_root .. "/src/google/protobuf/timestamp.pb.cc",
        pb_root .. "/src/google/protobuf/type.pb.cc",
        pb_root .. "/src/google/protobuf/wrappers.pb.cc",
        pb_root .. "/src/google/protobuf/any.cc",
        pb_root .. "/src/google/protobuf/any_lite.cc",
        pb_root .. "/src/google/protobuf/arena.cc",
        pb_root .. "/src/google/protobuf/arena_align.cc",
        pb_root .. "/src/google/protobuf/arenastring.cc",
        pb_root .. "/src/google/protobuf/arenaz_sampler.cc",
        pb_root .. "/src/google/protobuf/compiler/importer.cc",
        pb_root .. "/src/google/protobuf/compiler/parser.cc",
        pb_root .. "/src/google/protobuf/cpp_features.pb.cc",
        pb_root .. "/src/google/protobuf/descriptor.cc",
        pb_root .. "/src/google/protobuf/descriptor.pb.cc",
        pb_root .. "/src/google/protobuf/descriptor_database.cc",
        pb_root .. "/src/google/protobuf/dynamic_message.cc",
        pb_root .. "/src/google/protobuf/extension_set.cc",
        pb_root .. "/src/google/protobuf/extension_set_heavy.cc",
        pb_root .. "/src/google/protobuf/feature_resolver.cc",
        pb_root .. "/src/google/protobuf/generated_enum_util.cc",
        pb_root .. "/src/google/protobuf/generated_message_bases.cc",
        pb_root .. "/src/google/protobuf/generated_message_reflection.cc",
        pb_root .. "/src/google/protobuf/generated_message_tctable_full.cc",
        pb_root .. "/src/google/protobuf/generated_message_tctable_gen.cc",
        pb_root .. "/src/google/protobuf/generated_message_tctable_lite.cc",
        pb_root .. "/src/google/protobuf/generated_message_util.cc",
        pb_root .. "/src/google/protobuf/implicit_weak_message.cc",
        pb_root .. "/src/google/protobuf/inlined_string_field.cc",
        pb_root .. "/src/google/protobuf/internal_feature_helper.cc",
        pb_root .. "/src/google/protobuf/io/coded_stream.cc",
        pb_root .. "/src/google/protobuf/io/gzip_stream.cc",
        pb_root .. "/src/google/protobuf/io/io_win32.cc",
        pb_root .. "/src/google/protobuf/io/printer.cc",
        pb_root .. "/src/google/protobuf/io/strtod.cc",
        pb_root .. "/src/google/protobuf/io/tokenizer.cc",
        pb_root .. "/src/google/protobuf/io/zero_copy_sink.cc",
        pb_root .. "/src/google/protobuf/io/zero_copy_stream.cc",
        pb_root .. "/src/google/protobuf/io/zero_copy_stream_impl.cc",
        pb_root .. "/src/google/protobuf/io/zero_copy_stream_impl_lite.cc",
        pb_root .. "/src/google/protobuf/json/internal/lexer.cc",
        pb_root .. "/src/google/protobuf/json/internal/message_path.cc",
        pb_root .. "/src/google/protobuf/json/internal/parser.cc",
        pb_root .. "/src/google/protobuf/json/internal/unparser.cc",
        pb_root .. "/src/google/protobuf/json/internal/untyped_message.cc",
        pb_root .. "/src/google/protobuf/json/internal/writer.cc",
        pb_root .. "/src/google/protobuf/json/internal/zero_copy_buffered_stream.cc",
        pb_root .. "/src/google/protobuf/json/json.cc",
        pb_root .. "/src/google/protobuf/map.cc",
        pb_root .. "/src/google/protobuf/map_field.cc",
        pb_root .. "/src/google/protobuf/message.cc",
        pb_root .. "/src/google/protobuf/message_lite.cc",
        pb_root .. "/src/google/protobuf/micro_string.cc",
        pb_root .. "/src/google/protobuf/parse_context.cc",
        pb_root .. "/src/google/protobuf/port.cc",
        pb_root .. "/src/google/protobuf/raw_ptr.cc",
        pb_root .. "/src/google/protobuf/reflection_mode.cc",
        pb_root .. "/src/google/protobuf/reflection_ops.cc",
        pb_root .. "/src/google/protobuf/repeated_field.cc",
        pb_root .. "/src/google/protobuf/repeated_ptr_field.cc",
        pb_root .. "/src/google/protobuf/service.cc",
        pb_root .. "/src/google/protobuf/stubs/common.cc",
        pb_root .. "/src/google/protobuf/symbol_checker.cc",
        pb_root .. "/src/google/protobuf/text_format.cc",
        pb_root .. "/src/google/protobuf/unknown_field_set.cc",
        pb_root .. "/src/google/protobuf/util/delimited_message_util.cc",
        pb_root .. "/src/google/protobuf/util/field_comparator.cc",
        pb_root .. "/src/google/protobuf/util/field_mask_util.cc",
        pb_root .. "/src/google/protobuf/util/message_differencer.cc",
        pb_root .. "/src/google/protobuf/util/time_util.cc",
        pb_root .. "/src/google/protobuf/util/type_resolver_util.cc",
        pb_root .. "/src/google/protobuf/wire_format.cc",
        pb_root .. "/src/google/protobuf/wire_format_lite.cc"
}

-- libprotoc sources (137 files)
local libprotoc_srcs = {
        pb_root .. "/src/google/protobuf/compiler/code_generator.cc",
        pb_root .. "/src/google/protobuf/compiler/code_generator_lite.cc",
        pb_root .. "/src/google/protobuf/compiler/command_line_interface.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/enum.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/extension.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/field.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/field_chunk.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/field_generators/cord_field.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/field_generators/enum_field.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/field_generators/map_field.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/field_generators/message_field.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/field_generators/primitive_field.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/field_generators/string_field.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/field_generators/string_view_field.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/file.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/generator.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/helpers.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/ifndef_guard.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/message.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/message_layout_helper.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/namespace_printer.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/parse_function_generator.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/service.cc",
        pb_root .. "/src/google/protobuf/compiler/cpp/tracker.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_doc_comment.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_enum.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_enum_field.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_field_base.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_generator.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_helpers.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_map_field.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_message.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_message_field.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_primitive_field.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_reflection_class.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_repeated_enum_field.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_repeated_message_field.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_repeated_primitive_field.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_source_generator_base.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/csharp_wrapper_field.cc",
        pb_root .. "/src/google/protobuf/compiler/csharp/names.cc",
        pb_root .. "/src/google/protobuf/compiler/java/context.cc",
        pb_root .. "/src/google/protobuf/compiler/java/doc_comment.cc",
        pb_root .. "/src/google/protobuf/compiler/java/field_common.cc",
        pb_root .. "/src/google/protobuf/compiler/java/file.cc",
        pb_root .. "/src/google/protobuf/compiler/java/full/enum.cc",
        pb_root .. "/src/google/protobuf/compiler/java/full/enum_field.cc",
        pb_root .. "/src/google/protobuf/compiler/java/full/extension.cc",
        pb_root .. "/src/google/protobuf/compiler/java/full/generator_factory.cc",
        pb_root .. "/src/google/protobuf/compiler/java/full/make_field_gens.cc",
        pb_root .. "/src/google/protobuf/compiler/java/full/map_field.cc",
        pb_root .. "/src/google/protobuf/compiler/java/full/message.cc",
        pb_root .. "/src/google/protobuf/compiler/java/full/message_builder.cc",
        pb_root .. "/src/google/protobuf/compiler/java/full/message_field.cc",
        pb_root .. "/src/google/protobuf/compiler/java/full/primitive_field.cc",
        pb_root .. "/src/google/protobuf/compiler/java/full/service.cc",
        pb_root .. "/src/google/protobuf/compiler/java/full/string_field.cc",
        pb_root .. "/src/google/protobuf/compiler/java/generator.cc",
        pb_root .. "/src/google/protobuf/compiler/java/helpers.cc",
        pb_root .. "/src/google/protobuf/compiler/java/internal_helpers.cc",
        pb_root .. "/src/google/protobuf/compiler/java/java_features.pb.cc",
        pb_root .. "/src/google/protobuf/compiler/java/lite/enum.cc",
        pb_root .. "/src/google/protobuf/compiler/java/lite/enum_field.cc",
        pb_root .. "/src/google/protobuf/compiler/java/lite/extension.cc",
        pb_root .. "/src/google/protobuf/compiler/java/lite/generator_factory.cc",
        pb_root .. "/src/google/protobuf/compiler/java/lite/make_field_gens.cc",
        pb_root .. "/src/google/protobuf/compiler/java/lite/map_field.cc",
        pb_root .. "/src/google/protobuf/compiler/java/lite/message.cc",
        pb_root .. "/src/google/protobuf/compiler/java/lite/message_builder.cc",
        pb_root .. "/src/google/protobuf/compiler/java/lite/message_field.cc",
        pb_root .. "/src/google/protobuf/compiler/java/lite/primitive_field.cc",
        pb_root .. "/src/google/protobuf/compiler/java/lite/string_field.cc",
        pb_root .. "/src/google/protobuf/compiler/java/message_serialization.cc",
        pb_root .. "/src/google/protobuf/compiler/java/name_resolver.cc",
        pb_root .. "/src/google/protobuf/compiler/java/names.cc",
        pb_root .. "/src/google/protobuf/compiler/java/shared_code_generator.cc",
        pb_root .. "/src/google/protobuf/compiler/kotlin/field.cc",
        pb_root .. "/src/google/protobuf/compiler/kotlin/file.cc",
        pb_root .. "/src/google/protobuf/compiler/kotlin/generator.cc",
        pb_root .. "/src/google/protobuf/compiler/kotlin/message.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/enum.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/enum_field.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/extension.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/field.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/file.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/generator.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/helpers.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/import_writer.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/line_consumer.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/map_field.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/message.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/message_field.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/names.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/oneof.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/primitive_field.cc",
        pb_root .. "/src/google/protobuf/compiler/objectivec/tf_decode_data.cc",
        pb_root .. "/src/google/protobuf/compiler/php/names.cc",
        pb_root .. "/src/google/protobuf/compiler/php/php_generator.cc",
        pb_root .. "/src/google/protobuf/compiler/plugin.cc",
        pb_root .. "/src/google/protobuf/compiler/plugin.pb.cc",
        pb_root .. "/src/google/protobuf/compiler/python/generator.cc",
        pb_root .. "/src/google/protobuf/compiler/python/helpers.cc",
        pb_root .. "/src/google/protobuf/compiler/python/pyi_generator.cc",
        pb_root .. "/src/google/protobuf/compiler/retention.cc",
        pb_root .. "/src/google/protobuf/compiler/ruby/rbs_generator.cc",
        pb_root .. "/src/google/protobuf/compiler/ruby/ruby_generator.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/accessors/accessor_case.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/accessors/accessors.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/accessors/default_value.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/accessors/map.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/accessors/repeated_field.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/accessors/singular_cord.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/accessors/singular_message.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/accessors/singular_scalar.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/accessors/singular_string.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/accessors/unsupported_field.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/accessors/with_presence.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/context.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/crate_mapping.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/enum.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/extension.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/generator.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/message.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/naming.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/oneof.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/relative_path.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/rust_field_type.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/rust_keywords.cc",
        pb_root .. "/src/google/protobuf/compiler/rust/upb_helpers.cc",
        pb_root .. "/src/google/protobuf/compiler/subprocess.cc",
        pb_root .. "/src/google/protobuf/compiler/versions.cc",
        pb_root .. "/src/google/protobuf/compiler/zip_writer.cc",
        pb_root .. "/upb_generator/common.cc",
        pb_root .. "/upb_generator/common/names.cc",
        pb_root .. "/upb_generator/file_layout.cc",
        pb_root .. "/upb_generator/minitable/names.cc",
        pb_root .. "/upb_generator/minitable/names_internal.cc",
        pb_root .. "/upb_generator/plugin.cc"
}

-- libupb sources (63 files)
local libupb_srcs = {
        pb_root .. "/upb/base/status.c",
        pb_root .. "/upb/hash/common.c",
        pb_root .. "/upb/json/decode.c",
        pb_root .. "/upb/json/encode.c",
        pb_root .. "/upb/lex/atoi.c",
        pb_root .. "/upb/lex/round_trip.c",
        pb_root .. "/upb/lex/strtod.c",
        pb_root .. "/upb/lex/unicode.c",
        pb_root .. "/upb/mem/alloc.c",
        pb_root .. "/upb/mem/arena.c",
        pb_root .. "/upb/message/accessors.c",
        pb_root .. "/upb/message/array.c",
        pb_root .. "/upb/message/compare.c",
        pb_root .. "/upb/message/compat.c",
        pb_root .. "/upb/message/copy.c",
        pb_root .. "/upb/message/internal/compare_unknown.c",
        pb_root .. "/upb/message/internal/extension.c",
        pb_root .. "/upb/message/internal/iterator.c",
        pb_root .. "/upb/message/internal/message.c",
        pb_root .. "/upb/message/map.c",
        pb_root .. "/upb/message/map_sorter.c",
        pb_root .. "/upb/message/merge.c",
        pb_root .. "/upb/message/message.c",
        pb_root .. "/upb/mini_descriptor/build_enum.c",
        pb_root .. "/upb/mini_descriptor/decode.c",
        pb_root .. "/upb/mini_descriptor/internal/base92.c",
        pb_root .. "/upb/mini_descriptor/internal/encode.c",
        pb_root .. "/upb/mini_descriptor/link.c",
        pb_root .. "/upb/mini_table/compat.c",
        pb_root .. "/upb/mini_table/debug_string.c",
        pb_root .. "/upb/mini_table/extension_registry.c",
        pb_root .. "/upb/mini_table/generated_registry.c",
        pb_root .. "/upb/mini_table/internal/message.c",
        pb_root .. "/upb/mini_table/message.c",
        pb_root .. "/upb/reflection/def_pool.c",
        pb_root .. "/upb/reflection/def_type.c",
        pb_root .. "/upb/reflection/desc_state.c",
        pb_root .. "/upb/reflection/enum_def.c",
        pb_root .. "/upb/reflection/enum_reserved_range.c",
        pb_root .. "/upb/reflection/enum_value_def.c",
        pb_root .. "/upb/reflection/extension_range.c",
        pb_root .. "/upb/reflection/field_def.c",
        pb_root .. "/upb/reflection/file_def.c",
        pb_root .. "/upb/reflection/internal/def_builder.c",
        pb_root .. "/upb/reflection/internal/strdup2.c",
        pb_root .. "/upb/reflection/message.c",
        pb_root .. "/upb/reflection/message_def.c",
        pb_root .. "/upb/reflection/message_reserved_range.c",
        pb_root .. "/upb/reflection/method_def.c",
        pb_root .. "/upb/reflection/oneof_def.c",
        pb_root .. "/upb/reflection/service_def.c",
        pb_root .. "/upb/text/debug_string.c",
        pb_root .. "/upb/text/encode.c",
        pb_root .. "/upb/text/internal/encode.c",
        pb_root .. "/upb/util/def_to_proto.c",
        pb_root .. "/upb/util/required_fields.c",
        pb_root .. "/upb/wire/byte_size.c",
        pb_root .. "/upb/wire/decode.c",
        pb_root .. "/upb/wire/decode_fast/select.c",
        pb_root .. "/upb/wire/encode.c",
        pb_root .. "/upb/wire/eps_copy_input_stream.c",
        pb_root .. "/upb/wire/internal/decoder.c",
        pb_root .. "/upb/wire/reader.c"
}

target("abseil")
    set_kind("static")
    -- set_languages("c++17")
    add_rules("Rules.ThirdParty")
    set_group("protobuf")
    set_warnings("none")
    add_defines("NOMINMAX")

    add_sysincludedirs(absl_root, {public = true})

    add_files(absl_root .. "/absl/**/*.cc")

    remove_files(absl_root .. "/absl/**/*_test.cc")
    remove_files(absl_root .. "/absl/**/*_testing.cc")
    remove_files(absl_root .. "/absl/**/*benchmark*.cc")
    remove_files(absl_root .. "/absl/**/*_unittest.cc")
    remove_files(absl_root .. "/absl/**/*test_helper*.cc")
    remove_files(absl_root .. "/absl/**/*test_util*.cc")
    remove_files(absl_root .. "/absl/**/*test_defs*.cc")
    remove_files(absl_root .. "/absl/**/*test_common*.cc")
    remove_files(absl_root .. "/absl/**/*mock*.cc")
    remove_files(absl_root .. "/absl/**/*matchers*.cc")
    remove_files(absl_root .. "/absl/**/*gentables*.cc")
    remove_files(absl_root .. "/absl/**/print_hash_of.cc")
    remove_files(absl_root .. "/absl/**/chi_square.cc")
    remove_files(absl_root .. "/absl/**/pow10_helper.cc")

    if is_plat("windows") then
        add_syslinks("bcrypt", "advapi32", {public = true})
        add_cxflags("/w", {force = true})
    elseif is_plat("linux") then
        add_syslinks("pthread", {public = true})
    end

---------------------------------------------------------------------------
-- utf8_range
---------------------------------------------------------------------------

target("utf8_range")
    set_kind("static")
    -- set_languages("c11")
    add_rules("Rules.ThirdParty")
    set_group("protobuf")
    set_warnings("none")

    add_sysincludedirs(pb_root .. "/third_party/utf8_range", {public = true})
    add_files(pb_root .. "/third_party/utf8_range/utf8_range.c")
    
    if is_plat("windows") then
        add_cxflags("/w", {force = true})
    end

---------------------------------------------------------------------------
-- libprotobuf (full runtime, includes lite)
---------------------------------------------------------------------------

target("protobuf")
    set_kind("static")
    -- set_languages("c++17")
    add_rules("Rules.ThirdParty")
    set_warnings("none")

    add_deps("abseil", "utf8_range")

    add_sysincludedirs(pb_root .. "/src", {public = true})

    if is_plat("windows") then
        add_cxflags("/utf-8", "/bigobj", {force = true})
        add_cxflags("/w", {force = true})
    end

    -- Full protobuf (79 files) + lite (26 files)
    add_files(unpack(libprotobuf_srcs))
    add_files(unpack(libprotobuf_lite_srcs))

---------------------------------------------------------------------------
-- libupb (C runtime)
---------------------------------------------------------------------------

target("upb")
    set_kind("static")
    -- set_languages("c11")
    add_rules("Rules.ThirdParty")
    set_warnings("none")

    add_deps("utf8_range")

    add_sysincludedirs(pb_root, {public = true})
    add_sysincludedirs(pb_root .. "/upb/reflection/cmake", {public = true})

    if is_plat("windows") then
        add_cxflags("/Zc:preprocessor", {force = true})
        add_cxflags("/w", {force = true})
    end

    -- Bootstrap generated file
    add_files(pb_root .. "/upb/reflection/cmake/google/protobuf/descriptor.upb_minitable.c")

    add_files(unpack(libupb_srcs))

---------------------------------------------------------------------------
-- libprotoc (compiler library)
---------------------------------------------------------------------------

target("libprotoc")
    set_kind("static")
    -- set_languages("c++17")
    add_rules("Rules.ThirdParty")
    set_warnings("none")

    add_deps("protobuf", "upb", "abseil")

    if is_plat("windows") then
        add_cxflags("/utf-8", "/bigobj", {force = true})
        add_cxflags("/w", {force = true})
    end

    add_files(unpack(libprotoc_srcs))

    -- upb_generator sources (part of protoc)
    add_files(
        pb_root .. "/upb_generator/common.cc",
        pb_root .. "/upb_generator/common/names.cc",
        pb_root .. "/upb_generator/file_layout.cc",
        pb_root .. "/upb_generator/minitable/names.cc",
        pb_root .. "/upb_generator/minitable/names_internal.cc",
        pb_root .. "/upb_generator/plugin.cc")

---------------------------------------------------------------------------
-- protoc (compiler executable)
---------------------------------------------------------------------------

target("protoc")
    set_kind("binary")
    set_warnings("none")
    add_rules("Rules.ThirdParty")

    add_deps("libprotoc", "protobuf", "upb", "abseil", "utf8_range")

    add_files(pb_root .. "/src/google/protobuf/compiler/main.cc")

    if is_plat("windows") then
        add_cxflags("/utf-8", {force = true})
        add_syslinks("Shell32")
        add_cxflags("/w", {force = true})
    end