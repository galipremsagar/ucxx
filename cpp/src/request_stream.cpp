/**
 * Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See file LICENSE for terms.
 */
#include <ucp/api/ucp.h>

#include <ucxx/delayed_submission.h>
#include <ucxx/request_stream.h>

#if UCXX_ENABLE_PYTHON
#include <ucxx/python/python_future.h>
#endif

namespace ucxx {

RequestStream::RequestStream(std::shared_ptr<Endpoint> endpoint,
                             bool send,
                             void* buffer,
                             size_t length,
                             const bool enablePythonFuture)
  : Request(endpoint,
            std::make_shared<DelayedSubmission>(send, buffer, length),
            std::string(send ? "streamSend" : "streamRecv"),
            enablePythonFuture),
    _length(length)
{
  auto worker = Endpoint::getWorker(endpoint->getParent());

  // A delayed notification request is not populated immediately, instead it is
  // delayed to allow the worker progress thread to set its status, and more
  // importantly the Python future later on, so that we don't need the GIL here.
  worker->registerDelayedSubmission(
    std::bind(std::mem_fn(&Request::populateDelayedSubmission), this));
}

std::shared_ptr<RequestStream> createRequestStream(std::shared_ptr<Endpoint> endpoint,
                                                   bool send,
                                                   void* buffer,
                                                   size_t length,
                                                   const bool enablePythonFuture = false)
{
  return std::shared_ptr<RequestStream>(
    new RequestStream(endpoint, send, buffer, length, enablePythonFuture));
}

void RequestStream::request()
{
  ucp_request_param_t param = {.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                                               UCP_OP_ATTR_FIELD_DATATYPE |
                                               UCP_OP_ATTR_FIELD_USER_DATA,
                               .datatype  = ucp_dt_make_contig(1),
                               .user_data = this};

  if (_delayedSubmission->_send) {
    param.cb.send = streamSendCallback;
    _request      = ucp_stream_send_nbx(
      _endpoint->getHandle(), _delayedSubmission->_buffer, _delayedSubmission->_length, &param);
  } else {
    param.op_attr_mask |= UCP_OP_ATTR_FIELD_FLAGS;
    param.flags          = UCP_STREAM_RECV_FLAG_WAITALL;
    param.cb.recv_stream = streamRecvCallback;
    _request             = ucp_stream_recv_nbx(_endpoint->getHandle(),
                                   _delayedSubmission->_buffer,
                                   _delayedSubmission->_length,
                                   &_delayedSubmission->_length,
                                   &param);
  }
}

void RequestStream::populateDelayedSubmission()
{
  request();

#if UCXX_ENABLE_PYTHON
  const bool pythonFutureLog = _enablePythonFuture;

  if (pythonFutureLog)
    ucxx_trace_req("req: %p, %s, buffer: %p, size: %lu, future: %p, future handle: %p",
                   _request,
                   _operationName.c_str(),
                   _delayedSubmission->_buffer,
                   _delayedSubmission->_length,
                   _pythonFuture.get(),
                   _pythonFuture->getHandle());
#else
  const bool pythonFutureLog = false;
#endif

  if (!pythonFutureLog)
    ucxx_trace_req("req: %p, %s, buffer: %p, size: %lu",
                   _request,
                   _operationName.c_str(),
                   _delayedSubmission->_buffer,
                   _delayedSubmission->_length);
  process();
}

void RequestStream::callback(void* request, ucs_status_t status, size_t length)
{
  status  = length == _length ? status : UCS_ERR_MESSAGE_TRUNCATED;
  _status = status;

  if (status == UCS_ERR_MESSAGE_TRUNCATED) {
    const char* fmt = "length mismatch: %llu (got) != %llu (expected)";
    size_t len      = std::snprintf(nullptr, 0, fmt, length, _length);
    _status_msg     = std::string(len + 1, '\0');  // +1 for null terminator
    std::snprintf(_status_msg.data(), _status_msg.size(), fmt, length, _length);
  }

  Request::callback(request, status);
}

void RequestStream::streamSendCallback(void* request, ucs_status_t status, void* arg)
{
  ucxx_trace_req("req: %p, streamSendCallback", request);
  Request* req = (Request*)arg;
  return req->callback(request, status);
}

void RequestStream::streamRecvCallback(void* request, ucs_status_t status, size_t length, void* arg)
{
  ucxx_trace_req("req: %p, streamRecvCallback", request);
  RequestStream* req = (RequestStream*)arg;
  return req->callback(request, status, length);
}

}  // namespace ucxx
