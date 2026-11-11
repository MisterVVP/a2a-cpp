# Full before/after coordinates

Median of three repetitions, 256 measured operations per coordinate. See the integrated validation record for parameters and limitations.

| Store / transport / workload / c | Baseline sync rps | Candidate sync rps (delta) | Candidate async server rps | Sync p95 delta |
|---|---:|---:|---:|---:|
| inmemory/http_json/unary/c1 | 3047.8 | 3513.2 (+15.3%) | 2196.8 | -16.6% |
| inmemory/http_json/unary/c16 | 8139.3 | 8228.9 (+1.1%) | 7155.6 | -19.9% |
| inmemory/http_json/unary/c64 | 5977.6 | 6635.5 (+11.0%) | 5007.3 | +5.8% |
| inmemory/http_json/stream/c1 | 4099.4 | 4196.4 (+2.4%) | 1909.2 | +9.2% |
| inmemory/http_json/stream/c16 | 8841.0 | 8860.0 (+0.2%) | 5090.4 | +5.4% |
| inmemory/http_json/stream/c64 | 6789.0 | 6961.1 (+2.5%) | 4860.0 | +26.3% |
| inmemory/jsonrpc/unary/c1 | 1934.4 | 2675.0 (+38.3%) | 2133.1 | -24.4% |
| inmemory/jsonrpc/unary/c16 | 5123.9 | 6887.4 (+34.4%) | 5599.7 | -11.6% |
| inmemory/jsonrpc/unary/c64 | 4373.5 | 5712.8 (+30.6%) | 4655.9 | +14.5% |
| inmemory/jsonrpc/stream/c1 | 3178.7 | 4033.4 (+26.9%) | 1862.6 | -13.1% |
| inmemory/jsonrpc/stream/c16 | 7651.3 | 8277.1 (+8.2%) | 4882.9 | +13.2% |
| inmemory/jsonrpc/stream/c64 | 6955.9 | 6421.3 (-7.7%) | 4716.8 | -10.3% |
| inmemory/grpc/unary/c1 | 5805.5 | 5121.0 (-11.8%) | 3515.2 | -2.4% |
| inmemory/grpc/unary/c16 | 9552.6 | 9447.8 (-1.1%) | 8826.9 | -1.4% |
| inmemory/grpc/unary/c64 | 10313.7 | 10320.7 (+0.1%) | 11203.0 | -4.8% |
| inmemory/grpc/stream/c1 | 3837.5 | 3763.8 (-1.9%) | 3146.0 | +15.3% |
| inmemory/grpc/stream/c16 | 5199.0 | 5212.6 (+0.3%) | 6274.5 | +20.0% |
| inmemory/grpc/stream/c64 | 5765.3 | 6245.7 (+8.3%) | 7745.2 | +5.4% |
| postgres/http_json/unary/c1 | 1075.2 | 1058.3 (-1.6%) | 940.8 | -2.0% |
| postgres/http_json/unary/c16 | 2730.6 | 2674.5 (-2.1%) | 1972.7 | +9.6% |
| postgres/http_json/unary/c64 | 2241.8 | 2770.4 (+23.6%) | 2048.0 | -24.1% |
| postgres/http_json/stream/c1 | 1180.2 | 1252.1 (+6.1%) | 909.8 | -8.0% |
| postgres/http_json/stream/c16 | 2833.6 | 3057.2 (+7.9%) | 1891.8 | +7.1% |
| postgres/http_json/stream/c64 | 2376.2 | 2965.0 (+24.8%) | 2061.4 | -18.4% |
| postgres/jsonrpc/unary/c1 | 796.3 | 991.0 (+24.4%) | 892.5 | -17.2% |
| postgres/jsonrpc/unary/c16 | 2177.8 | 2060.8 (-5.4%) | 1792.2 | +24.6% |
| postgres/jsonrpc/unary/c64 | 1475.5 | 2357.2 (+59.8%) | 1931.2 | -36.8% |
| postgres/jsonrpc/stream/c1 | 1067.0 | 1174.7 (+10.1%) | 902.9 | -6.9% |
| postgres/jsonrpc/stream/c16 | 2298.7 | 2663.6 (+15.9%) | 1684.5 | -3.9% |
| postgres/jsonrpc/stream/c64 | 2565.3 | 2730.4 (+6.4%) | 1765.7 | +3.4% |
| postgres/grpc/unary/c1 | 1210.5 | 1215.7 (+0.4%) | 1161.5 | -4.7% |
| postgres/grpc/unary/c16 | 2766.3 | 3066.6 (+10.9%) | 2189.6 | +19.4% |
| postgres/grpc/unary/c64 | 2868.6 | 2884.8 (+0.6%) | 1992.9 | +2.4% |
| postgres/grpc/stream/c1 | 1022.8 | 1025.7 (+0.3%) | 886.9 | +5.0% |
| postgres/grpc/stream/c16 | 2152.1 | 2547.5 (+18.4%) | 1877.4 | -30.2% |
| postgres/grpc/stream/c64 | 1974.9 | 2361.3 (+19.6%) | 1726.2 | -11.9% |
