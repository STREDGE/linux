package com.skinforge.core;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Paints a model's textures by projecting reference pictures (front / side / back
 * views of a character) onto the mesh, and renders previews of the result.
 */
public final class Projector {

    /** One reference picture together with the box that frames the character in it. */
    public static final class View {
        public int[] px;
        public int w, h;
        /** Box around the figure: top = top of head, bottom = soles, left/right = centred on the body. */
        public float l, t, r, b;

        public View(int[] px, int w, int h, float l, float t, float r, float b) {
            this.px = px;
            this.w = w;
            this.h = h;
            this.l = l;
            this.t = t;
            this.r = r;
            this.b = b;
        }
    }

    public static final class Options {
        public int size = 1024;
        public boolean flipFront;
        public boolean sideFacesRight = true;
        public float widthScale = 1f;
    }

    /** A finished texture. */
    public static final class Tex {
        public final int w, h;
        public final int[] px;

        public Tex(int w, int h, int[] px) {
            this.w = w;
            this.h = h;
            this.px = px;
        }
    }

    /** Body-aligned coordinate frame of a model. */
    public static final class Frame {
        public final float[] up = new float[3], front = new float[3], left = new float[3];
        public float minUp, height, midLeft, midFront;
    }

    private Projector() {
    }

    // ------------------------------------------------------------------ frame

    public static Frame analyze(RwModel m, boolean flipFront) {
        float[] mn = {Float.MAX_VALUE, Float.MAX_VALUE, Float.MAX_VALUE};
        float[] mx = {-Float.MAX_VALUE, -Float.MAX_VALUE, -Float.MAX_VALUE};
        for (RwModel.Geom g : m.geoms)
            for (int i = 0; i < g.numVerts; i++)
                for (int k = 0; k < 3; k++) {
                    float v = g.pos[3 * i + k];
                    if (v < mn[k]) mn[k] = v;
                    if (v > mx[k]) mx[k] = v;
                }
        float[] ext = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
        int iu = 0;
        for (int k = 1; k < 3; k++) if (ext[k] > ext[iu]) iu = k;
        int id = iu == 0 ? 1 : 0;
        for (int k = 0; k < 3; k++) if (k != iu && ext[k] < ext[id]) id = k;
        int il = 3 - iu - id;

        // head end: the shoulder band is wider than the shin band
        float loMin = Float.MAX_VALUE, loMax = -Float.MAX_VALUE, hiMin = Float.MAX_VALUE, hiMax = -Float.MAX_VALUE;
        for (RwModel.Geom g : m.geoms)
            for (int i = 0; i < g.numVerts; i++) {
                float h = (g.pos[3 * i + iu] - mn[iu]) / ext[iu];
                float s = g.pos[3 * i + il];
                if (h > 0.12f && h < 0.30f) {
                    loMin = Math.min(loMin, s);
                    loMax = Math.max(loMax, s);
                } else if (h > 0.70f && h < 0.88f) {
                    hiMin = Math.min(hiMin, s);
                    hiMax = Math.max(hiMax, s);
                }
            }
        float upSign = (hiMax - hiMin) >= (loMax - loMin) * 0.98f ? 1f : -1f;

        // front: toes stick out forward of the ankles
        double toe = 0, shin = 0;
        int nToe = 0, nShin = 0;
        for (RwModel.Geom g : m.geoms)
            for (int i = 0; i < g.numVerts; i++) {
                float h = (g.pos[3 * i + iu] - mn[iu]) / ext[iu];
                if (upSign < 0) h = 1 - h;
                float dd = g.pos[3 * i + id];
                if (h < 0.035f) {
                    toe += dd;
                    nToe++;
                } else if (h > 0.07f && h < 0.25f) {
                    shin += dd;
                    nShin++;
                }
            }
        float frontSign = 1f;
        if (nToe > 0 && nShin > 0 && toe / nToe < shin / nShin) frontSign = -1f;
        if (flipFront) frontSign = -frontSign;

        Frame f = new Frame();
        f.up[iu] = upSign;
        f.front[id] = frontSign;
        f.left[0] = f.up[1] * f.front[2] - f.up[2] * f.front[1];
        f.left[1] = f.up[2] * f.front[0] - f.up[0] * f.front[2];
        f.left[2] = f.up[0] * f.front[1] - f.up[1] * f.front[0];
        f.height = ext[iu];
        f.minUp = upSign > 0 ? mn[iu] : -mx[iu];
        f.midLeft = (f.left[il] > 0 ? 1 : -1) * (mn[il] + mx[il]) * 0.5f;
        f.midFront = frontSign * (mn[id] + mx[id]) * 0.5f;
        return f;
    }

