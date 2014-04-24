// Copyright 2006-2008 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <errno.h>
#include <stdio.h>
#ifdef COMPRESS_STARTUP_DATA_BZ2
#include <bzlib.h>
#endif
#include <signal.h>

#include "v8.h"

#include "bootstrapper.h"
#include "flags.h"
#include "natives.h"
#include "platform.h"
#include "serialize.h"
#include "list.h"

#if V8_TARGET_ARCH_ARM
#include "arm/assembler-arm-inl.h"
#endif

using namespace v8;


class Compressor {
 public:
  virtual ~Compressor() {}
  virtual bool Compress(i::Vector<char> input) = 0;
  virtual i::Vector<char>* output() = 0;
};


class ListSnapshotSink : public i::SnapshotByteSink {
 public:
  explicit ListSnapshotSink(i::List<char>* data) : data_(data) { }
  virtual ~ListSnapshotSink() {}
  virtual void Put(int byte, const char* description) { data_->Add(byte); }
  virtual int Position() { return data_->length(); }
 private:
  i::List<char>* data_;
};


class SnapshotWriter {
 public:
  explicit SnapshotWriter(const char* snapshot_file)
      : fp_(GetFileDescriptorOrDie(snapshot_file))
      , raw_file_(NULL)
      , raw_context_file_(NULL)
      , compressor_(NULL)
      , omit_(false) {
  }

  ~SnapshotWriter() {
    fclose(fp_);
    if (raw_file_) fclose(raw_file_);
    if (raw_context_file_) fclose(raw_context_file_);
  }

  void SetCompressor(Compressor* compressor) {
    compressor_ = compressor;
  }

  void SetOmit(bool omit) {
    omit_ = omit;
  }

  void SetRawFiles(const char* raw_file, const char* raw_context_file) {
    raw_file_ = GetFileDescriptorOrDie(raw_file);
    raw_context_file_ = GetFileDescriptorOrDie(raw_context_file);
  }

  void WriteSnapshot(const i::List<char>& snapshot_data,
                     const i::Serializer& serializer,
                     const i::List<char>& context_snapshot_data,
                     const i::Serializer& context_serializer) const {
    WriteFilePrefix();
    WriteData("", snapshot_data, raw_file_);
    WriteData("context_", context_snapshot_data, raw_context_file_);
    WriteMeta("context_", context_serializer);
    WriteMeta("", serializer);
    WriteFileSuffix();
  }

 private:
  void WriteFilePrefix() const {
    fprintf(fp_, "// Autogenerated snapshot file. Do not edit.\n\n");
    fprintf(fp_, "#include \"v8.h\"\n");
    fprintf(fp_, "#include \"platform.h\"\n\n");
    fprintf(fp_, "#include \"snapshot.h\"\n\n");
    fprintf(fp_, "namespace v8 {\n");
    fprintf(fp_, "namespace internal {\n\n");
  }

  void WriteFileSuffix() const {
    fprintf(fp_, "}  // namespace internal\n");
    fprintf(fp_, "}  // namespace v8\n");
  }

  void WriteData(const char* prefix,
                 const i::List<char>& source_data,
                 FILE* raw_file) const {
    const i::List <char>* data_to_be_written = NULL;
    i::List<char> compressed_data;
    if (!compressor_) {
      data_to_be_written = &source_data;
    } else if (compressor_->Compress(source_data.ToVector())) {
      compressed_data.AddAll(*compressor_->output());
      data_to_be_written = &compressed_data;
    } else {
      i::PrintF("Compression failed. Aborting.\n");
      exit(1);
    }

    ASSERT(data_to_be_written);
    MaybeWriteRawFile(data_to_be_written, raw_file);
    WriteData(prefix, source_data, data_to_be_written);
  }

  void MaybeWriteRawFile(const i::List<char>* data, FILE* raw_file) const {
    if (!data || !raw_file)
      return;

    // Sanity check, whether i::List iterators truly return pointers to an
    // internal array.
    ASSERT(data->end() - data->begin() == data->length());

    size_t written = fwrite(data->begin(), 1, data->length(), raw_file);
    if (written != (size_t)data->length()) {
      i::PrintF("Writing raw file failed.. Aborting.\n");
      exit(1);
    }
  }

  void WriteData(const char* prefix,
                 const i::List<char>& source_data,
                 const i::List<char>* data_to_be_written) const {
    fprintf(fp_, "const byte Snapshot::%sdata_[] = {\n", prefix);
    if (!omit_)
      WriteSnapshotData(data_to_be_written);
    fprintf(fp_, "};\n");
    fprintf(fp_, "const int Snapshot::%ssize_ = %d;\n", prefix,
            data_to_be_written->length());

    if (data_to_be_written == &source_data && !omit_) {
      fprintf(fp_, "const byte* Snapshot::%sraw_data_ = Snapshot::%sdata_;\n",
              prefix, prefix);
      fprintf(fp_, "const int Snapshot::%sraw_size_ = Snapshot::%ssize_;\n",
              prefix, prefix);
    } else {
      fprintf(fp_, "const byte* Snapshot::%sraw_data_ = NULL;\n", prefix);
      fprintf(fp_, "const int Snapshot::%sraw_size_ = %d;\n",
              prefix, source_data.length());
    }
    fprintf(fp_, "\n");
  }

