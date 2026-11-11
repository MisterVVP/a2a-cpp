# Longer synchronous confirmation

Five alternating repetitions of 4,096 operations for each short-run alert. The 10% investigation trigger does not change existing benchmark thresholds.

| Synchronous coordinate | v0.5.2 ops/s | Candidate ops/s | Throughput delta | p95 delta |
|---|---:|---:|---:|---:|
| inmemory/grpc/unary/c1 | 6028.9 | 6509.7 | +8.0% | +1.5% |
| inmemory/grpc/stream/c1 | 3889.2 | 3902.3 | +0.3% | -1.4% |
| inmemory/grpc/stream/c16 | 5825.7 | 5955.0 | +2.2% | -10.6% |
| inmemory/http_json/stream/c64 | 9971.8 | 10345.0 | +3.7% | -17.1% |
| inmemory/jsonrpc/stream/c16 | 6214.9 | 8588.5 | +38.2% | -30.8% |
| inmemory/jsonrpc/unary/c64 | 4232.3 | 5274.6 | +24.6% | -7.9% |
| postgres/grpc/unary/c16 | 2956.5 | 2841.6 | -3.9% | +0.5% |
| postgres/jsonrpc/unary/c16 | 1907.4 | 2286.9 | +19.9% | -9.4% |