    // ------------------------------------------------------------- projection

    /**
     * Builds one square texture per texture name used by the model.
     * {@code side} and {@code back} may be null; a missing back is filled with a
     * blurred copy of the front so clothes keep their colours without a second face.
     */
    public static LinkedHashMap<String, Tex> project(RwModel m, View front, View side, View back, Options o) {
        return project(m, front, side, back, o, null);
    }

    /**
     * Same, with an explicit {width, height} for some textures (keyed by lower-case name);
     * the rest are square, {@code o.size} wide.
     */
    public static LinkedHashMap<String, Tex> project(RwModel m, View front, View side, View back, Options o,
                                                     Map<String, int[]> sizes) {
        Frame f = analyze(m, o.flipFront);
        View softFront = back == null ? blurred(front) : null;
        LinkedHashMap<String, float[]> acc = new LinkedHashMap<>();
        LinkedHashMap<String, int[]> dims = new LinkedHashMap<>();
        for (String name : m.textureNames()) {
            String key = name.toLowerCase();
            int[] d = sizes == null ? null : sizes.get(key);
            if (d == null) d = new int[]{o.size, o.size};
            dims.put(key, d);
            acc.put(key, new float[4 * d[0] * d[1]]);
        }

        float[] col = new float[3], tmp = new float[3];
        for (RwModel.Geom g : m.geoms) {
            if (g.uv == null) continue;
            for (int t = 0; t < g.numTris; t++) {
                int mat = g.triMat[t];
                if (mat < 0 || g.matTex[mat] == null) continue;
                String key = g.matTex[mat].toLowerCase();
                float[] a = acc.get(key);
                int W = dims.get(key)[0], H = dims.get(key)[1];
                int i0 = g.tri[3 * t], i1 = g.tri[3 * t + 1], i2 = g.tri[3 * t + 2];
                float x0 = g.uv[2 * i0] * W - 0.5f, y0 = g.uv[2 * i0 + 1] * H - 0.5f;
                float x1 = g.uv[2 * i1] * W - 0.5f, y1 = g.uv[2 * i1 + 1] * H - 0.5f;
                float x2 = g.uv[2 * i2] * W - 0.5f, y2 = g.uv[2 * i2 + 1] * H - 0.5f;
                float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                if (Math.abs(area) < 1e-6f || Float.isNaN(area)) continue;
                int minX = (int) Math.floor(Math.min(x0, Math.min(x1, x2))) - 1;
                int maxX = (int) Math.ceil(Math.max(x0, Math.max(x1, x2))) + 1;
                int minY = (int) Math.floor(Math.min(y0, Math.min(y1, y2))) - 1;
                int maxY = (int) Math.ceil(Math.max(y0, Math.max(y1, y2))) + 1;
                if (maxX - minX > 4 * W || maxY - minY > 4 * H) continue;
                float tol = Math.min(0.5f, 0.75f / (float) Math.sqrt(Math.abs(area)));
                for (int y = minY; y <= maxY; y++) {
                    for (int x = minX; x <= maxX; x++) {
                        float w0 = ((x1 - x) * (y2 - y) - (x2 - x) * (y1 - y)) / area;
                        float w1 = ((x2 - x) * (y0 - y) - (x0 - x) * (y2 - y)) / area;
                        float w2 = 1f - w0 - w1;
                        if (w0 < -tol || w1 < -tol || w2 < -tol) continue;
                        boolean inside = w0 >= 0 && w1 >= 0 && w2 >= 0;
                        if (w0 < 0) w0 = 0;
                        if (w1 < 0) w1 = 0;
                        if (w2 < 0) w2 = 0;
                        float ws = w0 + w1 + w2;
                        w0 /= ws;
                        w1 /= ws;
                        w2 /= ws;
                        float px = w0 * g.pos[3 * i0] + w1 * g.pos[3 * i1] + w2 * g.pos[3 * i2];
                        float py = w0 * g.pos[3 * i0 + 1] + w1 * g.pos[3 * i1 + 1] + w2 * g.pos[3 * i2 + 1];
                        float pz = w0 * g.pos[3 * i0 + 2] + w1 * g.pos[3 * i1 + 2] + w2 * g.pos[3 * i2 + 2];
                        float nx = w0 * g.nrm[3 * i0] + w1 * g.nrm[3 * i1] + w2 * g.nrm[3 * i2];
                        float ny = w0 * g.nrm[3 * i0 + 1] + w1 * g.nrm[3 * i1 + 1] + w2 * g.nrm[3 * i2 + 1];
                        float nz = w0 * g.nrm[3 * i0 + 2] + w1 * g.nrm[3 * i1 + 2] + w2 * g.nrm[3 * i2 + 2];
                        float conf = shade(f, front, side, back, softFront, o, px, py, pz, nx, ny, nz, col, tmp);
                        // texels just outside the triangle only fill gaps, they never outvote real coverage
                        if (!inside) conf *= 0.02f;
                        int tx = ((x % W) + W) % W, ty = ((y % H) + H) % H;
                        int k = 4 * (ty * W + tx);
                        a[k] += col[0] * conf;
                        a[k + 1] += col[1] * conf;
                        a[k + 2] += col[2] * conf;
                        a[k + 3] += conf;
                    }
                }
            }
        }

        LinkedHashMap<String, Tex> out = new LinkedHashMap<>();
        for (String name : m.textureNames()) {
            if (out.containsKey(name)) continue;
            int[] d = dims.get(name.toLowerCase());
            out.put(name, new Tex(d[0], d[1], resolve(acc.get(name.toLowerCase()), d[0], d[1])));
        }
        return out;
    }

