#!/usr/bin/env python3
"""Writes tests/data/toy_scale_add.onnx (P6-01): y = x * 2 + 1 for x: float[1, 8].

Our own tiny model for the ONNX Runtime tests, written as raw protobuf with the standard library only (no onnx
package needed). Fields as in onnx.proto: ModelProto, GraphProto, NodeProto, ValueInfoProto, TypeProto,
TensorShapeProto, TensorProto. Run from the repository root: python tools/make_toy_onnx.py
"""

from __future__ import annotations

import struct
from pathlib import Path

FLOAT = 1  # TensorProto.DataType.FLOAT


def varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def key(field: int, wire: int) -> bytes:
    return varint((field << 3) | wire)


def f_varint(field: int, value: int) -> bytes:
    return key(field, 0) + varint(value)


def f_bytes(field: int, value: bytes) -> bytes:
    return key(field, 2) + varint(len(value)) + value


def f_str(field: int, value: str) -> bytes:
    return f_bytes(field, value.encode("utf-8"))


def value_info(name: str, dims: list[int]) -> bytes:
    shape = b"".join(f_bytes(1, f_varint(1, d)) for d in dims)  # TensorShapeProto.dim { dim_value }
    tensor_type = f_varint(1, FLOAT) + f_bytes(2, shape)        # TypeProto.Tensor { elem_type, shape }
    type_proto = f_bytes(1, tensor_type)                        # TypeProto { tensor_type }
    return f_str(1, name) + f_bytes(2, type_proto)              # ValueInfoProto { name, type }


def scalar(name: str, value: float) -> bytes:
    # TensorProto { data_type, name, raw_data } with no dims: a scalar.
    return f_varint(2, FLOAT) + f_str(8, name) + f_bytes(9, struct.pack("<f", value))


def node(op: str, inputs: list[str], outputs: list[str], name: str) -> bytes:
    return b"".join(f_str(1, i) for i in inputs) + b"".join(f_str(2, o) for o in outputs) + f_str(3, name) + f_str(4, op)


def model() -> bytes:
    graph = (
        f_bytes(1, node("Mul", ["x", "two"], ["t"], "scale"))
        + f_bytes(1, node("Add", ["t", "one"], ["y"], "offset"))
        + f_str(2, "toy_scale_add")
        + f_bytes(5, scalar("two", 2.0))
        + f_bytes(5, scalar("one", 1.0))
        + f_bytes(11, value_info("x", [1, 8]))
        + f_bytes(12, value_info("y", [1, 8]))
    )
    opset = f_str(1, "") + f_varint(2, 13)  # OperatorSetIdProto { domain "", version 13 }
    return f_varint(1, 8) + f_str(2, "devil-dj") + f_bytes(7, graph) + f_bytes(8, opset)  # ir_version 8


if __name__ == "__main__":
    out = Path("tests/data/toy_scale_add.onnx")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(model())
    print(f"wrote {out} ({out.stat().st_size} bytes)")
