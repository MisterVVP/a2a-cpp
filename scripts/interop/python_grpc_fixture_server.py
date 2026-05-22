#!/usr/bin/env python3
import argparse
import concurrent.futures
import time

import grpc
from google.protobuf import empty_pb2

from a2a.v1 import a2a_pb2
from a2a.v1 import a2a_pb2_grpc

TASKS = {}


class Service(a2a_pb2_grpc.A2AServiceServicer):
    def SendMessage(self, request, context):
        task_id = request.message.task_id
        task = a2a_pb2.Task(id=task_id)
        task.status.state = a2a_pb2.TASK_STATE_WORKING
        TASKS[task_id] = task
        return a2a_pb2.SendMessageResponse(task=task)

    def GetTask(self, request, context):
        if request.id not in TASKS:
            context.abort(grpc.StatusCode.NOT_FOUND, "task not found")
        return TASKS[request.id]

    def CancelTask(self, request, context):
        if request.id not in TASKS:
            context.abort(grpc.StatusCode.NOT_FOUND, "task not found")
        task = TASKS[request.id]
        task.status.state = a2a_pb2.TASK_STATE_CANCELED
        return task

    def SendStreamingMessage(self, request, context):
        task = a2a_pb2.Task(id=request.message.task_id)
        task.status.state = a2a_pb2.TASK_STATE_WORKING
        yield a2a_pb2.StreamResponse(task=task)

    def SubscribeToTask(self, request, context):
        if request.id not in TASKS:
            context.abort(grpc.StatusCode.NOT_FOUND, "task not found")
        yield a2a_pb2.StreamResponse(task=TASKS[request.id])

    def CreateTaskPushNotificationConfig(self, request, context):
        context.abort(grpc.StatusCode.UNIMPLEMENTED, "Not implemented")

    def GetTaskPushNotificationConfig(self, request, context):
        context.abort(grpc.StatusCode.UNIMPLEMENTED, "Not implemented")

    def ListTaskPushNotificationConfigs(self, request, context):
        context.abort(grpc.StatusCode.UNIMPLEMENTED, "Not implemented")

    def DeleteTaskPushNotificationConfig(self, request, context):
        return empty_pb2.Empty()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    server = grpc.server(concurrent.futures.ThreadPoolExecutor(max_workers=4))
    a2a_pb2_grpc.add_A2AServiceServicer_to_server(Service(), server)
    server.add_insecure_port(f"127.0.0.1:{args.port}")
    server.start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        server.stop(0)


if __name__ == "__main__":
    main()
