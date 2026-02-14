#!/usr/bin/env python3
"""
Export waon-siglip2-base-patch16-256 to ONNX (vision + text).

Model: llm-jp/waon-siglip2-base-patch16-256
  - Vision: SigLIP2 ViT-B/16 (256px) -> 768-dim
  - Text:   GemmaTokenizer (SentencePiece, 256k vocab) -> 768-dim
  - Both outputs are L2-normalized and share the same embedding space
  - 109 languages, Apache 2.0 license
  - XM3600 retrieval: 73.75

Usage:
  pip install transformers torch sentencepiece protobuf onnx onnxruntime pillow
  python scripts/export_siglip2.py

Output (in ~/Library/Application Support/TrussPhoto/models/):
  waon-siglip2-vision.onnx   - Vision encoder (~370MB)
  waon-siglip2-text.onnx     - Text encoder (~1.1GB)
  waon-siglip2-spiece.model  - SentencePiece tokenizer model
"""

import os
import sys
import shutil
import platform
import glob

import torch
import torch.nn as nn
import numpy as np
from PIL import Image
from transformers import AutoModel, AutoTokenizer, AutoImageProcessor
import onnx


def get_models_dir():
    """Get TrussPhoto models directory (OS-dependent)."""
    system = platform.system()
    if system == "Darwin":
        base = os.path.expanduser("~/Library/Application Support/TrussPhoto")
    elif system == "Windows":
        base = os.path.join(os.environ.get("APPDATA", ""), "TrussPhoto")
    else:
        base = os.path.expanduser("~/.local/share/TrussPhoto")
    return os.path.join(base, "models")


MODEL_PATH = "llm-jp/waon-siglip2-base-patch16-256"
MAX_SEQ_LEN = 64
INPUT_SIZE = 256


# --- Vision wrapper ---

class VisionWrapper(nn.Module):
    """Wraps model.get_image_features() for ONNX export."""
    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, pixel_values):
        # SiglipModel returns BaseModelOutputWithPooling, extract pooler_output
        out = self.model.get_image_features(pixel_values=pixel_values)
        features = out.pooler_output if hasattr(out, 'pooler_output') else out
        # L2 normalize
        return torch.nn.functional.normalize(features, p=2, dim=1)


# --- Text wrapper ---

class TextWrapper(nn.Module):
    """Wraps model.get_text_features() for ONNX export."""
    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, input_ids, attention_mask):
        # SiglipModel returns BaseModelOutputWithPooling, extract pooler_output
        out = self.model.get_text_features(
            input_ids=input_ids,
            attention_mask=attention_mask,
        )
        features = out.pooler_output if hasattr(out, 'pooler_output') else out
        # L2 normalize
        return torch.nn.functional.normalize(features, p=2, dim=1)


def find_spiece_model(tokenizer):
    """Find spiece.model from tokenizer's cached files."""
    # Try vocab_file attribute
    if hasattr(tokenizer, 'vocab_file') and tokenizer.vocab_file:
        if os.path.exists(tokenizer.vocab_file):
            return tokenizer.vocab_file

    # Search in HF cache for this model
    from huggingface_hub import hf_hub_download
    try:
        path = hf_hub_download(MODEL_PATH, "spiece.model")
        if os.path.exists(path):
            return path
    except Exception:
        pass

    # Try save_pretrained and look for .model files
    if hasattr(tokenizer, 'name_or_path'):
        cache_dir = tokenizer.name_or_path
        for pattern in ["**/spiece.model", "spiece.model",
                        "**/*.model", "*.model"]:
            matches = glob.glob(os.path.join(cache_dir, pattern), recursive=True)
            if matches:
                return matches[0]

    return None


def merge_external_data(onnx_path):
    """Merge .onnx.data external weights into single ONNX file."""
    ext_data = onnx_path + ".data"
    if os.path.exists(ext_data):
        print("  Merging external weights into ONNX file...")
        model_onnx = onnx.load(onnx_path, load_external_data=True)
        onnx.save_model(model_onnx, onnx_path, save_as_external_data=False)
        os.remove(ext_data)


