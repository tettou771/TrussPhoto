#!/usr/bin/env python3
"""The key question: does AutoProcessor output attention_mask?"""

import torch
from transformers import AutoProcessor, AutoModel
from PIL import Image
import glob

ckpt = "llm-jp/waon-siglip2-base-patch16-256"
model = AutoModel.from_pretrained(ckpt)
proc = AutoProcessor.from_pretrained(ckpt)

cat_path = glob.glob("/Users/toru/Nextcloud/Make/TrussC/testPictures/thumbnail_cache/**/DSC05106*", recursive=True)[0]
img = Image.open(cat_path).convert("RGB")

inputs = proc(text=["猫"], images=img, padding="max_length", max_length=64, return_tensors="pt")

print("All keys from AutoProcessor:", list(inputs.keys()))
print()

# Does model.forward() create attention_mask internally if not provided?
# Test: pass with and without attention_mask
ids = inputs["input_ids"]
pv = inputs["pixel_values"]

# Without attention_mask
with torch.no_grad():
    out_no_mask = model(pixel_values=pv, input_ids=ids)
    logit_no_mask = out_no_mask.logits_per_image[0][0].item()

# With attention_mask = all ones
mask_ones = torch.ones_like(ids)
with torch.no_grad():
    out_all_ones = model(pixel_values=pv, input_ids=ids, attention_mask=mask_ones)
    logit_all_ones = out_all_ones.logits_per_image[0][0].item()

# With attention_mask = correct (1 for non-pad, 0 for pad)
pad_id = 0
mask_correct = (ids != pad_id).long()
with torch.no_grad():
    out_correct = model(pixel_values=pv, input_ids=ids, attention_mask=mask_correct)
    logit_correct = out_correct.logits_per_image[0][0].item()

print(f"No attention_mask:       logit={logit_no_mask:.4f}")
print(f"attention_mask=all_ones: logit={logit_all_ones:.4f}")
print(f"attention_mask=correct:  logit={logit_correct:.4f}")
print()
print(f"input_ids: {ids[0][:10].tolist()}")
print(f"mask_correct: {mask_correct[0][:10].tolist()}")
print(f"mask_ones:    {mask_ones[0][:10].tolist()}")
