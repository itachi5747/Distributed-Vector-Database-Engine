from pathlib import Path
import math
import random
import struct
import sys
import tempfile
import time

import grpc


def _load_proto_modules():
    script_dir = Path(__file__).resolve().parent
    proto_file = script_dir / "src" / "proto" / "vecdb.proto"
    if not proto_file.exists():
        raise FileNotFoundError(f"Missing proto file: {proto_file}")

    with tempfile.TemporaryDirectory(prefix="vecdb_py_proto_") as tmpdir:
        out_dir = Path(tmpdir)

        try:
            from grpc_tools import protoc
        except ImportError as exc:
            raise ImportError(
                "grpc_tools is not installed. Activate the venv and install grpcio-tools."
            ) from exc

        args = [
            "protoc",
            f"-I{proto_file.parent}",
            f"--python_out={out_dir}",
            f"--grpc_python_out={out_dir}",
            str(proto_file),
        ]
        if protoc.main(args) != 0:
            raise RuntimeError("Failed to generate Python gRPC stubs from vecdb.proto")

        sys.path.insert(0, str(out_dir))
        import vecdb_pb2  # type: ignore
        import vecdb_pb2_grpc  # type: ignore

        return vecdb_pb2, vecdb_pb2_grpc


vecdb_pb2, vecdb_pb2_grpc = _load_proto_modules()

DIM = 128   # must match --dim on your server
COUNT = 5000  # how many vectors to insert
TARGET_RPS = 25.0
MAX_BACKOFF_S = 2.0


def rand_unit_vec(dim):
    v = [random.gauss(0, 1) for _ in range(dim)]
    n = math.sqrt(sum(x * x for x in v))
    return [x / n for x in v]


def pack_vec(floats):
    return struct.pack(f"{len(floats)}f", *floats)


channel = grpc.insecure_channel("localhost:50051")
stub = vecdb_pb2_grpc.VectorDBStub(channel)

print(f"Inserting {COUNT} vectors of dim={DIM}...")
next_allowed = time.monotonic()
backoff_s = 0.05
for i in range(COUNT):
    now = time.monotonic()
    if now < next_allowed:
        time.sleep(next_allowed - now)

    vec = rand_unit_vec(DIM)
    req = vecdb_pb2.InsertRequest(
        vector_id=i,
        vector=pack_vec(vec),
    )
    while True:
        try:
            stub.Insert(req)
            backoff_s = 0.05
            break
        except grpc.RpcError as exc:
            if exc.code() != grpc.StatusCode.RESOURCE_EXHAUSTED:
                raise
            time.sleep(backoff_s)
            backoff_s = min(backoff_s * 2.0, MAX_BACKOFF_S)

    if i % 500 == 0:
        print(f"  {i}/{COUNT} done")

    next_allowed = max(next_allowed + (1.0 / TARGET_RPS), time.monotonic())

print("Done! Check the dashboard now.")
