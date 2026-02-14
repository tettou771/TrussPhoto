#!/usr/bin/env python3
"""Check SigLIP2 forward() logits vs raw cosine."""

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

texts = ["猫", "cat", "風景", "夜"]
enc = tokenizer(texts, padding="max_length", max_length=64,
                truncation=True, return_tensors="pt")
ids = enc["input_ids"]
pad_id = tokenizer.pad_token_id or 0
mask = (ids != pad_id).long()

with torch.no_grad():
    outputs = model(pixel_values=img_input["pixel_values"],
                    input_ids=ids, attention_mask=mask)

print("Output keys:", list(outputs.keys()))
print()

if hasattr(outputs, "logits_per_image"):
    logits = outputs.logits_per_image
    print("logits_per_image (rows=images, cols=texts):")
    print(f"  Images: [cat, noncat]")
    print(f"  Texts:  {texts}")
    for i, img_name in enumerate(["cat_img", "noncat_img"]):
        row = logits[i].numpy()
        print(f"  {img_name}: {row}")
    print()

    # Sigmoid probabilities
    probs = torch.sigmoid(logits)
    print("sigmoid(logits):")
    for i, img_name in enumerate(["cat_img", "noncat_img"]):
        row = probs[i].numpy()
        print(f"  {img_name}: {row}")

print()
print(f"logit_scale: {model.logit_scale.item():.4f}")
print(f"logit_bias: {model.logit_bias.item():.4f}")
print(f"exp(logit_scale) = temperature: {torch.exp(model.logit_scale).item():.4f}")
print()

# Compare: raw cosine vs scaled logits
print("=== Raw cosine (what we're using now) ===")
with torch.no_grad():
    img_out = model.get_image_features(**img_input)
    img_emb = img_out.pooler_output if hasattr(img_out, "pooler_output") else img_out
    img_emb = torch.nn.functional.normalize(img_emb, p=2, dim=1)

    txt_out = model.get_text_features(input_ids=ids, attention_mask=mask)
    txt_emb = txt_out.pooler_output if hasattr(txt_out, "pooler_output") else txt_out
    txt_emb = torch.nn.functional.normalize(txt_emb, p=2, dim=1)

raw_cos = (img_emb @ txt_emb.T).numpy()
print(f"  Images: [cat, noncat]")
print(f"  Texts:  {texts}")
for i, img_name in enumerate(["cat_img", "noncat_img"]):
    print(f"  {img_name}: {raw_cos[i]}")
