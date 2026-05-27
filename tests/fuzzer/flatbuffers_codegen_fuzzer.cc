#include <bfbs_gen_lua.h>
#include <bfbs_gen_nim.h>
#include <flatbuffers/flatc.h>
#include <idl_gen_binary.h>
#include <idl_gen_csharp.h>
#include <idl_gen_dart.h>
#include <idl_gen_fbs.h>
#include <idl_gen_go.h>
#include <idl_gen_java.h>
#include <idl_gen_json_schema.h>
#include <idl_gen_kotlin.h>
#include <idl_gen_lobster.h>
#include <idl_gen_php.h>
#include <idl_gen_python.h>
#include <idl_gen_rust.h>
#include <idl_gen_swift.h>
#include <idl_gen_text.h>
#include <idl_gen_ts.h>
#include <stddef.h>
#include <stdint.h>

#include <iostream>
#include <memory>
#include <string>

#include "flatbuffers/code_generator.h"
#include "flatbuffers/idl.h"  // For Parser and generation functions
#include "idl_gen_cpp.h"      // For C++ generator
#include "test_init.h"

static constexpr size_t kMinInputLength = 1;
static constexpr size_t kMaxInputLength = 16384;

static constexpr uint8_t flags_strict_json = 0x80;
static constexpr uint8_t flags_skip_unexpected_fields_in_json = 0x40;
static constexpr uint8_t flags_allow_non_utf8 = 0x20;

// Utility for test run.
OneTimeTestInit OneTimeTestInit::one_time_init_;

// Discards generated output instead of writing it to disk. flatc installs a
// real FileSaver before running code generation; the fuzzer has no writable
// filesystem, so it supplies this no-op sink. Without a non-null file_saver
// every generator that emits a file dereferences a null pointer, so the
// harness aborted before exercising any generator (see GenerateBinary in
// src/idl_gen_binary.cpp).
namespace {
class NullFileSaver : public flatbuffers::FileSaver {
 public:
  bool SaveFile(const char*, const char*, size_t, bool) override {
    return true;
  }
};
}  // namespace

static const char* g_program_name = nullptr;

static void Warn(const flatbuffers::FlatCompiler* flatc,
                 const std::string& warn, bool show_exe_name) {
  (void)flatc;
  if (show_exe_name) {
    printf("%s: ", g_program_name);
  }
  fprintf(stderr, "\nwarning:\n  %s\n\n", warn.c_str());
}

static void Error(const flatbuffers::FlatCompiler* flatc,
                  const std::string& err, bool usage, bool show_exe_name) {
  if (show_exe_name) {
    printf("%s: ", g_program_name);
  }
  if (usage && flatc) {
    fprintf(stderr, "%s\n", flatc->GetShortUsageString(g_program_name).c_str());
  }
  fprintf(stderr, "\nerror:\n  %s\n\n", err.c_str());
  // A code generator rejecting a schema (e.g. an unsupported option/type
  // combination) is an expected outcome while fuzzing, not a fatal condition;
  // returning lets the remaining generators still be exercised. flatc itself
  // exits here, but the fuzz harness must keep running.
}

namespace flatbuffers {
void LogCompilerWarn(const std::string& warn) {
  Warn(static_cast<const flatbuffers::FlatCompiler*>(nullptr), warn, true);
}
void LogCompilerError(const std::string& err) {
  Error(static_cast<const flatbuffers::FlatCompiler*>(nullptr), err, false,
        true);
}
}  // namespace flatbuffers