    /** Colour of one surface point blended from the available views; returns its confidence. */
    private static float shade(Frame f, View front, View side, View back, View softFront, Options o,
                               float px, float py, float pz, float nx, float ny, float nz,
                               float[] col, float[] tmp) {
        float hU = px * f.up[0] + py * f.up[1] + pz * f.up[2];
        float hL = px * f.left[0] + py * f.left[1] + pz * f.left[2] - f.midLeft;
        float hF = px * f.front[0] + py * f.front[1] + pz * f.front[2] - f.midFront;
        float tUp = (hU - f.minUp) / f.height;
        float nf = nx * f.front[0] + ny * f.front[1] + nz * f.front[2];
        float nl = nx * f.left[0] + ny * f.left[1] + nz * f.left[2];

        // steep weights: each surface takes almost all its colour from the view it faces most
        float nf2 = nf * nf, nf6 = nf2 * nf2 * nf2;
        float wF = nf > 0 ? nf6 : 0, wB = nf < 0 ? nf6 : 0;
        if (nf >= 0) wF += 0.003f;
        else wB += 0.003f;
        float wS = 0;
        if (side != null) {
            float s = nl * nl;
            wS = s * s * s;
        }
        float r = 0, g = 0, b = 0;
        float sc = o.widthScale;

        sample(front, 0.5f * (front.l + front.r) + hL / f.height * (front.b - front.t) * sc,
                front.b - tUp * (front.b - front.t), tmp);
        r += tmp[0] * wF;
        g += tmp[1] * wF;
        b += tmp[2] * wF;

        if (wB > 0) {
            if (back != null) {
                sample(back, 0.5f * (back.l + back.r) - hL / f.height * (back.b - back.t) * sc,
                        back.b - tUp * (back.b - back.t), tmp);
            } else {
                sample(softFront, 0.5f * (softFront.l + softFront.r)
                                + hL / f.height * (softFront.b - softFront.t) * sc,
                        softFront.b - tUp * (softFront.b - softFront.t), tmp);
            }
            r += tmp[0] * wB;
            g += tmp[1] * wB;
            b += tmp[2] * wB;
        }
        if (wS > 0) {
            float dir = o.sideFacesRight ? 1f : -1f;
            sample(side, 0.5f * (side.l + side.r) + dir * hF / f.height * (side.b - side.t) * sc,
                    side.b - tUp * (side.b - side.t), tmp);
            r += tmp[0] * wS;
            g += tmp[1] * wS;
            b += tmp[2] * wS;
        }
        float w = wF + wB + wS;
        col[0] = r / w;
        col[1] = g / w;
        col[2] = b / w;
        return w;
    }

