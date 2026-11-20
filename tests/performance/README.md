# A2A performance driver

See the [mdBook guide](https://mistervvp.github.io/a2a-cpp/performance/driver.html).

Documentation source: [book/src/performance/driver.md](../../book/src/performance/driver.md).

## Event HTTP runtime comparison

Build `tck_sut` and `tck_async_sut` in the same optimized Asio-enabled build,
then run the Linux process-level comparison:

```sh
python3 tests/performance/event_http_evidence.py \
  --baseline build/tests/tck_sut --event build/tests/tck_async_sut \
  --requests 4096 --output work/http-evidence/results.json
```

Both fixtures use the shared application, in-memory stores, protocol serializers,
request headers, persistent connections and equal request counts. The event
fixture uses the installed SDK listener. Each c1/c16/c64 case starts a fresh
process; clients connect before a barrier releases their workload. The report
includes throughput, completion and first-event p95/p99, protocol/network errors,
process CPU, resident memory and thread counts. A separate 64-idle-client case
measures resource use without application work. Results are local workload
evidence; account for client and host CPU limits when interpreting throughput.
