//
//
// Copyright 2015 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//

#include "src/core/lib/transport/transport.h"

#include <grpc/event_engine/event_engine.h>
#include <grpc/grpc.h>
#include <grpc/support/port_platform.h>
#include <string.h>

#include <memory>
#include <new>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "src/core/lib/event_engine/default_event_engine.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/promise/for_each.h"
#include "src/core/lib/promise/promise.h"
#include "src/core/lib/promise/try_seq.h"
#include "src/core/lib/slice/slice.h"
#include "src/core/lib/transport/error_utils.h"
#include "src/core/util/time.h"

void grpc_stream_destroy(grpc_stream_refcount* refcount) {
  if ((grpc_core::ExecCtx::Get()->flags() &
       GRPC_EXEC_CTX_FLAG_THREAD_RESOURCE_LOOP)) {
    // Ick.
    // The thread we're running on MAY be owned (indirectly) by a call-stack.
    // If that's the case, destroying the call-stack MAY try to destroy the
    // thread, which is a tangled mess that we just don't want to ever have to
    // cope with.
    // Throw this over to the executor (on a core-owned thread) and process it
    // there.
    grpc_event_engine::experimental::GetDefaultEventEngine()->Run([refcount] {
      grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
      grpc_core::ExecCtx exec_ctx;
      grpc_core::ExecCtx::Run(DEBUG_LOCATION, &refcount->destroy,
                              absl::OkStatus());
    });
  } else {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, &refcount->destroy,
                            absl::OkStatus());
  }
}

void slice_stream_destroy(void* arg) {
  grpc_stream_destroy(static_cast<grpc_stream_refcount*>(arg));
}

#ifndef NDEBUG
void grpc_stream_ref_init(grpc_stream_refcount* refcount, int /*initial_refs*/,
                          grpc_iomgr_cb_func cb, void* cb_arg,
                          const char* object_type) {
  refcount->object_type = object_type;
#else
void grpc_stream_ref_init(grpc_stream_refcount* refcount, int /*initial_refs*/,
                          grpc_iomgr_cb_func cb, void* cb_arg) {
#endif
  GRPC_CLOSURE_INIT(&refcount->destroy, cb, cb_arg, grpc_schedule_on_exec_ctx);

  new (&refcount->refs) grpc_core::RefCount(
      1,
      GRPC_TRACE_FLAG_ENABLED(stream_refcount) ? "stream_refcount" : nullptr);
}

namespace grpc_core {
void Transport::SetPollingEntity(grpc_stream* stream,
                                 grpc_polling_entity* pollset_or_pollset_set) {
  if (auto* pollset = grpc_polling_entity_pollset(pollset_or_pollset_set)) {
    SetPollset(stream, pollset);
  } else if (auto* pollset_set =
                 grpc_polling_entity_pollset_set(pollset_or_pollset_set)) {
    SetPollsetSet(stream, pollset_set);
  } else {
    // No-op for empty pollset. Empty pollset is possible when using
    // non-fd-based event engines such as CFStream.
  }
}
}  // namespace grpc_core

// This comment should be sung to the tune of
// "Supercalifragilisticexpialidocious":
//
// grpc_transport_stream_op_batch_finish_with_failure
// is a function that must always unref cancel_error
// though it lives in lib, it handles transport stream ops sure
// it's grpc_transport_stream_op_batch_finish_with_failure
void grpc_transport_stream_op_batch_finish_with_failure(
    grpc_transport_stream_op_batch* batch, grpc_error_handle error,
    grpc_core::CallCombiner* call_combiner) {
  grpc_core::CallCombinerClosureList closures;
  grpc_transport_stream_op_batch_queue_finish_with_failure(batch, error,
                                                           &closures);
  // Execute closures.
  closures.RunClosures(call_combiner);
}

void grpc_transport_stream_op_batch_queue_finish_with_failure(
    grpc_transport_stream_op_batch* batch, grpc_error_handle error,
    grpc_core::CallCombinerClosureList* closures) {
  // Construct a list of closures to execute.
  if (batch->recv_initial_metadata) {
    closures->Add(
        batch->payload->recv_initial_metadata.recv_initial_metadata_ready,
        error, "failing recv_initial_metadata_ready");
  }
  if (batch->recv_message) {
    closures->Add(batch->payload->recv_message.recv_message_ready, error,
                  "failing recv_message_ready");
  }
  if (batch->recv_trailing_metadata) {
    closures->Add(
        batch->payload->recv_trailing_metadata.recv_trailing_metadata_ready,
        error, "failing recv_trailing_metadata_ready");
  }
  if (batch->on_complete != nullptr) {
    closures->Add(batch->on_complete, error, "failing on_complete");
  }
}

void grpc_transport_stream_op_batch_finish_with_failure_from_transport(
    grpc_transport_stream_op_batch* batch, grpc_error_handle error) {
  // Construct a list of closures to execute.
  if (batch->recv_initial_metadata) {
    grpc_core::ExecCtx::Run(
        DEBUG_LOCATION,
        batch->payload->recv_initial_metadata.recv_initial_metadata_ready,
        error);
  }
  if (batch->recv_message) {
    grpc_core::ExecCtx::Run(
        DEBUG_LOCATION, batch->payload->recv_message.recv_message_ready, error);
  }
  if (batch->recv_trailing_metadata) {
    grpc_core::ExecCtx::Run(
        DEBUG_LOCATION,
        batch->payload->recv_trailing_metadata.recv_trailing_metadata_ready,
        error);
  }
  if (batch->on_complete != nullptr) {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, batch->on_complete, error);
  }
}

struct made_transport_op {
  grpc_closure outer_on_complete;
  grpc_closure* inner_on_complete = nullptr;
  grpc_transport_op op;
  made_transport_op() {
    memset(&outer_on_complete, 0, sizeof(outer_on_complete));
  }
};

static void destroy_made_transport_op(void* arg, grpc_error_handle error) {
  made_transport_op* op = static_cast<made_transport_op*>(arg);
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, op->inner_on_complete, error);
  delete op;
}

grpc_transport_op* grpc_make_transport_op(grpc_closure* on_complete) {
  made_transport_op* op = new made_transport_op();
  GRPC_CLOSURE_INIT(&op->outer_on_complete, destroy_made_transport_op, op,
                    grpc_schedule_on_exec_ctx);
  op->inner_on_complete = on_complete;
  op->op.on_consumed = &op->outer_on_complete;
  return &op->op;
}

struct made_transport_stream_op {
  grpc_closure outer_on_complete;
  grpc_closure* inner_on_complete = nullptr;
  grpc_transport_stream_op_batch op;
  grpc_transport_stream_op_batch_payload payload;
};
static void destroy_made_transport_stream_op(void* arg,
                                             grpc_error_handle error) {
  made_transport_stream_op* op = static_cast<made_transport_stream_op*>(arg);
  grpc_closure* c = op->inner_on_complete;
  delete op;
  if (c != nullptr) {
    grpc_core::Closure::Run(DEBUG_LOCATION, c, error);
  }
}

grpc_transport_stream_op_batch* grpc_make_transport_stream_op(
    grpc_closure* on_complete) {
  made_transport_stream_op* op = new made_transport_stream_op();
  op->op.payload = &op->payload;
  GRPC_CLOSURE_INIT(&op->outer_on_complete, destroy_made_transport_stream_op,
                    op, grpc_schedule_on_exec_ctx);
  op->inner_on_complete = on_complete;
  op->op.on_complete = &op->outer_on_complete;
  return &op->op;
}
