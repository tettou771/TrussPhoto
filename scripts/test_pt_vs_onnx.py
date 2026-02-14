#!/usr/bin/env python3
"""Compare PyTorch vs ONNX embedding on actual cat thumbnail."""

import numpy as np
import torch
import os
import glob
from PIL import Image
from transformers import AutoModel, AutoImageProcessor, AutoTokenizer
import onnxruntime as ort

model = AutoModel.from_pretrained("llm-jp/waon-siglip2-base-patch16-256")
model.eval()
processor = AutoImageProcessor.from_pretrained("llm-jp/waon-siglip2-base-patch16-256")
tokenizer = AutoTokenizer.from_pretrained("llm-jp/waon-siglip2-base-patch16-256")

models_dir = os.path.expanduser("~/Library/Application Support/TrussPhoto/models")
ort_vision = ort.InferenceSession(os.path.join(models_dir, "waon-siglip2-vision.onnx"))
ort_text = ort.InferenceSession(os.path.join(models_dir, "waon-siglip2-text.onnx"))

# Load actual cat thumbnail
thumbs = glob.glob(
    "/Users/toru/Nextcloud/Make/TrussC/testPictures/thumbnail_cache/**/DSC05106*",
    recursive=True,
)
img = Image.open(thumbs[0]).convert("RGB")
print(f"Image: {thumbs[0]}, size={img.size}")

# PyTorch path (via official processor)
pt_input = processor(images=img, return_tensors="pt")
with torch.no_grad():
    pt_out = model.get_image_features(**pt_input)
    pt_emb = pt_out.pooler_output if hasattr(pt_out, "pooler_output") else pt_out
    pt_emb = torch.nn.functional.normalize(pt_emb, p=2, dim=1).numpy()[0]

# ONNX path (via same processor input)
ort_emb = ort_vision.run(
    ["image_embeds"], {"pixel_values": pt_input["pixel_values"].numpy()}
)[0][0]

cos = np.dot(pt_emb, ort_emb) / (np.linalg.norm(pt_emb) * np.linalg.norm(ort_emb))
print(f"PyTorch vs ONNX (same input): cosine={cos:.6f}")
print()

# Image-text matching comparison
queries = ["猫", "犬", "風景", "人", "カメラ"]
for q in queries:
    enc = tokenizer(
        [q], padding="max_length", max_length=64,
        truncation=True, return_tensors="pt",
    )
    ids = enc["input_ids"]
    pad_id = tokenizer.pad_token_id or 0
    mask = (ids != pad_id).long()

    with torch.no_grad():
        txt_out = model.get_text_features(input_ids=ids, attention_mask=mask)
        txt_emb = txt_out.pooler_output if hasattr(txt_out, "pooler_output") else txt_out
        txt_emb = torch.nn.functional.normalize(txt_emb, p=2, dim=1).numpy()[0]

    # ONNX text
    ort_txt = ort_text.run(["text_embeds"], {
        "input_ids": ids.numpy().astype(np.int64),
        "attention_mask": mask.numpy().astype(np.int64),
    })[0][0]

    pt_score = float(np.dot(pt_emb, txt_emb))
    ort_score = float(np.dot(ort_emb, ort_txt))
    print(f'  "{q}": PT_img*PT_txt={pt_score:.4f}, ORT_img*ORT_txt={ort_score:.4f}')
