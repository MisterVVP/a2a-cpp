FROM python:3.14.3-alpine3.23
WORKDIR /opt/mcp
COPY examples/tutorials/mcp_server/requirements.txt .
RUN pip install --no-cache-dir --requirement requirements.txt \
    && adduser -S -D -H -u 10002 mcp
COPY examples/tutorials/mcp_server/server.py .
COPY examples/tutorials/job_application_assistant/samples /fixtures/job_application
COPY examples/tutorials/customer_support_copilot/samples /fixtures/customer_support
USER mcp
ENTRYPOINT ["python", "/opt/mcp/server.py"]
