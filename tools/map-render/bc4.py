import numpy as np

def decode_bc4_unorm(raw: bytes, width: int, height: int) -> np.ndarray:
    """Decode a BC4_UNORM (ATI1/3Dc single-channel) compressed buffer into
    an (height, width) uint8 grayscale array. Vectorized with numpy."""
    assert width % 4 == 0 and height % 4 == 0, "BC4 requires dims multiple of 4"
    blocks_x = width // 4
    blocks_y = height // 4
    num_blocks = blocks_x * blocks_y
    expected_len = num_blocks * 8
    buf = np.frombuffer(raw, dtype=np.uint8)
    if buf.size < expected_len:
        raise ValueError(f"raw buffer too small: have {buf.size}, need {expected_len}")
    buf = buf[:expected_len].reshape(num_blocks, 8)

    red0 = buf[:, 0].astype(np.int32)
    red1 = buf[:, 1].astype(np.int32)
    idx_bytes = buf[:, 2:8].astype(np.uint64)
    # pack 6 bytes little-endian into a 48-bit integer per block
    bits = np.zeros(num_blocks, dtype=np.uint64)
    for i in range(6):
        bits |= idx_bytes[:, i] << np.uint64(8 * i)

    # extract 16 x 3-bit indices per block
    indices = np.zeros((num_blocks, 16), dtype=np.uint8)
    for i in range(16):
        indices[:, i] = ((bits >> np.uint64(3 * i)) & np.uint64(0x7)).astype(np.uint8)

    # build the 8-value palette per block depending on red0 vs red1
    palette = np.zeros((num_blocks, 8), dtype=np.float32)
    palette[:, 0] = red0
    palette[:, 1] = red1

    gt = red0 > red1  # 8-value interpolation mode
    # mode A (gt): 6 interpolated between red0..red1
    for i in range(1, 7):
        palette[gt, i + 1] = ((7 - i) * red0[gt] + i * red1[gt]) / 7.0
    # mode B (not gt): 4 interpolated + 0 and 255
    for i in range(1, 5):
        palette[~gt, i + 1] = ((5 - i) * red0[~gt] + i * red1[~gt]) / 5.0
    palette[~gt, 6] = 0.0
    palette[~gt, 7] = 255.0

    # gather pixel values per block: (num_blocks, 16)
    block_pixels = np.take_along_axis(palette, indices.astype(np.int64), axis=1)
    block_pixels = np.clip(np.round(block_pixels), 0, 255).astype(np.uint8)
    block_pixels = block_pixels.reshape(num_blocks, 4, 4)  # (block, row-in-block, col-in-block)

    out = np.zeros((height, width), dtype=np.uint8)
    block_pixels = block_pixels.reshape(blocks_y, blocks_x, 4, 4)
    # place each block into the output grid
    out = block_pixels.transpose(0, 2, 1, 3).reshape(blocks_y * 4, blocks_x * 4)
    return out

if __name__ == "__main__":
    import sys
    path = sys.argv[1]
    w = int(sys.argv[2])
    h = int(sys.argv[3])
    with open(path, "rb") as f:
        raw = f.read()
    arr = decode_bc4_unorm(raw, w, h)
    print("decoded shape", arr.shape, "min", arr.min(), "max", arr.max(), "mean", arr.mean())
    from PIL import Image
    Image.fromarray(arr, mode="L").save(path + ".png")
    print("saved " + path + ".png")
