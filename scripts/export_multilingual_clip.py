#!/usr/bin/env python3
"""
Export multilingual CLIP text encoder to ONNX.

Model: sentence-transformers/clip-ViT-B-32-multilingual-v1
  - DistilBERT multilingual base → MeanPooling → Dense(768→512) → L2Normalize
  - Produces 512-dim embeddings in the same space as CLIP ViT-B/32 vision

Usage:
  pip install sentence-transformers torch onnx onnxruntime
  python export_multilingual_clip.py

Output:
  ~/Library/Application Support/TrussPhoto/models/multilingual-clip-text.onnx
  ~/Library/Application Support/TrussPhoto/models/multilingual-vocab.txt
"""

import os
import sys
import shutil
import platform
import torch
import torch.nn as nn
import numpy as np
from sentence_transformers import SentenceTransformer


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


class MultilingualClipTextEncoder(nn.Module):
    """Wraps DistilBERT + MeanPooling + Dense + L2Normalize into a single module."""

    def __init__(self, model):
        super().__init__()
        # Extract components from SentenceTransformer
        self.transformer = model[0].auto_model  # DistilBERT
        self.dense_linear = model[2].linear  # Dense linear layer (768→512)
        self.dense_activation = model[2].activation_function  # identity or tanh

    def forward(self, input_ids, attention_mask):
        # 1. DistilBERT forward
        outputs = self.transformer(input_ids=input_ids, attention_mask=attention_mask)
        token_embeddings = outputs.last_hidden_state  # [B, seq_len, 768]

        # 2. Mean pooling (with attention mask)
        mask_expanded = attention_mask.unsqueeze(-1).expand(token_embeddings.size()).float()
        sum_embeddings = torch.sum(token_embeddings * mask_expanded, dim=1)
        sum_mask = torch.clamp(mask_expanded.sum(dim=1), min=1e-9)
        mean_pooled = sum_embeddings / sum_mask  # [B, 768]

        # 3. Dense projection (768 → 512)
        projected = self.dense_activation(self.dense_linear(mean_pooled))  # [B, 512]

        # 4. L2 normalize
        normalized = torch.nn.functional.normalize(projected, p=2, dim=1)

        return normalized


def main():
    models_dir = get_models_dir()
    os.makedirs(models_dir, exist_ok=True)

    onnx_path = os.path.join(models_dir, "multilingual-clip-text.onnx")
    vocab_path = os.path.join(models_dir, "multilingual-vocab.txt")

    print(f"Output directory: {models_dir}")
    print()

    # Load model
    print("Loading sentence-transformers/clip-ViT-B-32-multilingual-v1 ...")
    model = SentenceTransformer(
        "sentence-transformers/clip-ViT-B-32-multilingual-v1", device="cpu"
    )
    print(f"  Modules: {[type(m).__name__ for m in model]}")

    # Inspect model structure
    print(f"  Module 0 (Transformer): {type(model[0]).__name__}")
    print(f"  Module 1 (Pooling): {type(model[1]).__name__}")
    print(f"  Module 2 (Dense): {type(model[2]).__name__}")
    print(f"  Dense: {model[2].linear.in_features} → {model[2].linear.out_features}")
    print()

    # Create wrapper
    wrapper = MultilingualClipTextEncoder(model)
    wrapper.eval()

    # Dummy inputs (batch=1, seq_len=128)
    seq_len = 128
    dummy_input_ids = torch.ones(1, seq_len, dtype=torch.long)
    dummy_attention_mask = torch.ones(1, seq_len, dtype=torch.long)

    # Test forward pass
    with torch.no_grad():
        test_output = wrapper(dummy_input_ids, dummy_attention_mask)
    print(f"Test output shape: {test_output.shape}")
    print(f"Test output norm: {torch.norm(test_output, dim=1).item():.4f} (should be ~1.0)")
    print()

    # Export to ONNX
    print(f"Exporting to ONNX: {onnx_path}")
    torch.onnx.export(
        wrapper,
        (dummy_input_ids, dummy_attention_mask),
        onnx_path,
        input_names=["input_ids", "attention_mask"],
        output_names=["text_embeds"],
        dynamic_axes={
            "input_ids": {0: "batch", 1: "seq_len"},
            "attention_mask": {0: "batch", 1: "seq_len"},
            "text_embeds": {0: "batch"},
        },
        opset_version=14,
        do_constant_folding=True,
    )

    file_size_mb = os.path.getsize(onnx_path) / (1024 * 1024)
    print(f"  ONNX file size: {file_size_mb:.1f} MB")
    print()

    # Copy vocab.txt from the transformer
    tokenizer = model[0].tokenizer
    src_vocab = tokenizer.vocab_file  # path to vocab.txt in cache
    print(f"Copying vocab.txt: {src_vocab}")
    shutil.copy2(src_vocab, vocab_path)
    print(f"  → {vocab_path}")
    print()

    # Verify with ONNX Runtime
    print("Verifying with ONNX Runtime ...")
    import onnxruntime as ort

    session = ort.InferenceSession(onnx_path)
    print(f"  Inputs:  {[(i.name, i.shape) for i in session.get_inputs()]}")
    print(f"  Outputs: {[(o.name, o.shape) for o in session.get_outputs()]}")

    # Test with real text
    from transformers import AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(
        "sentence-transformers/clip-ViT-B-32-multilingual-v1"
    )

    test_texts = [
        "a photo of a cat",
        "猫の写真",
        "子供が遊んでいる",
        "children playing",
        "Ein Foto einer Katze",
    ]

    print()
    print("Test embeddings:")
    for text in test_texts:
        encoded = tokenizer(
            text, padding="max_length", truncation=True, max_length=seq_len,
            return_tensors="np"
        )
        result = session.run(
            ["text_embeds"],
            {
                "input_ids": encoded["input_ids"].astype(np.int64),
                "attention_mask": encoded["attention_mask"].astype(np.int64),
            },
        )
        emb = result[0][0]
        norm = np.linalg.norm(emb)
        print(f"  '{text}': dim={len(emb)}, norm={norm:.4f}, first5={emb[:5].round(4)}")

    # Cross-check: compare ONNX vs PyTorch outputs
    print()
    print("Cross-validation (PyTorch vs ONNX):")
    for text in test_texts[:2]:
        # PyTorch
        pt_emb = model.encode(text, normalize_embeddings=True)

        # ONNX
        encoded = tokenizer(
            text, padding="max_length", truncation=True, max_length=seq_len,
            return_tensors="np"
        )
        ort_result = session.run(
            ["text_embeds"],
            {
                "input_ids": encoded["input_ids"].astype(np.int64),
                "attention_mask": encoded["attention_mask"].astype(np.int64),
            },
        )
        ort_emb = ort_result[0][0]

        cos_sim = np.dot(pt_emb, ort_emb) / (np.linalg.norm(pt_emb) * np.linalg.norm(ort_emb))
        print(f"  '{text}': cosine_similarity = {cos_sim:.6f}")

    print()
    print("Done!")
    print(f"  ONNX model: {onnx_path}")
    print(f"  Vocab file: {vocab_path}")


if __name__ == "__main__":
    main()
