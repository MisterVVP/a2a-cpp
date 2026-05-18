#!/usr/bin/env python3
import argparse
import grpc

from a2a.v1 import a2a_pb2
from a2a.v1 import a2a_pb2_grpc


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--endpoint", required=True)
    args = p.parse_args()
    channel = grpc.insecure_channel(args.endpoint)
    stub = a2a_pb2_grpc.A2AServiceStub(channel)
    metadata = (("a2a-version", "1.0"),)

    send = a2a_pb2.SendMessageRequest()
    send.message.role = a2a_pb2.ROLE_USER
    send.message.task_id = "py-interop-task"
    send.message.parts.add().text = "hello from python interop"
    send_resp = stub.SendMessage(send, timeout=5, metadata=metadata)
    assert send_resp.task.id == "py-interop-task"

    get_resp = stub.GetTask(
        a2a_pb2.GetTaskRequest(id="py-interop-task"), timeout=5, metadata=metadata
    )
    assert get_resp.id == "py-interop-task"

    stream_request = a2a_pb2.SendMessageRequest()
    stream_request.message.role = a2a_pb2.ROLE_USER
    stream_request.message.task_id = "py-interop-stream-task"
    stream_request.message.parts.add().text = "hello from python interop stream"
    stream = stub.SendStreamingMessage(stream_request, timeout=5, metadata=metadata)
    events = list(stream)
    assert events and events[0].task.id == "py-interop-stream-task"

    cancel_task = a2a_pb2.SendMessageRequest()
    cancel_task.message.role = a2a_pb2.ROLE_USER
    cancel_task.message.task_id = "py-interop-cancel-task"
    cancel_task.message.parts.add().text = "input-required"
    stub.SendMessage(cancel_task, timeout=5, metadata=metadata)
    cancel_resp = stub.CancelTask(
        a2a_pb2.CancelTaskRequest(id="py-interop-cancel-task"), timeout=5, metadata=metadata
    )
    assert cancel_resp.status.state == a2a_pb2.TASK_STATE_CANCELED

    try:
        stub.GetTask(a2a_pb2.GetTaskRequest(id="missing"), timeout=5, metadata=metadata)
        raise AssertionError("missing task should fail")
    except grpc.RpcError as exc:
        assert exc.code() in (
            grpc.StatusCode.INVALID_ARGUMENT,
            grpc.StatusCode.NOT_FOUND,
            grpc.StatusCode.INTERNAL,
        )
        assert "Task not found" in (exc.details() or "")


if __name__ == "__main__":
    main()
