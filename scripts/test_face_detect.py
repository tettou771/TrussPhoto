#!/usr/bin/env python3
"""Test SCRFD face detection + ArcFace embedding with ONNX Runtime."""

import sys, os
import numpy as np
from PIL import Image
import onnxruntime as ort

MODEL_DIR = os.path.join(os.path.dirname(__file__), "..", "bin", "data", "insightface")
DET_MODEL = os.path.join(MODEL_DIR, "det_10g.onnx")
REC_MODEL = os.path.join(MODEL_DIR, "w600k_r50.onnx")

INPUT_SIZE = 640
NMS_THRESH = 0.4
SCORE_THRESH = 0.5

def distance2bbox(points, distance):
    x1 = points[:, 0] - distance[:, 0]
    y1 = points[:, 1] - distance[:, 1]
    x2 = points[:, 0] + distance[:, 2]
    y2 = points[:, 1] + distance[:, 3]
    return np.stack([x1, y1, x2, y2], axis=-1)

def distance2kps(points, distance):
    preds = []
    for i in range(0, distance.shape[1], 2):
        px = points[:, i % 2] + distance[:, i]
        py = points[:, i % 2 + 1] + distance[:, i + 1]
        preds.append(px)
        preds.append(py)
    return np.stack(preds, axis=-1)

def nms(dets, thresh):
    x1, y1, x2, y2, scores = dets[:, 0], dets[:, 1], dets[:, 2], dets[:, 3], dets[:, 4]
    areas = (x2 - x1 + 1) * (y2 - y1 + 1)
    order = scores.argsort()[::-1]
    keep = []
    while order.size > 0:
        i = order[0]
        keep.append(i)
        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])
        w = np.maximum(0.0, xx2 - xx1 + 1)
        h = np.maximum(0.0, yy2 - yy1 + 1)
        inter = w * h
        ovr = inter / (areas[i] + areas[order[1:]] - inter)
        inds = np.where(ovr <= thresh)[0]
        order = order[inds + 1]
    return keep

def detect_faces(session, img_rgb):
    """Run SCRFD detection on RGB image (numpy HWC uint8)."""
    img_h, img_w = img_rgb.shape[:2]

    # Resize keeping aspect ratio
    im_ratio = float(img_h) / img_w
    if im_ratio > 1.0:
        new_h = INPUT_SIZE
        new_w = int(new_h / im_ratio)
    else:
        new_w = INPUT_SIZE
        new_h = int(new_w * im_ratio)
    det_scale = float(new_h) / img_h

    resized = np.array(Image.fromarray(img_rgb).resize((new_w, new_h), Image.BILINEAR))
    det_img = np.zeros((INPUT_SIZE, INPUT_SIZE, 3), dtype=np.uint8)
    det_img[:new_h, :new_w, :] = resized

    # Normalize: (pixel - 127.5) / 128.0, BGR, NCHW
    blob = det_img[:, :, ::-1].astype(np.float32)  # RGB -> BGR
    blob = (blob - 127.5) / 128.0
    blob = blob.transpose(2, 0, 1)[np.newaxis, ...]  # NCHW

    # Inference
    input_name = session.get_inputs()[0].name
    output_names = [o.name for o in session.get_outputs()]
    outputs = session.run(output_names, {input_name: blob})

    # Decode (9 outputs: 3×scores + 3×bbox + 3×kps)
    strides = [8, 16, 32]
    num_anchors = 2
    fmc = 3

    all_scores, all_bboxes, all_kps = [], [], []
    for idx, stride in enumerate(strides):
        scores = outputs[idx]
        bbox_preds = outputs[idx + fmc] * stride
        kps_preds = outputs[idx + fmc * 2] * stride

        h = INPUT_SIZE // stride
        w = INPUT_SIZE // stride

        anchor_centers = np.stack(np.mgrid[:h, :w][::-1], axis=-1).astype(np.float32)
        anchor_centers = (anchor_centers * stride).reshape((-1, 2))
        anchor_centers = np.stack([anchor_centers] * num_anchors, axis=1).reshape((-1, 2))

        pos_inds = np.where(scores >= SCORE_THRESH)[0]
        bboxes = distance2bbox(anchor_centers, bbox_preds)
        kpss = distance2kps(anchor_centers, kps_preds).reshape((-1, 5, 2))

        all_scores.append(scores[pos_inds])
        all_bboxes.append(bboxes[pos_inds])
        all_kps.append(kpss[pos_inds])

    scores = np.vstack(all_scores).ravel()
    bboxes = np.vstack(all_bboxes) / det_scale
    kpss = np.vstack(all_kps) / det_scale

    # NMS
    pre_det = np.hstack((bboxes, scores[:, np.newaxis])).astype(np.float32)
    order = scores.argsort()[::-1]
    pre_det = pre_det[order]
    kpss = kpss[order]
    keep = nms(pre_det, NMS_THRESH)

    return pre_det[keep], kpss[keep]

