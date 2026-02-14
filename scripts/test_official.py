#!/usr/bin/env python3
"""Test using the exact official usage from the model card."""

import torch
import numpy as np
from PIL import Image
from transformers import AutoProcessor, AutoModel
import glob

ckpt = "llm-jp/waon-siglip2-base-patch16-256"
model = AutoModel.from_pretrained(ckpt)
processor = AutoProcessor.from_pretrained(ckpt)

cat_path = glob.glob("/Users/toru/Nextcloud/Make/TrussC/testPictures/thumbnail_cache/**/DSC05106*", recursive=True)[0]
noncat_path = glob.glob("/Users/toru/Nextcloud/Make/TrussC/testPictures/thumbnail_cache/**/DSC06157*", recursive=True)[0]
cat2_path = glob.glob("/Users/toru/Nextcloud/Make/TrussC/testPictures/thumbnail_cache/**/7C207125*", recursive=True)[0]

cat_img = Image.open(cat_path).convert("RGB")
noncat_img = Image.open(noncat_path).convert("RGB")
cat2_img = Image.open(cat2_path).convert("RGB")

labels = ["猫", "犬", "風景", "夜景", "プロジェクションマッピング", "実験室"]

print("=== Official usage (AutoProcessor, model(**inputs)) ===")
for img, name in [(cat_img, "cat1(DSC05106)"),
                   (cat2_img, "cat2(7C207125)"),
                   (noncat_img, "noncat(DSC06157)")]:
    inputs = processor(
        text=labels, images=img,
        padding="max_length", max_length=64,
        return_tensors="pt",
    ).to(model.device)

    with torch.no_grad():
        outputs = model(**inputs)

    logits = outputs.logits_per_image
    probs = torch.sigmoid(logits)

    print(f"\n{name}:")
    print(f"  logits: {logits[0].numpy()}")
    print(f"  probs:  {probs[0].numpy()}")
    for i, label in enumerate(labels):
        print(f"    '{label}': logit={logits[0][i]:.2f}, prob={probs[0][i]:.4f}")

# Now compare: same texts, which image gets highest prob for "猫"?
print("\n=== Ranking for '猫' across images ===")
for img, name in [(cat_img, "cat1"), (cat2_img, "cat2"), (noncat_img, "noncat")]:
    inputs = processor(
        text=["猫"], images=img,
        padding="max_length", max_length=64,
        return_tensors="pt",
    )
    with torch.no_grad():
        out = model(**inputs)
    logit = out.logits_per_image[0][0].item()
    prob = torch.sigmoid(out.logits_per_image)[0][0].item()
    print(f"  {name}: logit={logit:.2f}, prob={prob:.6f}")