namespace {

// Runs every registered code generator (and its gRPC variant) over an
// already-parsed schema. Generated output is discarded through the
// NullFileSaver installed on parser.opts.
void RunAllGenerators(const flatbuffers::Parser& parser) {
  const std::string flatbuffers_version(flatbuffers::FLATBUFFERS_VERSION());
  std::vector<std::unique_ptr<flatbuffers::CodeGenerator>> generators;
  generators.emplace_back(flatbuffers::NewBinaryCodeGenerator());
  generators.emplace_back(flatbuffers::NewCppCodeGenerator());
  generators.emplace_back(flatbuffers::NewCSharpCodeGenerator());
  generators.emplace_back(flatbuffers::NewDartCodeGenerator());
  generators.emplace_back(flatbuffers::NewFBSCodeGenerator());
  generators.emplace_back(flatbuffers::NewGoCodeGenerator());
  generators.emplace_back(flatbuffers::NewJavaCodeGenerator());
  generators.emplace_back(flatbuffers::NewJsonSchemaCodeGenerator());
  generators.emplace_back(flatbuffers::NewKotlinCodeGenerator());
  generators.emplace_back(flatbuffers::NewKotlinKMPCodeGenerator());
  generators.emplace_back(flatbuffers::NewLobsterCodeGenerator());
  generators.emplace_back(flatbuffers::NewLuaBfbsGenerator(flatbuffers_version));
  generators.emplace_back(flatbuffers::NewNimBfbsGenerator(flatbuffers_version));
  generators.emplace_back(flatbuffers::NewPythonCodeGenerator());
  generators.emplace_back(flatbuffers::NewPhpCodeGenerator());
  generators.emplace_back(flatbuffers::NewRustCodeGenerator());
  generators.emplace_back(flatbuffers::NewTextCodeGenerator());
  generators.emplace_back(flatbuffers::NewSwiftCodeGenerator());
  generators.emplace_back(flatbuffers::NewTsCodeGenerator());

  const std::string temp_path = "/tmp/";
  for (auto& gen : generators) {
    auto p = gen.get();
    auto status = p->GenerateCode(parser, temp_path, "fuzzer_generated");
    if (status != flatbuffers::CodeGenerator::Status::OK) {
      TEST_OUTPUT_LINE("GenerateCode failed %d", status);
    }
    auto grpc_status =
        p->GenerateGrpcCode(parser, temp_path, "fuzzer_generated");
    if (grpc_status != flatbuffers::CodeGenerator::Status::OK) {
      TEST_OUTPUT_LINE("GenerateGrpcCode failed %d", grpc_status);
    }
  }
}

// Turns on the optional, opt-in code-generation features. Every generator
// gates a large amount of code (object API, mutable accessors, comparison
// operators, name strings, ...) behind these flags, so running the generators
// only with default options leaves those paths unexercised.
void EnableExtraCodegenOptions(flatbuffers::IDLOptions& opts) {
  opts.generate_object_based_api = true;
  opts.gen_compare = true;
  opts.mutable_buffer = true;
  opts.generate_name_strings = true;
  opts.gen_nullable = true;
  opts.gen_json_coders = true;
  opts.gen_generated = true;
  opts.cpp_static_reflection = true;
  opts.cpp_std = "c++17";  // required by cpp_static_reflection
  opts.gen_absl_hash = true;
  opts.java_primitive_has_method = true;
  opts.cs_gen_json_serializer = true;
  opts.gen_jvmstatic = true;
  opts.scoped_enums = true;
  opts.keep_prefix = true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Reserve one byte for Parser flags and one byte for repetition counter.
  if (size < 3) return 0;
  const uint8_t flags = data[0];
  (void)data[1];  //  reserved
  data += 2;
  size -= 2;  // bypass

  const std::string original(reinterpret_cast<const char*>(data), size);
  auto input = std::string(original.c_str());  // until '\0'
  if (input.size() < kMinInputLength || input.size() > kMaxInputLength)
    return 0;

  flatbuffers::IDLOptions opts;
  opts.strict_json = (flags & flags_strict_json);
  opts.skip_unexpected_fields_in_json =
      (flags & flags_skip_unexpected_fields_in_json);
  opts.allow_non_utf8 = (flags & flags_allow_non_utf8);

  static NullFileSaver null_file_saver;
  opts.file_saver = &null_file_saver;

  flatbuffers::Parser parser(opts);

  // Guarantee 0-termination in the input.
  auto parse_input = input.c_str();

  // Parse the input schema
  if (parser.Parse(parse_input)) {
    parser.Serialize();
    const uint8_t* buf = parser.builder_.GetBufferPointer();
    flatbuffers::Verifier verifier(buf, parser.builder_.GetSize());
    TEST_EQ(true, reflection::VerifySchemaBuffer(verifier));

    auto root = flatbuffers::GetRoot<flatbuffers::Table>(buf);
    if (verifier.VerifyTableStart(buf) && root->VerifyTableStart(verifier)) {
      if (parser.root_struct_def_) {
        std::string json_output;
        flatbuffers::GenText(parser, parser.builder_.GetBufferPointer(),
                             &json_output);
      }
    }

    // Run every generator twice: once with default options and once with the
    // optional features enabled, so both sides of each option-gated branch are
    // exercised.
    RunAllGenerators(parser);
    EnableExtraCodegenOptions(parser.opts);
    RunAllGenerators(parser);
  }

  return 0;
}
