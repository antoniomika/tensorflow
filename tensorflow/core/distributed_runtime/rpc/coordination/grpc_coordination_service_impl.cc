/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/distributed_runtime/rpc/coordination/grpc_coordination_service_impl.h"

#include "tensorflow/core/platform/threadpool.h"

namespace tensorflow {

GrpcCoordinationServiceImpl::GrpcCoordinationServiceImpl(
    thread::ThreadPool* compute_pool, ::grpc::ServerBuilder* server_builder)
    : compute_pool_(*compute_pool) {
  server_builder->RegisterService(&service_);
  cq_ = server_builder->AddCompletionQueue();
}

void GrpcCoordinationServiceImpl::HandleRPCsLoop() {
#define ENQUEUE_REQUEST(method)                                                \
  do {                                                                         \
    Call<GrpcCoordinationServiceImpl, grpc::CoordinationService::AsyncService, \
         method##Request, method##Response>::                                  \
        EnqueueRequest(                                                        \
            &service_, cq_.get(),                                              \
            &grpc::CoordinationService::AsyncService::Request##method,         \
            &GrpcCoordinationServiceImpl::method##Handler, false);             \
  } while (0)
  ENQUEUE_REQUEST(RegisterWorker);
  ENQUEUE_REQUEST(WaitForAllTasks);
  ENQUEUE_REQUEST(ShutdownAgent);
  ENQUEUE_REQUEST(ResetAgent);
  ENQUEUE_REQUEST(Heartbeat);
  ENQUEUE_REQUEST(ReportErrorToAgent);
  ENQUEUE_REQUEST(ReportErrorToService);
  ENQUEUE_REQUEST(InsertKeyValue);
  ENQUEUE_REQUEST(GetKeyValue);
  ENQUEUE_REQUEST(DeleteKeyValue);
  ENQUEUE_REQUEST(Barrier);
  ENQUEUE_REQUEST(CancelBarrier);
#undef ENQUEUE_REQUEST

  void* tag;  // Matches the operation started against this cq_.
  bool ok;

  while (true) {
    if (!cq_->Next(&tag, &ok)) {
      // The queue is shutting down.
      break;
    }
    GrpcCallTag<GrpcCoordinationServiceImpl>* callback_tag =
        static_cast<GrpcCallTag<GrpcCoordinationServiceImpl>*>(tag);

    if (callback_tag) {
      callback_tag->OnCompleted(this, ok);
    } else {
      cq_->Shutdown();
      break;
    }
  }
}

void GrpcCoordinationServiceImpl::Shutdown() {
  // This enqueues a special event (with a null tag) that causes the completion
  // queue to be shut down on the polling thread.
  shutdown_alarm_ = std::make_unique<::grpc::Alarm>(
      cq_.get(), gpr_now(GPR_CLOCK_MONOTONIC), nullptr);
}

}  // namespace tensorflow
