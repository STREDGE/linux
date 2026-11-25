#!/usr/bin/env python3
"""Доводка экспорта xVibePocketTTS (русская Pocket TTS) под движок sherpa-onnx 1.12.x.

sherpa-onnx (offline-tts-pocket-impl.h):
  1) считает фразу законченной при eos_logit > -4 (зашито), а xVibe обучен с порогом -1 —
     фразы обрезаются. Сдвигаем выход eos_logit модели flow_lm_main на -3.
  (Вектор bos_before_voice экспортёр lomotron уже встраивает в mimi_encoder — его не трогаем.)

Использование: patch_for_sherpa.py <папка экспорта lomotron> <папка результата>
"""
import json, os, shutil, sys
import numpy as np
import onnx
from onnx import helper, numpy_helper

src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
EOS_SHIFT = -3.0

def patch_lm_main(name):
    m = onnx.load(os.path.join(src, name))
    g = m.graph
    out = next(o for o in g.output if o.name == "eos_logit")
    # переименовываем старый выход и добавляем Add(-3) под прежним именем
    for n in g.node:
        for i, o in enumerate(n.output):
            if o == "eos_logit":
                n.output[i] = "eos_logit_raw"
    for n in g.node:
        for i, x in enumerate(n.input):
            if x == "eos_logit":
                n.input[i] = "eos_logit_raw"
    g.initializer.append(numpy_helper.from_array(np.array([EOS_SHIFT], dtype=np.float32), "eos_shift"))
    g.node.append(helper.make_node("Add", ["eos_logit_raw", "eos_shift"], ["eos_logit"], name="eos_shift_add"))
    onnx.checker.check_model(m)
    onnx.save(m, os.path.join(dst, name))

def patch_encoder(name, bos):
    m = onnx.load(os.path.join(src, name))
    g = m.graph
    for n in g.node:
        for i, o in enumerate(n.output):
            if o == "latents":
                n.output[i] = "latents_raw"
    g.initializer.append(numpy_helper.from_array(bos.astype(np.float32), "bos_before_voice_const"))
    g.node.append(helper.make_node("Concat", ["bos_before_voice_const", "latents_raw"], ["latents"], axis=1, name="bos_concat"))
    # длина выхода стала на 1 больше — делаем ось безымянной динамической
    d = g.output[0].type.tensor_type.shape.dim[1]
    d.ClearField("dim_value"); d.dim_param = "voice_len"
    onnx.checker.check_model(m)
    onnx.save(m, os.path.join(dst, name))

bos = np.load(os.path.join(src, "bos_before_voice.npy"))
for f in ("flow_lm_main.onnx", "flow_lm_main_int8.onnx"):
    if os.path.exists(os.path.join(src, f)): patch_lm_main(f); print("eos сдвинут:", f)
for f in ("mimi_encoder.onnx", "mimi_encoder_int8.onnx", "flow_lm_flow.onnx", "flow_lm_flow_int8.onnx", "mimi_decoder.onnx", "mimi_decoder_int8.onnx",
          "text_conditioner.onnx", "text_conditioner_int8.onnx", "vocab.json", "token_scores.json", "tokenizer.model"):
    if os.path.exists(os.path.join(src, f)): shutil.copy2(os.path.join(src, f), os.path.join(dst, f))
print("готово:", dst)
