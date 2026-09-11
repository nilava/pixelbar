"""Tiny numpy z-buffer renderer: meshes -> PNG (flat shading, perspective camera)."""
import zlib, struct
import numpy as np


def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def render(meshes, cam, look, W=1400, H=900, fov=30, path="out.png", up=(0, 0, 1)):
    cam, look, up = map(lambda v: np.asarray(v, float), (cam, look, up))
    f = look - cam; f /= np.linalg.norm(f)
    r = np.cross(f, up); r /= np.linalg.norm(r)
    u = np.cross(r, f)
    focal = 0.5 * H / np.tan(np.radians(fov) / 2)
    zbuf = np.full((H, W), np.inf)
    img = np.full((H, W, 3), 42, np.uint8)
    light = np.array([-0.4, -0.6, 0.7]); light /= np.linalg.norm(light)
    for tm, col in meshes:
        col = np.asarray(col, float)
        V = tm.vertices[tm.faces]
        N = tm.face_normals
        shade = 0.35 + 0.65 * np.clip(np.abs(N @ light), 0, 1)
        P = V - cam
        x = P @ r; y = P @ u; z = P @ f
        sx = W / 2 + focal * x / z
        sy = H / 2 - focal * y / z
        for i in range(len(V)):
            if np.any(z[i] <= 0.1):
                continue
            xs, ys, zs = sx[i], sy[i], z[i]
            x0, x1 = int(max(0, np.floor(xs.min()))), int(min(W - 1, np.ceil(xs.max())))
            y0, y1 = int(max(0, np.floor(ys.min()))), int(min(H - 1, np.ceil(ys.max())))
            if x1 < x0 or y1 < y0:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            (ax, ay), (bx, by), (cx, cy) = zip(xs, ys)
            det = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay)
            if abs(det) < 1e-9:
                continue
            l1 = ((bx - gx) * (cy - gy) - (cx - gx) * (by - gy)) / det
            l2 = ((cx - gx) * (ay - gy) - (ax - gx) * (cy - gy)) / det
            l3 = 1 - l1 - l2
            inside = (l1 >= -1e-6) & (l2 >= -1e-6) & (l3 >= -1e-6)
            if not inside.any():
                continue
            depth = l1 * zs[0] + l2 * zs[1] + l3 * zs[2]
            sub = zbuf[y0:y1 + 1, x0:x1 + 1]
            m = inside & (depth < sub)
            sub[m] = depth[m]
            img[y0:y1 + 1, x0:x1 + 1][m] = np.clip(col * shade[i], 0, 255).astype(np.uint8)
    write_png(path, img)
    print("wrote", path)
