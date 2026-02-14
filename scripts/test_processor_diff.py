#!/usr/bin/env python3
"""Find the difference between AutoProcessor and separate tokenizer+image_processor."""

import torch
import numpy as np
from PIL import Image
from transformers import AutoProcessor, AutoTokenizer, AutoImageProcessor, AutoModel
import glob

ckpt = "llm-jp/waon-siglip2-base-patch16-256"
model = AutoModel.from_pretrained(ckpt)
proc = AutoProcessor.from_pretrained(ckpt)
tokenizer = AutoTokenizer.from_pretrained(ckpt)
img_proc = AutoImageProcessor.from_pretrained(ckpt)

cat_path = glob.glob("/Users/toru/Nextcloud/Make/TrussC/testPictures/thumbnail_cache/**/DSC05106*", recursive=True)[0]
img = Image.open(cat_path).convert("RGB")
text = "猫"

# Method 1: AutoProcessor (works correctly)
inputs_proc = proc(text=[text], images=img, padding="max_length", max_length=64, return_tensors="pt")

# Method 2: Separate tokenizer + image processor (what we used before)
img_inputs = img_proc(images=img, return_tensors="pt")
txt_inputs = tokenizer([text], padding="max_length", max_length=64, truncation=True, return_tensors="pt")
pad_id = tokenizer.pad_token_id or 0

print("=== AutoProcessor outputs ===")
for k, v in inputs_proc.items():
    if isinstance(v, torch.Tensor):
        print(f"  {k}: shape={v.shape}, dtype={v.dtype}")
        if "ids" in k or "mask" in k:
            print(f"    values: {v[0][:20].tolist()}")

print("\n=== Separate tokenizer output ===")
for k, v in txt_inputs.items():
    if isinstance(v, torch.Tensor):
        print(f"  {k}: shape={v.shape}, dtype={v.dtype}")
        if "ids" in k or "mask" in k:
            print(f"    values: {v[0][:20].tolist()}")

print("\n=== Image processor output ===")
for k, v in img_inputs.items():
    if isinstance(v, torch.Tensor):
        print(f"  {k}: shape={v.shape}, dtype={v.dtype}")

# Compare pixel values
if "pixel_values" in inputs_proc and "pixel_values" in img_inputs:
    pv1 = inputs_proc["pixel_values"]
    pv2 = img_inputs["pixel_values"]
    diff = (pv1 - pv2).abs().max().item()
    print(f"\nPixel values max diff: {diff}")

# Compare input_ids
if "input_ids" in inputs_proc and "input_ids" in txt_inputs:
    ids1 = inputs_proc["input_ids"][0].tolist()
    ids2 = txt_inputs["input_ids"][0].tolist()
    print(f"\nAutoProcessor input_ids: {ids1[:20]}...")
    print(f"Tokenizer input_ids:     {ids2[:20]}...")
    print(f"Same: {ids1 == ids2}")

# Compare attention_mask
if "attention_mask" in inputs_proc:
    mask1 = inputs_proc["attention_mask"][0].tolist()
    print(f"\nAutoProcessor attention_mask: {mask1[:20]}...")
    if "attention_mask" in txt_inputs:
        mask2 = txt_inputs["attention_mask"][0].tolist()
        print(f"Tokenizer attention_mask:     {mask2[:20]}...")
    else:
        print("Tokenizer has NO attention_mask!")
        # Build one manually
        manual_mask = [1 if x != pad_id else 0 for x in ids2]
        print(f"Manual attention_mask:        {manual_mask[:20]}...")
        print(f"Same as AutoProcessor: {mask1 == manual_mask}")