    /** Bilinear sample, clamped to the picture. */
    private static void sample(View v, float x, float y, float[] out) {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > v.w - 1) x = v.w - 1;
        if (y > v.h - 1) y = v.h - 1;
        int x0 = (int) x, y0 = (int) y;
        int x1 = Math.min(x0 + 1, v.w - 1), y1 = Math.min(y0 + 1, v.h - 1);
        float fx = x - x0, fy = y - y0;
        int c00 = v.px[y0 * v.w + x0], c10 = v.px[y0 * v.w + x1], c01 = v.px[y1 * v.w + x0], c11 = v.px[y1 * v.w + x1];
        for (int k = 0; k < 3; k++) {
            int sh = 16 - 8 * k;
            float top = ((c00 >> sh) & 0xFF) * (1 - fx) + ((c10 >> sh) & 0xFF) * fx;
            float bot = ((c01 >> sh) & 0xFF) * (1 - fx) + ((c11 >> sh) & 0xFF) * fx;
            out[k] = top * (1 - fy) + bot * fy;
        }
    }

    /** Heavily blurred copy of a view (used for sides of the body no picture shows). */
    private static View blurred(View v) {
        int k = Math.max(2, Math.round((v.b - v.t) / 28f));
        int w = Math.max(1, v.w / k), h = Math.max(1, v.h / k);
        long[] sum = new long[3 * w * h];
        int[] cnt = new int[w * h];
        for (int y = 0; y < h * k; y++)
            for (int x = 0; x < w * k; x++) {
                int c = v.px[y * v.w + x];
                int i = (y / k) * w + x / k;
                sum[3 * i] += (c >> 16) & 0xFF;
                sum[3 * i + 1] += (c >> 8) & 0xFF;
                sum[3 * i + 2] += c & 0xFF;
                cnt[i]++;
            }
        int[] px = new int[w * h];
        for (int i = 0; i < w * h; i++) {
            int n = Math.max(1, cnt[i]);
            px[i] = 0xFF000000 | (int) (sum[3 * i] / n) << 16 | (int) (sum[3 * i + 1] / n) << 8 | (int) (sum[3 * i + 2] / n);
        }
        // one smoothing pass to hide the blocks
        int[] sm = new int[w * h];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        int c = px[yy * w + xx];
                        r += (c >> 16) & 0xFF;
                        g += (c >> 8) & 0xFF;
                        b += c & 0xFF;
                        n++;
                    }
                sm[y * w + x] = 0xFF000000 | (r / n) << 16 | (g / n) << 8 | (b / n);
            }
        float s = 1f / k;
        return new View(sm, w, h, v.l * s, v.t * s, v.r * s, v.b * s);
    }

    /** Turns accumulated colour into pixels and pads the gaps between UV islands. */
    private static int[] resolve(float[] a, int T, int TH) {
        int n = T * TH;
        int[] px = new int[n];
        boolean[] has = new boolean[n];
        double sr = 0, sg = 0, sb = 0;
        int cnt = 0;
        for (int i = 0; i < n; i++) {
            float w = a[4 * i + 3];
            if (w > 0) {
                int r = clamp(a[4 * i] / w), g = clamp(a[4 * i + 1] / w), b = clamp(a[4 * i + 2] / w);
                px[i] = 0xFF000000 | r << 16 | g << 8 | b;
                has[i] = true;
                sr += r;
                sg += g;
                sb += b;
                cnt++;
            }
        }
        int mean = cnt == 0 ? 0xFF808080
                : 0xFF000000 | (int) (sr / cnt) << 16 | (int) (sg / cnt) << 8 | (int) (sb / cnt);
        boolean[] next = new boolean[n];
        int[] npx = new int[n];
        for (int pass = 0; pass < 12; pass++) {
            boolean changed = false;
            System.arraycopy(has, 0, next, 0, n);
            System.arraycopy(px, 0, npx, 0, n);
            for (int y = 0; y < TH; y++)
                for (int x = 0; x < T; x++) {
                    int i = y * T + x;
                    if (has[i]) continue;
                    int r = 0, g = 0, b = 0, c = 0;
                    for (int d = 0; d < 4; d++) {
                        int xx = x + (d == 0 ? 1 : d == 1 ? -1 : 0), yy = y + (d == 2 ? 1 : d == 3 ? -1 : 0);
                        if (xx < 0 || yy < 0 || xx >= T || yy >= TH) continue;
                        int j = yy * T + xx;
                        if (!has[j]) continue;
                        r += (px[j] >> 16) & 0xFF;
                        g += (px[j] >> 8) & 0xFF;
                        b += px[j] & 0xFF;
                        c++;
                    }
                    if (c > 0) {
                        npx[i] = 0xFF000000 | (r / c) << 16 | (g / c) << 8 | (b / c);
                        next[i] = true;
                        changed = true;
                    }
                }
            System.arraycopy(next, 0, has, 0, n);
            System.arraycopy(npx, 0, px, 0, n);
            if (!changed) break;
        }
        for (int i = 0; i < n; i++) if (!has[i]) px[i] = mean;
        return px;
    }

    private static int clamp(float v) {
        return v < 0 ? 0 : v > 255 ? 255 : Math.round(v);
    }

    // ---------------------------------------------------------------- preview

    /**
     * Software render of the textured model, seen from {@code yawDeg} around the body
     * (0 = front, 180 = back). Returns ARGB pixels of size w * h.
     */
    public static int[] render(RwModel m, Map<String, Tex> textures, Frame f, float yawDeg, int w, int h) {
        int[] out = new int[w * h];
        float[] zb = new float[w * h];
        java.util.Arrays.fill(out, 0xFF26282C);
        java.util.Arrays.fill(zb, -Float.MAX_VALUE);
        double yaw = Math.toRadians(yawDeg);
        float cs = (float) Math.cos(yaw), sn = (float) Math.sin(yaw);
        float[] right = new float[3], toward = new float[3];
        for (int k = 0; k < 3; k++) {
            right[k] = f.left[k] * cs - f.front[k] * sn;
            toward[k] = f.front[k] * cs + f.left[k] * sn;
        }
        float cRight = f.midLeft * cs - f.midFront * sn;
        float scale = 0.92f * h / f.height;
        LinkedHashMap<String, Tex> lower = new LinkedHashMap<>();
        for (Map.Entry<String, Tex> e : textures.entrySet()) lower.put(e.getKey().toLowerCase(), e.getValue());

        float[] sx = new float[3], sy = new float[3], sz = new float[3], lit = new float[3];
        for (RwModel.Geom g : m.geoms) {
            for (int t = 0; t < g.numTris; t++) {
                int mat = g.triMat[t];
                Tex tex = (mat >= 0 && g.matTex[mat] != null && g.uv != null)
                        ? lower.get(g.matTex[mat].toLowerCase()) : null;
                for (int c = 0; c < 3; c++) {
                    int v = g.tri[3 * t + c];
                    float px = g.pos[3 * v], py = g.pos[3 * v + 1], pz = g.pos[3 * v + 2];
                    sx[c] = w * 0.5f + (px * right[0] + py * right[1] + pz * right[2] - cRight) * scale;
                    sy[c] = h * 0.96f - (px * f.up[0] + py * f.up[1] + pz * f.up[2] - f.minUp) * scale;
                    sz[c] = px * toward[0] + py * toward[1] + pz * toward[2];
                    float nd = g.nrm[3 * v] * toward[0] + g.nrm[3 * v + 1] * toward[1] + g.nrm[3 * v + 2] * toward[2];
                    lit[c] = 0.72f + 0.28f * Math.max(0, nd);
                }
                float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
                if (Math.abs(area) < 1e-6f) continue;
                int minX = Math.max(0, (int) Math.floor(Math.min(sx[0], Math.min(sx[1], sx[2]))));
                int maxX = Math.min(w - 1, (int) Math.ceil(Math.max(sx[0], Math.max(sx[1], sx[2]))));
                int minY = Math.max(0, (int) Math.floor(Math.min(sy[0], Math.min(sy[1], sy[2]))));
                int maxY = Math.min(h - 1, (int) Math.ceil(Math.max(sy[0], Math.max(sy[1], sy[2]))));
                int a = g.tri[3 * t], b = g.tri[3 * t + 1], c = g.tri[3 * t + 2];
                for (int y = minY; y <= maxY; y++)
                    for (int x = minX; x <= maxX; x++) {
                        float w0 = ((sx[1] - x) * (sy[2] - y) - (sx[2] - x) * (sy[1] - y)) / area;
                        float w1 = ((sx[2] - x) * (sy[0] - y) - (sx[0] - x) * (sy[2] - y)) / area;
                        float w2 = 1f - w0 - w1;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                        int i = y * w + x;
                        if (z <= zb[i]) continue;
                        zb[i] = z;
                        int colr = 0xFF9A9A9A;
                        if (tex != null) {
                            float u = w0 * g.uv[2 * a] + w1 * g.uv[2 * b] + w2 * g.uv[2 * c];
                            float v = w0 * g.uv[2 * a + 1] + w1 * g.uv[2 * b + 1] + w2 * g.uv[2 * c + 1];
                            int tx = (int) Math.floor((u - Math.floor(u)) * tex.w), ty = (int) Math.floor((v - Math.floor(v)) * tex.h);
                            if (tx >= tex.w) tx = tex.w - 1;
                            if (ty >= tex.h) ty = tex.h - 1;
                            colr = tex.px[ty * tex.w + tx];
                        }
                        float l = w0 * lit[0] + w1 * lit[1] + w2 * lit[2];
                        int r = (int) (((colr >> 16) & 0xFF) * l), gg = (int) (((colr >> 8) & 0xFF) * l),
                                bb = (int) ((colr & 0xFF) * l);
                        out[i] = 0xFF000000 | r << 16 | gg << 8 | bb;
                    }
            }
        }
        return out;
    }
}
