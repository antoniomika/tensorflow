/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/compile_only_service.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "tensorflow/compiler/xla/debug_options_flags.h"
#include "tensorflow/compiler/xla/service/backend.h"
#include "tensorflow/compiler/xla/service/computation_layout.h"
#include "tensorflow/compiler/xla/service/dump.h"
#include "tensorflow/compiler/xla/service/platform_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/gtl/cleanup.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/platform/host_info.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/stream_executor_no_cuda.h"

namespace xla {

/* static */ StatusOr<std::unique_ptr<CompileOnlyService>>
CompileOnlyService::NewService(se::Platform* platform) {
  ServiceOptions default_options;
  default_options.set_platform(platform);
  return NewService(default_options);
}

/* static */ StatusOr<std::unique_ptr<CompileOnlyService>>
CompileOnlyService::NewService(const ServiceOptions& options) {
  se::Platform* platform = options.platform();
  if (platform == nullptr) {
    TF_ASSIGN_OR_RETURN(platform, PlatformUtil::GetDefaultPlatform());
  }

  TF_ASSIGN_OR_RETURN(auto compiler, Compiler::GetForPlatform(platform));

  std::unique_ptr<CompileOnlyService> service(
      new CompileOnlyService(options, compiler));
  return std::move(service);
}

CompileOnlyService::CompileOnlyService(const ServiceOptions& options,
                                       Compiler* compiler)
    : Service(options, /*execute_backend=*/nullptr), compiler_(compiler) {}

StatusOr<std::vector<std::unique_ptr<AotCompilationResult>>>
CompileOnlyService::CompileAheadOfTime(
    const absl::Span<const AotXlaComputationInstance> computations,
    const AotCompilationOptions& options,
    std::unique_ptr<AotCompilationMetadata>* metadata) {
  std::vector<std::unique_ptr<HloModule>> hlo_modules;

  const DebugOptions& debug_options = options.debug_options();
  ExecutionOptions execution_options;
  *execution_options.mutable_debug_options() = debug_options;
  // Capture replica_count, num_cores, and device_assignment in ExecutionOptions
  // to later save in a proto dump.
  if (options.replica_count() > 0) {
    execution_options.set_num_replicas(options.replica_count());
    if (options.has_static_device_assignment()) {
      CHECK_EQ(options.replica_count(),
               options.static_device_assignment().replica_count());
    }
  }
  if (options.num_cores() > 0) {
    execution_options.set_num_partitions(options.num_cores());
    if (options.has_static_device_assignment()) {
      CHECK_EQ(options.num_cores(),
               options.static_device_assignment().computation_count());
    }
  }
  if (options.has_static_device_assignment()) {
    TF_RETURN_IF_ERROR(options.static_device_assignment().Serialize(
        execution_options.mutable_device_assignment()));
  }
  execution_options.set_use_spmd_partitioning(options.use_spmd_partitioning());
  execution_options.set_deduplicate_hlo(options.deduplicate_hlo());
  for (const AotXlaComputationInstance& instance : computations) {
    TF_RET_CHECK(instance.computation.has_host_program_shape());
    *execution_options.mutable_shape_with_output_layout() =
        instance.result_layout->layout().tiles().empty()
            ? compiler_
                  ->DefaultDeviceShapeRepresentation(*instance.result_layout)
                  .ToProto()
            : instance.result_layout->ToProto();
    for (auto shape : instance.argument_layouts) {
      if (shape->layout().tiles().empty()) {
        *(const_cast<Shape*>(shape)) =
            compiler_->DefaultDeviceShapeRepresentation(*shape);
      }
    }

    TF_ASSIGN_OR_RETURN(
        std::unique_ptr<HloModuleConfig> module_config,
        CreateModuleConfig(
            ProgramShape(instance.computation.host_program_shape()),
            instance.argument_layouts, &execution_options, &options));

    TF_ASSIGN_OR_RETURN(
        std::unique_ptr<HloModule> hlo_module,
        HloModule::CreateFromProto(instance.computation, *module_config));
    DumpHloModuleIfEnabled(*hlo_module, "before_optimizations");
    hlo_modules.push_back(std::move(hlo_module));
  }

  execution_options.clear_shape_with_output_layout();
  DumpExecutionOptions(execution_options, debug_options);

  return compiler_->CompileAheadOfTime(
      absl::make_unique<HloModuleGroup>(hlo_modules[0]->name(),
                                        absl::MakeSpan(hlo_modules)),
      options, metadata);
}

}  // namespace xla
