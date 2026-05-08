#!/usr/bin/env python3
import argparse
import grpc

import a2a_pb2
import a2a_pb2_grpc


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--endpoint", required=True)
    args = p.parse_args()
    channel = grpc.insecure_channel(args.endpoint)
    stub = a2a_pb2_grpc.A2AServiceStub(channel)

    send = a2a_pb2.SendMessageRequest()
    send.message.role = "user"
    send.message.task_id = "py-interop-task"
    send_resp = stub.SendMessage(send, timeout=5)
    assert send_resp.task.id == "py-interop-task"

    get_resp = stub.GetTask(a2a_pb2.GetTaskRequest(id="py-interop-task"), timeout=5)
    assert get_resp.id == "py-interop-task"

    stream = stub.SubscribeTask(a2a_pb2.GetTaskRequest(id="py-interop-task"), timeout=5)
    events = list(stream)
    assert events and events[0].task.id == "py-interop-task"

    cancel_resp = stub.CancelTask(a2a_pb2.CancelTaskRequest(id="py-interop-task"), timeout=5)
    assert cancel_resp.status.state == a2a_pb2.TASK_STATE_CANCELED

    try:
        stub.GetTask(a2a_pb2.GetTaskRequest(id="missing"), timeout=5)
        raise AssertionError("missing task should fail")
    except grpc.RpcError as exc:
        assert exc.code() in (grpc.StatusCode.NOT_FOUND, grpc.StatusCode.INTERNAL)

if __name__ == "__main__":
    main()
