#!/usr/bin/env python3
"""Встроенный голос xVibe (*.safetensors, состояние модели) → бинарный файл состояния FlowLM для движка мода.

Логика — как в KevinAHM/pocket-tts-onnx (_state_from_model_state). Формат файла:
  'XVST' u32 count, затем count записей: u8 dtype (0 float32, 1 int64, 2 bool), u8 ndim, int64 dims[ndim], данные.
Порядок записей — как flow_lm_state_manifest в bundle.json.
Использование: voice_state.py bundle.json voice.safetensors out.xvst
"""
import json, struct, sys
import numpy as np
from safetensors import safe_open

bundle, src, out = sys.argv[1:4]
man = json.load(open(bundle))["flow_lm_state_manifest"]
DT = {"float32": np.float32, "int64": np.int64, "bool": np.bool_}

def filled(shape, dtype, fill):
    if fill == "nan": return np.full(shape, np.nan, dtype=dtype)
    if fill == "ones": return np.ones(shape, dtype=dtype)
    return np.zeros(shape, dtype=dtype)

ms = {}
with safe_open(src, framework="np") as h:
    for k in h.keys():
        mod, key = k.split("/", 1); ms.setdefault(mod, {})[key] = h.get_tensor(k)

def derive_step(m):
    if "step" in m: return np.asarray(m["step"], dtype=np.int64).reshape(1)
    if "offset" in m and "end_offset" not in m: return np.asarray(m["offset"], dtype=np.int64).reshape(1)
    if "current_end" in m: return np.array([m["current_end"].shape[0]], dtype=np.int64)
    return np.array([0], dtype=np.int64)

def adapt(srct, e):
    shape, dtype = tuple(e["shape"]), DT[e["dtype"]]
    srct = np.asarray(srct, dtype=dtype)
    if srct.shape == shape: return srct.copy()
    if srct.size == int(np.prod(shape)): return srct.reshape(shape).copy()
    t = filled(list(shape), dtype, e["fill"])
    if srct.ndim != len(shape): return t
    sl = tuple(slice(0, min(a, b)) for a, b in zip(srct.shape, shape))
    if all(s.start == s.stop for s in sl): return t
    t[sl] = srct[sl]; return t

with open(out, "wb") as f:
    f.write(b"XVST" + struct.pack("<I", len(man)))
    for e in man:
        m = ms.get(e["module"], {}); t = m.get(e["key"])
        if t is None and e["key"] == "step": t = derive_step(m)
        a = filled(e["shape"], DT[e["dtype"]], e["fill"]) if t is None else adapt(t, e)
        code = {"float32": 0, "int64": 1, "bool": 2}[e["dtype"]]
        f.write(struct.pack("<BB", code, a.ndim) + struct.pack(f"<{a.ndim}q", *a.shape) + np.ascontiguousarray(a).tobytes())
print("ok", out)