def main():
    models_dir = get_models_dir()
    os.makedirs(models_dir, exist_ok=True)

    vision_onnx_path = os.path.join(models_dir, "waon-siglip2-vision.onnx")
    text_onnx_path = os.path.join(models_dir, "waon-siglip2-text.onnx")
    spiece_path = os.path.join(models_dir, "waon-siglip2-spiece.model")

    print(f"Output directory: {models_dir}")
    print()

    # --- Load model ---
    print(f"Loading {MODEL_PATH} ...")

    # SigLIP2 uses standard transformers SiglipModel (no trust_remote_code needed)
    model = AutoModel.from_pretrained(MODEL_PATH)
    model.eval()
    tokenizer = AutoTokenizer.from_pretrained(MODEL_PATH)
    processor = AutoImageProcessor.from_pretrained(MODEL_PATH)
    print(f"  Model loaded successfully")
    print(f"  Model type: {type(model).__name__}")
    print(f"  Tokenizer type: {type(tokenizer).__name__}")
    print()

    # --- Check dimensions ---
    dummy_image = processor(
        images=Image.new("RGB", (INPUT_SIZE, INPUT_SIZE)),
        return_tensors="pt",
    )
    dummy_text = tokenizer(
        ["test"], padding="max_length",
        max_length=MAX_SEQ_LEN, truncation=True,
        return_tensors="pt",
    )
    with torch.no_grad():
        img_out = model.get_image_features(**dummy_image)
        txt_out = model.get_text_features(**dummy_text)
        img_feat = img_out.pooler_output if hasattr(img_out, 'pooler_output') else img_out
        txt_feat = txt_out.pooler_output if hasattr(txt_out, 'pooler_output') else txt_out
    print(f"Image feature shape: {img_feat.shape}")
    print(f"Text feature shape:  {txt_feat.shape}")
    embed_dim = img_feat.shape[1]
    print(f"Embedding dimension: {embed_dim}")
    print()

    # --- Check preprocessor config ---
    print(f"Image preprocessor config:")
    if hasattr(processor, 'image_mean'):
        print(f"  image_mean: {processor.image_mean}")
        print(f"  image_std:  {processor.image_std}")
    if hasattr(processor, 'size'):
        print(f"  size:       {processor.size}")
    if hasattr(processor, 'crop_size'):
        print(f"  crop_size:  {processor.crop_size}")
    if hasattr(processor, 'rescale_factor'):
        print(f"  rescale_factor: {processor.rescale_factor}")
    print()

    # --- Check text input structure ---
    print(f"Text input keys: {list(dummy_text.keys())}")
    for k, v in dummy_text.items():
        print(f"  {k}: shape={v.shape}, dtype={v.dtype}")
    print()

    # --- Print tokenizer info ---
    print(f"Tokenizer info:")
    print(f"  vocab_size:    {tokenizer.vocab_size}")
    for attr in ['bos_token_id', 'eos_token_id', 'pad_token_id', 'unk_token_id']:
        val = getattr(tokenizer, attr, None)
        print(f"  {attr}: {val}")
    print()

    # --- Export Vision ONNX ---
    print(f"Exporting vision encoder: {vision_onnx_path}")
    vision_wrapper = VisionWrapper(model)
    vision_wrapper.eval()

    dummy_pixels = torch.randn(1, 3, INPUT_SIZE, INPUT_SIZE)
    with torch.no_grad():
        test_out = vision_wrapper(dummy_pixels)
    print(f"  Test output shape: {test_out.shape}, norm: {torch.norm(test_out, dim=1).item():.4f}")

    # dynamo=False forces legacy TorchScript exporter which respects opset_version.
    # PyTorch 2.6+ default (dynamo) ignores opset_version and emits opset 18 with
    # LayerNormalization op, which causes NaN on some ONNX Runtime C++ builds.
    torch.onnx.export(
        vision_wrapper,
        (dummy_pixels,),
        vision_onnx_path,
        input_names=["pixel_values"],
        output_names=["image_embeds"],
        dynamic_axes={
            "pixel_values": {0: "batch"},
            "image_embeds": {0: "batch"},
        },
        opset_version=14,
        do_constant_folding=True,
        dynamo=False,
    )
    merge_external_data(vision_onnx_path)

    vision_size_mb = os.path.getsize(vision_onnx_path) / (1024 * 1024)
    print(f"  Vision ONNX size: {vision_size_mb:.1f} MB")
    print()

    # --- Export Text ONNX ---
    print(f"Exporting text encoder: {text_onnx_path}")
    text_wrapper = TextWrapper(model)
    text_wrapper.eval()

    dummy_ids = torch.ones(1, MAX_SEQ_LEN, dtype=torch.long)
    dummy_mask = torch.ones(1, MAX_SEQ_LEN, dtype=torch.long)

    with torch.no_grad():
        test_out = text_wrapper(dummy_ids, dummy_mask)
    print(f"  Test output shape: {test_out.shape}, norm: {torch.norm(test_out, dim=1).item():.4f}")

    torch.onnx.export(
        text_wrapper,
        (dummy_ids, dummy_mask),
        text_onnx_path,
        input_names=["input_ids", "attention_mask"],
        output_names=["text_embeds"],
        dynamic_axes={
            "input_ids": {0: "batch", 1: "seq_len"},
            "attention_mask": {0: "batch", 1: "seq_len"},
            "text_embeds": {0: "batch"},
        },
        opset_version=14,
        do_constant_folding=True,
        dynamo=False,
    )
    merge_external_data(text_onnx_path)

    text_size_mb = os.path.getsize(text_onnx_path) / (1024 * 1024)
    print(f"  Text ONNX size: {text_size_mb:.1f} MB")
    print()

    # --- Copy SentencePiece model ---
    print(f"Extracting SentencePiece model...")
    src = find_spiece_model(tokenizer)
    if src is None:
        print("  Trying save_pretrained to extract...")
        import tempfile
        with tempfile.TemporaryDirectory() as tmpdir:
            tokenizer.save_pretrained(tmpdir)
            candidates = glob.glob(os.path.join(tmpdir, "**/*spiece*"), recursive=True)
            if not candidates:
                candidates = glob.glob(os.path.join(tmpdir, "**/*.model"), recursive=True)
            if candidates:
                src = candidates[0]
                shutil.copy2(src, spiece_path)
                print(f"  Extracted from save_pretrained: {os.path.basename(src)}")
            else:
                print("  ERROR: Could not extract SentencePiece model")
                sys.exit(1)
    else:
        shutil.copy2(src, spiece_path)
        print(f"  Source: {src}")

    print(f"  Dest:   {spiece_path}")
    spiece_size_kb = os.path.getsize(spiece_path) / 1024
    print(f"  Size:   {spiece_size_kb:.0f} KB")
    print()

    # --- Verify with ONNX Runtime ---
    print("Verifying with ONNX Runtime ...")
    import onnxruntime as ort

    # Vision
    vision_session = ort.InferenceSession(vision_onnx_path)
    print(f"  Vision inputs:  {[(i.name, i.shape) for i in vision_session.get_inputs()]}")
    print(f"  Vision outputs: {[(o.name, o.shape) for o in vision_session.get_outputs()]}")

    # Text
    text_session = ort.InferenceSession(text_onnx_path)
    print(f"  Text inputs:    {[(i.name, i.shape) for i in text_session.get_inputs()]}")
    print(f"  Text outputs:   {[(o.name, o.shape) for o in text_session.get_outputs()]}")
    print()

    # --- Cross-validation: PyTorch vs ONNX ---
    print("Cross-validation (PyTorch vs ONNX):")

    # Vision cross-check
    test_image = Image.new("RGB", (300, 200), color=(128, 64, 32))
    image_inputs = processor(images=test_image, return_tensors="pt")
    with torch.no_grad():
        pt_img_out = model.get_image_features(**image_inputs)
        pt_img_feat = pt_img_out.pooler_output if hasattr(pt_img_out, 'pooler_output') else pt_img_out
        pt_img_feat = torch.nn.functional.normalize(pt_img_feat, p=2, dim=1).numpy()

    ort_img_result = vision_session.run(
        ["image_embeds"],
        {"pixel_values": image_inputs["pixel_values"].numpy()},
    )
    ort_img_feat = ort_img_result[0]
    cos_sim = np.dot(pt_img_feat[0], ort_img_feat[0]) / (
        np.linalg.norm(pt_img_feat[0]) * np.linalg.norm(ort_img_feat[0])
    )
    print(f"  Vision: cosine_similarity = {cos_sim:.6f}")

    # Text cross-check
    test_texts = ["猫の写真", "子供が遊んでいる", "a photo of a cat", "夕焼けの海"]
    for text in test_texts:
        text_inputs = tokenizer(
            [text], padding="max_length",
            max_length=MAX_SEQ_LEN, truncation=True,
            return_tensors="pt",
        )
        # Build attention_mask from input_ids (1 for non-PAD, 0 for PAD)
        ids_tensor = text_inputs["input_ids"]
        mask_tensor = (ids_tensor != tokenizer.pad_token_id).long()

        with torch.no_grad():
            pt_txt_out = model.get_text_features(
                input_ids=ids_tensor, attention_mask=mask_tensor)
            pt_txt_feat = pt_txt_out.pooler_output if hasattr(pt_txt_out, 'pooler_output') else pt_txt_out
            pt_txt_feat = torch.nn.functional.normalize(pt_txt_feat, p=2, dim=1).numpy()

        ort_feed = {
            "input_ids": ids_tensor.numpy().astype(np.int64),
            "attention_mask": mask_tensor.numpy().astype(np.int64),
        }
        ort_txt_result = text_session.run(["text_embeds"], ort_feed)
        ort_txt_feat = ort_txt_result[0]
        cos_sim = np.dot(pt_txt_feat[0], ort_txt_feat[0]) / (
            np.linalg.norm(pt_txt_feat[0]) * np.linalg.norm(ort_txt_feat[0])
        )
        print(f"  Text '{text}': cosine_similarity = {cos_sim:.6f}")

    # --- Text embedding quality check ---
    print()
    print("Text embedding quality check (cosine between text pairs):")
    def tokenize_with_mask(texts):
        """Tokenize and build attention_mask from pad_token_id."""
        enc = tokenizer(texts, padding="max_length", max_length=MAX_SEQ_LEN,
                         truncation=True, return_tensors="pt")
        ids = enc["input_ids"].numpy().astype(np.int64)
        mask = (ids != tokenizer.pad_token_id).astype(np.int64)
        return {"input_ids": ids, "attention_mask": mask}

    text_pairs = [
        ("猫", "犬"),
        ("猫", "カメラ"),
        ("猫", "夕焼け"),
        ("海の写真", "ビーチの風景"),
    ]
    for t1, t2 in text_pairs:
        f1 = text_session.run(["text_embeds"], tokenize_with_mask([t1]))[0][0]
        f2 = text_session.run(["text_embeds"], tokenize_with_mask([t2]))[0][0]
        cos = np.dot(f1, f2) / (np.linalg.norm(f1) * np.linalg.norm(f2))
        print(f"  '{t1}' vs '{t2}': {cos:.4f}")

    # --- Image-text matching test ---
    print()
    print("Image-text matching test:")
    test_img = Image.new("RGB", (INPUT_SIZE, INPUT_SIZE), color=(100, 150, 200))
    img_inputs = processor(images=test_img, return_tensors="pt")
    ort_img = vision_session.run(
        ["image_embeds"],
        {"pixel_values": img_inputs["pixel_values"].numpy()},
    )[0][0]

    for text in test_texts:
        ort_txt = text_session.run(
            ["text_embeds"], tokenize_with_mask([text]))[0][0]
        score = np.dot(ort_img, ort_txt)
        print(f"  '{text}': score = {score:.4f}")

    print()
    print("=" * 60)
    print("Done!")
    print(f"  Vision ONNX:   {vision_onnx_path} ({vision_size_mb:.1f} MB)")
    print(f"  Text ONNX:     {text_onnx_path} ({text_size_mb:.1f} MB)")
    print(f"  SentencePiece: {spiece_path} ({spiece_size_kb:.0f} KB)")
    print()
    print("C++ integration notes:")
    print(f"  Embedding dimension: {embed_dim}")
    print(f"  Input image size: {INPUT_SIZE}x{INPUT_SIZE}")
    if hasattr(processor, 'image_mean'):
        print(f"  Image normalization: mean={processor.image_mean}, std={processor.image_std}")
    for attr in ['bos_token_id', 'eos_token_id', 'pad_token_id', 'unk_token_id']:
        print(f"  {attr}: {getattr(tokenizer, attr, 'N/A')}")
    print(f"  Max sequence length: {MAX_SEQ_LEN}")
    print(f"  Text inputs: input_ids + attention_mask (2 inputs, no position_ids)")
    print(f"  SigLIP2 text format: [tokens..., EOS, PAD, PAD] (no CLS prefix)")
    print(f"  GemmaTokenizer: add_eos_token=True, do_lower_case=True")


if __name__ == "__main__":
    main()
