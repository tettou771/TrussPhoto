#!/usr/bin/env python3
"""Quick test: SigLIP2 embedding quality on actual thumbnails."""

import numpy as np
import onnxruntime as ort
from PIL import Image
import os, glob

models_dir = os.path.expanduser("~/Library/Application Support/TrussPhoto/models")
thumb_dir = "/Users/toru/Nextcloud/Make/TrussC/testPictures/thumbnail_cache"

vision = ort.InferenceSession(os.path.join(models_dir, "waon-siglip2-vision.onnx"))
text_sess = ort.InferenceSession(os.path.join(models_dir, "waon-siglip2-text.onnx"))

from transformers import AutoTokenizer
tokenizer = AutoTokenizer.from_pretrained("llm-jp/waon-siglip2-base-patch16-256")

def embed_image(path):
    img = Image.open(path).convert("RGB")
    img_resized = img.resize((256, 256), Image.BILINEAR)
    arr = np.array(img_resized, dtype=np.float32) / 255.0
    arr = (arr - 0.5) / 0.5
    arr = arr.transpose(2, 0, 1)[np.newaxis, ...]
    emb = vision.run(["image_embeds"], {"pixel_values": arr})[0][0]
    return emb / np.linalg.norm(emb)

def embed_text(t):
    enc = tokenizer([t], padding="max_length", max_length=64,
                    truncation=True, return_tensors="np")
    ids = enc["input_ids"].astype(np.int64)
    pad_id = tokenizer.pad_token_id or 0
    mask = (ids != pad_id).astype(np.int64)
    emb = text_sess.run(["text_embeds"], {"input_ids": ids, "attention_mask": mask})[0][0]
    return emb / np.linalg.norm(emb)

# Find all thumbnails
all_thumbs = sorted(glob.glob(thumb_dir + "/**/*.jpg", recursive=True))
print(f"Found {len(all_thumbs)} thumbnails")

queries = ["猫", "人", "風景", "夜景", "花"]
for q in queries:
    t_emb = embed_text(q)
    scores = []
    for tp in all_thumbs:
        fname = os.path.basename(tp)
        i_emb = embed_image(tp)
        has_nan = np.any(np.isnan(i_emb))
        score = float(np.dot(t_emb, i_emb))
        scores.append((fname, score, has_nan))

    scores.sort(key=lambda x: x[1], reverse=True)
    print(f"\nQuery: '{q}'")
    for fname, score, has_nan in scores[:10]:
        nan_flag = " [NaN!]" if has_nan else ""
        print(f"  {score:+.4f}  {fname}{nan_flag}")
