#!/usr/bin/env python3
"""Test retrieval: find cat photos with SigLIP2 (PyTorch direct, no ONNX)."""

import torch
import numpy as np
import os
import glob
from PIL import Image
from transformers import AutoModel, AutoImageProcessor, AutoTokenizer

model = AutoModel.from_pretrained("llm-jp/waon-siglip2-base-patch16-256")
model.eval()
processor = AutoImageProcessor.from_pretrained("llm-jp/waon-siglip2-base-patch16-256")
tokenizer = AutoTokenizer.from_pretrained("llm-jp/waon-siglip2-base-patch16-256")

thumb_dir = "/Users/toru/Nextcloud/Make/TrussC/testPictures/thumbnail_cache"
all_thumbs = sorted(glob.glob(thumb_dir + "/**/*.jpg", recursive=True))
print(f"Found {len(all_thumbs)} thumbnails")

# Embed all images via PyTorch
img_embs = {}
for tp in all_thumbs:
    fname = os.path.basename(tp).split("_")[0]  # strip ID suffix
    img = Image.open(tp).convert("RGB")
    inp = processor(images=img, return_tensors="pt")
    with torch.no_grad():
        out = model.get_image_features(**inp)
        emb = out.pooler_output if hasattr(out, "pooler_output") else out
        emb = torch.nn.functional.normalize(emb, p=2, dim=1).numpy()[0]
    img_embs[fname] = emb

# Search
queries = ["猫", "cat", "人物", "portrait", "夜", "花"]
for q in queries:
    enc = tokenizer([q], padding="max_length", max_length=64,
                    truncation=True, return_tensors="pt")
    ids = enc["input_ids"]
    pad_id = tokenizer.pad_token_id or 0
    mask = (ids != pad_id).long()
    with torch.no_grad():
        tout = model.get_text_features(input_ids=ids, attention_mask=mask)
        temb = tout.pooler_output if hasattr(tout, "pooler_output") else tout
        temb = torch.nn.functional.normalize(temb, p=2, dim=1).numpy()[0]

    scores = [(fname, float(np.dot(temb, emb))) for fname, emb in img_embs.items()]
    scores.sort(key=lambda x: x[1], reverse=True)
    print(f"\nQuery: '{q}'")
    for fname, score in scores[:10]:
        print(f"  {score:+.4f}  {fname}")