  void WriteMeta(const char* prefix, const i::Serializer& ser) const {
    WriteSizeVar(ser, prefix, "new", i::NEW_SPACE);
    WriteSizeVar(ser, prefix, "pointer", i::OLD_POINTER_SPACE);
    WriteSizeVar(ser, prefix, "data", i::OLD_DATA_SPACE);
    WriteSizeVar(ser, prefix, "code", i::CODE_SPACE);
    WriteSizeVar(ser, prefix, "map", i::MAP_SPACE);
    WriteSizeVar(ser, prefix, "cell", i::CELL_SPACE);
    WriteSizeVar(ser, prefix, "property_cell", i::PROPERTY_CELL_SPACE);
    fprintf(fp_, "\n");
  }

  void WriteSizeVar(const i::Serializer& ser, const char* prefix,
                    const char* name, int space) const {
    fprintf(fp_, "const int Snapshot::%s%s_space_used_ = %d;\n",
            prefix, name, ser.CurrentAllocationAddress(space));
  }

  void WriteSnapshotData(const i::List<char>* data) const {
    for (int i = 0; i < data->length(); i++) {
      if ((i & 0x1f) == 0x1f)
        fprintf(fp_, "\n");
      if (i > 0)
        fprintf(fp_, ",");
      fprintf(fp_, "%u", static_cast<unsigned char>(data->at(i)));
    }
    fprintf(fp_, "\n");
  }

  FILE* GetFileDescriptorOrDie(const char* filename) {
    FILE* fp = i::OS::FOpen(filename, "wb");
    if (fp == NULL) {
      i::PrintF("Unable to open file \"%s\" for writing.\n", filename);
      exit(1);
    }
    return fp;
  }

  FILE* fp_;
  FILE* raw_file_;
  FILE* raw_context_file_;
  Compressor* compressor_;
  bool omit_;
};


#ifdef COMPRESS_STARTUP_DATA_BZ2
class BZip2Compressor : public Compressor {
 public:
  BZip2Compressor() : output_(NULL) {}
  virtual ~BZip2Compressor() {
    delete output_;
  }
  virtual bool Compress(i::Vector<char> input) {
    delete output_;
    output_ = new i::ScopedVector<char>((input.length() * 101) / 100 + 1000);
    unsigned int output_length_ = output_->length();
    int result = BZ2_bzBuffToBuffCompress(output_->start(), &output_length_,
                                          input.start(), input.length(),
                                          9, 1, 0);
    if (result == BZ_OK) {
      output_->Truncate(output_length_);
      return true;
    } else {
      fprintf(stderr, "bzlib error code: %d\n", result);
      return false;
    }
  }
  virtual i::Vector<char>* output() { return output_; }

 private:
  i::ScopedVector<char>* output_;
};


class BZip2Decompressor : public StartupDataDecompressor {
 public:
  virtual ~BZip2Decompressor() { }

 protected:
  virtual int DecompressData(char* raw_data,
                             int* raw_data_size,
                             const char* compressed_data,
                             int compressed_data_size) {
    ASSERT_EQ(StartupData::kBZip2,
              V8::GetCompressedStartupDataAlgorithm());
    unsigned int decompressed_size = *raw_data_size;
    int result =
        BZ2_bzBuffToBuffDecompress(raw_data,
                                   &decompressed_size,
                                   const_cast<char*>(compressed_data),
                                   compressed_data_size,
                                   0, 1);
    if (result == BZ_OK) {
      *raw_data_size = decompressed_size;
    }
    return result;
  }
};
#endif


void DumpException(Handle<Message> message) {
  String::Utf8Value message_string(message->Get());
  String::Utf8Value message_line(message->GetSourceLine());
  fprintf(stderr, "%s at line %d\n", *message_string, message->GetLineNumber());
  fprintf(stderr, "%s\n", *message_line);
  for (int i = 0; i <= message->GetEndColumn(); ++i) {
    fprintf(stderr, "%c", i < message->GetStartColumn() ? ' ' : '^');
  }
  fprintf(stderr, "\n");
}


