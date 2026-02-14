#!/usr/bin/env python3
"""Check which embedding to use: pooler_output vs model.forward() embeds."""

import torch
import numpy as np
from transformers import AutoModel, AutoImageProcessor, AutoTokenizer
from PIL import Image
import glob

model = AutoModel.from_pretrained("llm-jp/waon-siglip2-base-patch16-256")
model.eval()
processor = AutoImageProcessor.from_pretrained("llm-jp/waon-siglip2-base-patch16-256")
tokenizer = AutoTokenizer.from_pretrained("llm-jp/waon-siglip2-base-patch16-256")

cat_path = glob.glob("/Users/toru/Nextcloud/Make/TrussC/testPictures/thumbnail_cache/**/DSC05106*", recursive=True)[0]
noncat_path = glob.glob("/Users/toru/Nextcloud/Make/TrussC/testPictures/thumbnail_cache/**/DSC06157*", recursive=True)[0]

cat_img = Image.open(cat_path).convert("RGB")
noncat_img = Image.open(noncat_path).convert("RGB")

img_input = processor(images=[cat_img, noncat_img], return_tensors="pt")

texts = ["猫", "cat", "風景"]
enc = tokenizer(texts, padding="max_length", max_length=64,
                truncation=True, return_tensors="pt")
ids = enc["input_ids"]
pad_id = tokenizer.pad_token_id or 0
mask = (ids != pad_id).long()

with torch.no_grad():
    # Method 1: model.forward() — returns image_embeds/text_embeds (used for logits)
    outputs = model(pixel_values=img_input["pixel_values"],
                    input_ids=ids, attention_mask=mask)
    fwd_img = outputs.image_embeds  # what forward() uses for logits
    fwd_txt = outputs.text_embeds

    # Method 2: get_image_features / get_text_features — returns pooler_output
    gf_img_out = model.get_image_features(**img_input)
    gf_img = gf_img_out.pooler_output if hasattr(gf_img_out, "pooler_output") else gf_img_out
    gf_txt_out = model.get_text_features(input_ids=ids, attention_mask=mask)
    gf_txt = gf_txt_out.pooler_output if hasattr(gf_txt_out, "pooler_output") else gf_txt_out

print("=== Shapes ===")
print(f"forward() image_embeds: {fwd_img.shape}")
print(f"forward() text_embeds:  {fwd_txt.shape}")
print(f"get_features pooler_output img: {gf_img.shape}")
print(f"get_features pooler_output txt: {gf_txt.shape}")
print()

# Are they the same?
print("=== Are they the same tensor? ===")
cos_img = torch.nn.functional.cosine_similarity(fwd_img, gf_img, dim=1)
cos_txt = torch.nn.functional.cosine_similarity(fwd_txt, gf_txt, dim=1)
print(f"forward.image_embeds vs pooler_output img: cosine={cos_img.numpy()}")
print(f"forward.text_embeds vs pooler_output txt:  cosine={cos_txt.numpy()}")
print()

# Compare retrieval quality
fwd_img_n = torch.nn.functional.normalize(fwd_img, p=2, dim=1)
fwd_txt_n = torch.nn.functional.normalize(fwd_txt, p=2, dim=1)
gf_img_n = torch.nn.functional.normalize(gf_img, p=2, dim=1)
gf_txt_n = torch.nn.functional.normalize(gf_txt, p=2, dim=1)

print("=== Cosine similarity (using forward() embeds) ===")
scores = (fwd_img_n @ fwd_txt_n.T).numpy()
for i, name in enumerate(["cat_img", "noncat_img"]):
    for j, t in enumerate(texts):
        print(f'  {name} vs "{t}": {scores[i][j]:.4f}')

print()
print("=== Cosine similarity (using pooler_output) ===")
scores2 = (gf_img_n @ gf_txt_n.T).numpy()
for i, name in enumerate(["cat_img", "noncat_img"]):
    for j, t in enumerate(texts):
        print(f'  {name} vs "{t}": {scores2[i][j]:.4f}')

print()
print("=== Norms (pre-normalize) ===")
print(f"forward img norms: {torch.norm(fwd_img, dim=1).numpy()}")
print(f"forward txt norms: {torch.norm(fwd_txt, dim=1).numpy()}")
print(f"pooler img norms:  {torch.norm(gf_img, dim=1).numpy()}")
print(f"pooler txt norms:  {torch.norm(gf_txt, dim=1).numpy()}")
