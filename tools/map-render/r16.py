import numpy as np
from PIL import Image
import sys

def decode_r16_unorm(raw: bytes, width: int, height: int) -> np.ndarray:
    arr = np.frombuffer(raw, dtype='<u2')  # little-endian uint16
    expected = width * height
    if arr.size < expected:
        raise ValueError(f"raw too small: {arr.size} < {expected}")
    arr = arr[:expected].reshape(height, width)
    return arr

if __name__ == "__main__":
    path = sys.argv[1]
    w = int(sys.argv[2]); h = int(sys.argv[3])
    raw = open(path, "rb").read()
    arr = decode_r16_unorm(raw, w, h)
    print("shape", arr.shape, "min", arr.min(), "max", arr.max(), "mean", arr.mean())
    # normalize for preview
    norm = ((arr.astype(np.float32) - arr.min()) / max(1, (arr.max() - arr.min())) * 255).astype(np.uint8)
    Image.fromarray(norm, mode="L").save(path + ".png")
    print("saved preview")