int main(int argc, char** argv) {
  V8::InitializeICU();
  i::Isolate::SetCrashIfDefaultIsolateInitialized();

  // By default, log code create information in the snapshot.
  i::FLAG_log_code = true;

#if V8_TARGET_ARCH_ARM
  // Printing flags on ARM requires knowing if we intend to enable
  // the serializer or not.
  v8::internal::CpuFeatures::SetHintCreatingSnapshot();
#endif

  // Print the usage if an error occurs when parsing the command line
  // flags or if the help flag is set.
  int result = i::FlagList::SetFlagsFromCommandLine(&argc, argv, true);
  if (result > 0 || argc != 2 || i::FLAG_help) {
    ::printf("Usage: %s [flag] ... outfile\n", argv[0]);
    i::FlagList::PrintHelp();
    return !i::FLAG_help;
  }
#ifdef COMPRESS_STARTUP_DATA_BZ2
  BZip2Decompressor natives_decompressor;
  int bz2_result = natives_decompressor.Decompress();
  if (bz2_result != BZ_OK) {
    fprintf(stderr, "bzip error code: %d\n", bz2_result);
    exit(1);
  }
#endif
  i::FLAG_logfile_per_isolate = false;

  Isolate* isolate = v8::Isolate::New();
  isolate->Enter();
  i::Isolate* internal_isolate = reinterpret_cast<i::Isolate*>(isolate);
  i::Serializer::RequestEnable(internal_isolate);
  Persistent<Context> context;
  {
    HandleScope handle_scope(isolate);
    context.Reset(isolate, Context::New(isolate));
  }

  if (context.IsEmpty()) {
    fprintf(stderr,
            "\nException thrown while compiling natives - see above.\n\n");
    exit(1);
  }
  if (i::FLAG_extra_code != NULL) {
    // Capture 100 frames if anything happens.
    V8::SetCaptureStackTraceForUncaughtExceptions(true, 100);
    HandleScope scope(isolate);
    v8::Context::Scope cscope(v8::Local<v8::Context>::New(isolate, context));
    const char* name = i::FLAG_extra_code;
    FILE* file = i::OS::FOpen(name, "rb");
    if (file == NULL) {
      fprintf(stderr, "Failed to open '%s': errno %d\n", name, errno);
      exit(1);
    }

    fseek(file, 0, SEEK_END);
    int size = ftell(file);
    rewind(file);

    char* chars = new char[size + 1];
    chars[size] = '\0';
    for (int i = 0; i < size;) {
      int read = static_cast<int>(fread(&chars[i], 1, size - i, file));
      if (read < 0) {
        fprintf(stderr, "Failed to read '%s': errno %d\n", name, errno);
        exit(1);
      }
      i += read;
    }
    fclose(file);
    Local<String> source = String::NewFromUtf8(isolate, chars);
    TryCatch try_catch;
    Local<Script> script = Script::Compile(source);
    if (try_catch.HasCaught()) {
      fprintf(stderr, "Failure compiling '%s'\n", name);
      DumpException(try_catch.Message());
      exit(1);
    }
    script->Run();
    if (try_catch.HasCaught()) {
      fprintf(stderr, "Failure running '%s'\n", name);
      DumpException(try_catch.Message());
      exit(1);
    }
  }
  // Make sure all builtin scripts are cached.
  { HandleScope scope(isolate);
    for (int i = 0; i < i::Natives::GetBuiltinsCount(); i++) {
      internal_isolate->bootstrapper()->NativesSourceLookup(i);
    }
  }
  // If we don't do this then we end up with a stray root pointing at the
  // context even after we have disposed of the context.
  internal_isolate->heap()->CollectAllGarbage(
      i::Heap::kNoGCFlags, "mksnapshot");
  i::Object* raw_context = *v8::Utils::OpenPersistent(context);
  context.Reset();

  // This results in a somewhat smaller snapshot, probably because it gets rid
  // of some things that are cached between garbage collections.
  i::List<char> snapshot_data;
  ListSnapshotSink snapshot_sink(&snapshot_data);
  i::StartupSerializer ser(internal_isolate, &snapshot_sink);
  ser.SerializeStrongReferences();

  i::List<char> context_data;
  ListSnapshotSink contex_sink(&context_data);
  i::PartialSerializer context_ser(internal_isolate, &ser, &contex_sink);
  context_ser.Serialize(&raw_context);
  ser.SerializeWeakReferences();

  {
    SnapshotWriter writer(argv[1]);
    writer.SetOmit(i::FLAG_omit);
    if (i::FLAG_raw_file && i::FLAG_raw_context_file)
      writer.SetRawFiles(i::FLAG_raw_file, i::FLAG_raw_context_file);
#ifdef COMPRESS_STARTUP_DATA_BZ2
    BZip2Compressor bzip2;
    writer.SetCompressor(&bzip2);
#endif
    writer.WriteSnapshot(snapshot_data, ser, context_data, context_ser);
  }

  isolate->Exit();
  isolate->Dispose();
  V8::Dispose();
  return 0;
}