def align_face(img_rgb, kps, image_size=112):
    """Align face using similarity transform from 5 landmarks to ArcFace template."""
    from numpy.linalg import inv, norm

    dst = np.array([
        [38.2946, 51.6963], [73.5318, 51.5014], [56.0252, 71.7366],
        [41.5493, 92.3655], [70.7299, 92.2041]], dtype=np.float32)

    src = kps.astype(np.float32)

    # Umeyama: estimate similarity transform
    src_mean = src.mean(axis=0)
    dst_mean = dst.mean(axis=0)
    src_c = src - src_mean
    dst_c = dst - dst_mean

    src_var = np.sum(src_c ** 2) / len(src)
    cov = dst_c.T @ src_c / len(src)

    U, S, Vt = np.linalg.svd(cov)
    d = np.eye(2)
    if np.linalg.det(cov) < 0:
        d[1, 1] = -1

    R = U @ d @ Vt
    scale = np.trace(np.diag(S) @ d) / src_var

    t = dst_mean - scale * R @ src_mean
    M = np.zeros((2, 3), dtype=np.float32)
    M[:2, :2] = scale * R
    M[:, 2] = t

    # Warp using PIL
    # Inverse of M for PIL transform
    M_full = np.eye(3)
    M_full[:2] = M
    M_inv = inv(M_full)[:2]

    img_pil = Image.fromarray(img_rgb)
    aligned = img_pil.transform(
        (image_size, image_size), Image.AFFINE,
        M_inv.flatten(), Image.BILINEAR)
    return np.array(aligned), M

def get_embedding(session, aligned_rgb):
    """Get 512D face embedding from aligned 112x112 RGB image."""
    blob = aligned_rgb[:, :, ::-1].astype(np.float32)  # RGB -> BGR
    blob = (blob - 127.5) / 127.5
    blob = blob.transpose(2, 0, 1)[np.newaxis, ...]

    input_name = session.get_inputs()[0].name
    output_name = session.get_outputs()[0].name
    out = session.run([output_name], {input_name: blob})[0]

    # L2 normalize
    emb = out.flatten()
    emb = emb / np.linalg.norm(emb)
    return emb

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <image_path> [image_path2 ...]")
        sys.exit(1)

    print("Loading SCRFD...")
    det_session = ort.InferenceSession(DET_MODEL, providers=['CoreMLExecutionProvider', 'CPUExecutionProvider'])
    print("Loading ArcFace...")
    rec_session = ort.InferenceSession(REC_MODEL, providers=['CoreMLExecutionProvider', 'CPUExecutionProvider'])

    embeddings = {}

    for img_path in sys.argv[1:]:
        print(f"\n=== {os.path.basename(img_path)} ===")
        img = np.array(Image.open(img_path).convert("RGB"))
        print(f"  Image: {img.shape[1]}x{img.shape[0]}")

        dets, kpss = detect_faces(det_session, img)
        print(f"  Faces detected: {len(dets)}")

        for i, (det, kps) in enumerate(zip(dets, kpss)):
            x1, y1, x2, y2, score = det
            print(f"  Face {i}: bbox=({x1:.0f},{y1:.0f},{x2:.0f},{y2:.0f}) score={score:.3f}")
            for k in range(5):
                print(f"    kp{k}: ({kps[k,0]:.1f}, {kps[k,1]:.1f})")

            aligned, M = align_face(img, kps)
            emb = get_embedding(rec_session, aligned)
            key = f"{os.path.basename(img_path)}_face{i}"
            embeddings[key] = emb
            print(f"  Embedding: dim={len(emb)}, norm={np.linalg.norm(emb):.4f}, first5={emb[:5]}")

    # Compare embeddings
    if len(embeddings) > 1:
        keys = list(embeddings.keys())
        print("\n=== Similarity Matrix ===")
        for i in range(len(keys)):
            for j in range(i + 1, len(keys)):
                sim = np.dot(embeddings[keys[i]], embeddings[keys[j]])
                print(f"  {keys[i]} vs {keys[j]}: {sim:.4f}")

if __name__ == "__main__":
    main()
