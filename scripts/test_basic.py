#!/usr/bin/env python3
"""Basic sanity check for SigLIP2 embeddings."""

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

def embed_image(path):
    img = Image.open(path).convert("RGB")
    inp = processor(images=img, return_tensors="pt")
    with torch.no_grad():
        out = model.get_image_features(**inp)
        emb = out.pooler_output if hasattr(out, "pooler_output") else out
        return torch.nn.functional.normalize(emb, p=2, dim=1).numpy()[0]

def embed_text(t):
    enc = tokenizer([t], padding="max_length", max_length=64,
                    truncation=True, return_tensors="pt")
    ids = enc["input_ids"]
    pad_id = tokenizer.pad_token_id or 0
    mask = (ids != pad_id).long()
    with torch.no_grad():
        out = model.get_text_features(input_ids=ids, attention_mask=mask)
        emb = out.pooler_output if hasattr(out, "pooler_output") else out
        return torch.nn.functional.normalize(emb, p=2, dim=1).numpy()[0]

def cos(a, b):
    return float(np.dot(a, b))

# Find cat photos and a non-cat photo
cat1_paths = glob.glob(thumb_dir + "/**/DSC05106*", recursive=True)
cat2_paths = glob.glob(thumb_dir + "/**/7C207125*", recursive=True)
noncat_paths = glob.glob(thumb_dir + "/**/DSC06157*", recursive=True)  # top-ranked non-cat

print("=== File check ===")
print(f"Cat1 (DSC05106): {cat1_paths}")
print(f"Cat2 (7C207125): {cat2_paths}")
print(f"Non-cat (DSC06157): {noncat_paths}")
print()

# 1. Image-Image similarity
print("=== Image-Image similarity ===")
if cat1_paths and cat2_paths and noncat_paths:
    e_cat1 = embed_image(cat1_paths[0])
    e_cat2 = embed_image(cat2_paths[0])
    e_noncat = embed_image(noncat_paths[0])
    print(f"cat1 vs cat2:   {cos(e_cat1, e_cat2):.4f}")
    print(f"cat1 vs noncat: {cos(e_cat1, e_noncat):.4f}")
    print(f"cat2 vs noncat: {cos(e_cat2, e_noncat):.4f}")
elif cat1_paths and noncat_paths:
    e_cat1 = embed_image(cat1_paths[0])
    e_noncat = embed_image(noncat_paths[0])
    print(f"cat1 vs noncat: {cos(e_cat1, e_noncat):.4f}")
    print("(cat2 not found)")
print()

# 2. Text-Text similarity
print("=== Text-Text similarity ===")
texts = ["猫", "ねこ", "cat", "犬", "風景", "カメラ"]
t_embs = {t: embed_text(t) for t in texts}
for i, t1 in enumerate(texts):
    for t2 in texts[i+1:]:
        print(f'  "{t1}" vs "{t2}": {cos(t_embs[t1], t_embs[t2]):.4f}')
print()

# 3. Image-Text cross-modal similarity
print("=== Image-Text similarity ===")
for name, paths in [("cat1(DSC05106)", cat1_paths),
                     ("cat2(7C207125)", cat2_paths),
                     ("noncat(DSC06157)", noncat_paths)]:
    if not paths:
        print(f"{name}: not found")
        continue
    e_img = embed_image(paths[0])
    for t in ["猫", "ねこ", "cat", "犬", "風景", "カメラ"]:
        score = cos(e_img, t_embs[t])
        print(f"  {name} vs \"{t}\": {score:.4f}")
    print()

# 4. Quick sanity: use a downloaded cat image from the internet
print("=== Sanity check with synthetic image ===")
# Create a simple test: embed a pure orange image vs "orange"
orange_img = Image.new("RGB", (256, 256), (255, 165, 0))
e_orange = embed_image_pil(orange_img) if False else None
# Actually just use processor directly
inp = processor(images=Image.new("RGB", (256, 256), (255, 165, 0)), return_tensors="pt")
with torch.no_grad():
    out = model.get_image_features(**inp)
    e_orange = out.pooler_output if hasattr(out, "pooler_output") else out
    e_orange = torch.nn.functional.normalize(e_orange, p=2, dim=1).numpy()[0]

for t in ["orange", "オレンジ", "blue", "青", "cat", "猫"]:
    print(f'  orange_img vs "{t}": {cos(e_orange, t_embs.get(t, embed_text(t))):.4f}')
