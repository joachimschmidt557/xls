// Copyright 2022 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef XLS_FLOWS_IR_WRAPPER_H_
#define XLS_FLOWS_IR_WRAPPER_H_

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/dslx/ast.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/default_dslx_stdlib_path.h"
#include "xls/dslx/import_data.h"
#include "xls/ir/function.h"
#include "xls/ir/proc.h"
#include "xls/jit/function_jit.h"
#include "xls/jit/proc_jit.h"

namespace xls {

// This class provides a buffer and convenience functions to access a
// JitChannelQueue.
class JitChannelQueueWrapper {
 public:
  static absl::StatusOr<JitChannelQueueWrapper> Create(JitChannelQueue* queue,
                                                       ProcJit* jit);

  // Get XLS type the queue stores.
  Type* GetType() const { return type_; }

  // Returns if the queue is empty.
  bool Empty() const { return (queue_ == nullptr) || (queue_->Empty()); }

  // Enqueue on the channel the value v.
  absl::Status Enqueue(const Value& v);

  // Dequeue on the channel the value v.
  absl::StatusOr<Value> Dequeue();

  // Convenience function to enqueue uint64.
  absl::Status EnqueueWithUint64(uint64_t v);

  // Convenience function to dequeue uint64.
  absl::StatusOr<uint64_t> DequeueWithUint64();

  // Return the buffer of the instance.
  absl::Span<uint8_t> buffer() { return absl::MakeSpan(buffer_); }

  // Enqueue the buffer on the channel.
  absl::Status Enqueue(absl::Span<uint8_t> buffer);

  // Dequeue the content of the channel in the buffer.
  absl::Status Dequeue(absl::Span<uint8_t> buffer);

 private:
  // Pointer to the jit.
  ProcJit* jit_ = nullptr;

  // Pointer to the jit channel queue this object wraps.
  JitChannelQueue* queue_ = nullptr;

  // XLS type of the data to be sent/received from the channel.
  Type* type_ = nullptr;

  // Preallocated buffer sized to hold the data in LLVM representation.
  std::vector<uint8_t> buffer_;
};

// This class owns and is responsible for the flow to take ownership of a set
// of DSLX modules, compile/typecheck them, and convert them into an
// IR package.
//
// Additional convenience functions are available.
class IrWrapper {
 public:
  // Retrieve a specific dslx module.
  absl::StatusOr<dslx::Module*> GetDslxModule(absl::string_view name) const;

  // Retrieve a specific top-level function from the compiled BOP IR.
  //
  // name is the unmangled name.
  absl::StatusOr<Function*> GetIrFunction(absl::string_view name) const;

  // Retrieve a specific top-level proc from the compiled BOP IR.
  //
  // name is the unmangled name.
  absl::StatusOr<Proc*> GetIrProc(absl::string_view name) const;

  // Retrieve top level package.
  absl::StatusOr<Package*> GetIrPackage() const;

  // Retrieve and create (if needed) the JIT for the given function name.
  absl::StatusOr<FunctionJit*> GetAndMaybeCreateFunctionJit(
      absl::string_view name);

  // Retrieve and create (if needed) the JIT for the given proc.
  absl::StatusOr<ProcJit*> GetAndMaybeCreateProcJit(absl::string_view name);

  // Retrieve JIT channel queue for the given channel name.
  absl::StatusOr<JitChannelQueue*> GetJitChannelQueue(
      absl::string_view name) const;

  // Retrieve JIT channel queue wrapper for the given channel name and jit
  absl::StatusOr<JitChannelQueueWrapper> CreateJitChannelQueueWrapper(
      absl::string_view name, ProcJit* jit) const;

  // Takes ownership of a set of DSLX modules, converts to IR and creates
  // an IrWrapper object.
  static absl::StatusOr<IrWrapper> Create(
      absl::string_view ir_package_name,
      std::unique_ptr<dslx::Module> top_module,
      absl::string_view top_module_path,
      std::unique_ptr<dslx::Module> other_module,
      absl::string_view other_module_path);

  static absl::StatusOr<IrWrapper> Create(
      absl::string_view ir_package_name,
      std::unique_ptr<dslx::Module> top_module,
      absl::string_view top_module_path,
      absl::Span<std::unique_ptr<dslx::Module>> other_modules =
          absl::Span<std::unique_ptr<dslx::Module>>{},
      absl::Span<absl::string_view> other_modules_path =
          absl::Span<absl::string_view>{});

 private:
  // Construct this object with a default ImportData.
  explicit IrWrapper(absl::string_view package_name)
      : import_data_(dslx::CreateImportData(xls::kDefaultDslxStdlibPath,
                                            /*additional_search_paths=*/{})),
        package_(std::make_unique<Package>(package_name)) {}

  // Pointers to the each of the DSLX modules explicitly given to this wrapper.
  //
  // Ownership of this and all other DSLX modules is with import_data_;
  dslx::Module* top_module_;
  std::vector<dslx::Module*> other_modules_;

  // Holds typechecked DSLX modules.
  dslx::ImportData import_data_;

  // IR Package.
  std::unique_ptr<Package> package_;

  // Holds pre-compiled IR Function Jit.
  absl::flat_hash_map<Function*, std::unique_ptr<FunctionJit>>
      pre_compiled_function_jit_;

  // Holds pre-compiled IR Proc Jit.
  absl::flat_hash_map<Proc*, std::unique_ptr<ProcJit>> pre_compiled_proc_jit_;

  // Holds set of queues for each channel in the top-level package.
  std::unique_ptr<JitChannelQueueManager> jit_channel_manager_;
};

}  // namespace xls

#endif  // XLS_FLOWS_IR_WRAPPER_H_
