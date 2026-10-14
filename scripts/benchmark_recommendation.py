#!/usr/bin/env python3
"""Run a small TLV recommendation or webpage-search baseline against a local server."""

import argparse
import json
import os
from pathlib import Path
import socket
import statistics
import struct
import subprocess
import threading
import time


ROOT = Path(__file__).resolve().parents[1]
RECOMMEND_QUERIES = ["a", "hello", "international", "中", "中囯", "xyz",
                     "hello中国", "x" * 64]
SEARCH_QUERIES = ["中国", "经济", "国际", "人民网", "科技", "新闻",
                  "alpha", "不存在词项"]


def receive_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise RuntimeError("server closed before a complete TLV response")
        data.extend(chunk)
    return bytes(data)


def request(connection, request_id, query, operation):
    body = json.dumps({"version": 1, "request_id": request_id,
                       "query": query, "top_k": 5},
                      ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    before = time.perf_counter_ns()
    connection.sendall(struct.pack("!II", operation, len(body)) + body)
    response_type, size = struct.unpack("!II", receive_exact(connection, 8))
    if response_type != (0x80000000 | operation) or size > 65536:
        raise RuntimeError("unexpected response type or size")
    result = json.loads(receive_exact(connection, size))
    elapsed_ms = (time.perf_counter_ns() - before) / 1_000_000
    if result.get("request_id") != request_id:
        raise RuntimeError("response request_id mismatch")
    return elapsed_ms, result


def percentile(values, percent):
    position = (len(values) - 1) * percent / 100
    lower = int(position)
    upper = min(lower + 1, len(values) - 1)
    return values[lower] + (values[upper] - values[lower]) * (position - lower)


def available_port():
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def process_stats(pid):
    raw = Path(f"/proc/{pid}/stat").read_text()
    fields = raw[raw.rfind(")") + 2:].split()
    ticks = os.sysconf("SC_CLK_TCK")
    cpu_seconds = (int(fields[11]) + int(fields[12])) / ticks
    status = Path(f"/proc/{pid}/status").read_text().splitlines()
    peak_rss_kib = next(int(line.split()[1]) for line in status
                        if line.startswith("VmHWM:"))
    return cpu_seconds, peak_rss_kib


def candidate_counts(recommend_bin, config, queries):
    result = {}
    for query in queries:
        command = [str(recommend_bin), "--config", str(config),
                   "--query", query, "--top-k", "5"]
        output = subprocess.check_output(command, text=True)
        result[query] = json.loads(output)["candidate_count"]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-bin", type=Path,
                        default=ROOT / "bin/searchengine")
    parser.add_argument("--recommend-bin", type=Path,
                        default=ROOT / "bin/recommend_keywords")
    parser.add_argument("--operation", choices=["recommendation", "search"],
                        default="recommendation")
    parser.add_argument("--config", type=Path,
                        default=ROOT / "conf/myconf.conf")
    parser.add_argument("--requests", type=int, default=240)
    parser.add_argument("--concurrency", type=int, default=4)
    parser.add_argument("--warmup", type=int, default=2,
                        help="warmup requests per connection")
    args = parser.parse_args()
    if args.requests <= 0 or args.concurrency <= 0 or args.warmup < 0:
        parser.error("requests and concurrency must be positive; warmup nonnegative")
    operation = 1 if args.operation == "recommendation" else 2
    queries = RECOMMEND_QUERIES if operation == 1 else SEARCH_QUERIES
    port = available_port()
    server = subprocess.Popen(
        [str(args.server_bin), "--config", str(args.config),
         "--host", "127.0.0.1", "--port", str(port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 10
        while True:
            try:
                connection = socket.create_connection(("127.0.0.1", port), .2)
                connection.close()
                break
            except OSError:
                if server.poll() is not None:
                    raise RuntimeError("server startup failed: " +
                                       server.stderr.read().decode())
                if time.monotonic() >= deadline:
                    raise RuntimeError("server startup timed out")
                time.sleep(.02)

        counts = (candidate_counts(args.recommend_bin, args.config, queries)
                  if operation == 1 else {})
        barrier = threading.Barrier(args.concurrency + 1)
        latencies = [[] for _ in range(args.concurrency)]
        codes = [[] for _ in range(args.concurrency)]
        search_counts = [{} for _ in range(args.concurrency)]
        errors = []

        def worker(number):
            try:
                with socket.create_connection(("127.0.0.1", port), 5) as connection:
                    connection.settimeout(10)
                    for index in range(args.warmup):
                        request(connection, f"warm-{number}-{index}",
                                queries[(number + index) % len(queries)], operation)
                    barrier.wait()
                    for index in range(number, args.requests, args.concurrency):
                        query = queries[index % len(queries)]
                        latency, result = request(connection, f"bench-{index}",
                                                  query, operation)
                        latencies[number].append(latency)
                        codes[number].append(result.get("code"))
                        if operation == 2:
                            search_counts[number][query] = result.get("candidate_count")
            except Exception as error:
                errors.append(str(error))
                barrier.abort()

        threads = [threading.Thread(target=worker, args=(index,))
                   for index in range(args.concurrency)]
        for thread in threads:
            thread.start()
        try:
            barrier.wait(timeout=30)
        except threading.BrokenBarrierError:
            raise RuntimeError("benchmark setup failed: " + repr(errors))
        cpu_before, _ = process_stats(server.pid)
        started = time.perf_counter()
        for thread in threads:
            thread.join()
        elapsed = time.perf_counter() - started
        if errors:
            raise RuntimeError("benchmark failed: " + repr(errors))
        cpu_after, rss_kib = process_stats(server.pid)
        samples = sorted(value for batch in latencies for value in batch)
        response_codes = [value for batch in codes for value in batch]
        if operation == 2:
            counts = {query: count for batch in search_counts
                      for query, count in batch.items()}
        result = {
            "operation": args.operation,
            "requests": len(samples), "concurrency": args.concurrency,
            "warmup_per_connection": args.warmup,
            "query_candidate_counts": counts,
            "duration_seconds": round(elapsed, 3),
            "throughput_requests_per_second": round(len(samples) / elapsed, 1),
            "latency_ms": {name: round(percentile(samples, percentage), 3)
                           for name, percentage in [("p50", 50), ("p95", 95),
                                                    ("p99", 99)]},
            "mean_latency_ms": round(statistics.mean(samples), 3),
            "response_codes": {str(code): response_codes.count(code)
                               for code in sorted(set(response_codes))},
            "server_cpu_seconds": round(cpu_after - cpu_before, 3),
            "server_peak_rss_kib": rss_kib,
        }
        print(json.dumps(result, ensure_ascii=False, indent=2))
    finally:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
        server.stderr.close()


if __name__ == "__main__":
    main()
