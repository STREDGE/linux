package com.skinforge.core;

import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

/**
 * Minimal reader for RenderWare DFF models (GTA III / VC / SA).
 * Only the data needed for texture projection is extracted:
 * vertices, UVs, triangles and the texture name of each material.
 */
public final class RwModel {

    private static final int T_STRUCT = 0x01, T_STRING = 0x02, T_TEXTURE = 0x06,
            T_MATERIAL = 0x07, T_MATLIST = 0x08, T_GEOMETRY = 0x0F, T_CLUMP = 0x10,
            T_GEOMLIST = 0x1A;

    public static final class Geom {
        public int numVerts, numTris;
        public float[] pos;      // 3 * numVerts
        public float[] uv;       // 2 * numVerts
        public int[] tri;        // 3 * numTris
        public int[] triMat;     // numTris
        public String[] matTex;  // texture name per material, null if untextured
        public float[] nrm;      // smooth outward normals, filled by computeNormals()
    }

    public final List<Geom> geoms = new ArrayList<>();

    public static RwModel parse(byte[] d) throws IOException {
        RwModel m = new RwModel();
        try {
            int off = 0;
            while (off + 12 <= d.length) {
                int type = i32(d, off), size = i32(d, off + 4);
                int end = off + 12 + size;
                if (size < 0 || end > d.length) break;
                if (type == T_CLUMP) m.parseClump(d, off + 12, end);
                off = end;
            }
        } catch (IndexOutOfBoundsException e) {
            throw new IOException("Файл DFF повреждён или обрезан");
        }
        if (m.geoms.isEmpty()) throw new IOException("В файле не найдена модель (DFF Clump)");
        for (Geom g : m.geoms) computeNormals(g);
        return m;
    }

    /** All distinct texture names used by the model, in order of appearance. */
    public List<String> textureNames() {
        List<String> out = new ArrayList<>();
        for (Geom g : geoms)
            for (String s : g.matTex)
                if (s != null && !out.contains(s)) out.add(s);
        return out;
    }

    private void parseClump(byte[] d, int s, int e) throws IOException {
        int off = s;
        while (off + 12 <= e) {
            int type = i32(d, off), size = chunkSize(d, off, e);
            if (type == T_GEOMLIST) parseGeomList(d, off + 12, off + 12 + size);
            off += 12 + size;
        }
    }

    private void parseGeomList(byte[] d, int s, int e) throws IOException {
        int off = s;
        while (off + 12 <= e) {
            int type = i32(d, off), size = chunkSize(d, off, e);
            if (type == T_GEOMETRY) {
                Geom g = parseGeometry(d, off + 12, off + 12 + size, i32(d, off + 8));
                if (g != null) geoms.add(g);
            }
            off += 12 + size;
        }
    }

    private Geom parseGeometry(byte[] d, int s, int e, int libId) throws IOException {
        Geom g = new Geom();
        int version = (libId & 0xFFFF0000) != 0
                ? (((libId >>> 14) & 0x3FF00) + 0x30000) | ((libId >>> 16) & 0x3F)
                : libId << 8;
        int off = s;
        boolean gotStruct = false;
        while (off + 12 <= e) {
            int type = i32(d, off), size = chunkSize(d, off, e);
            int body = off + 12;
            if (type == T_STRUCT && !gotStruct) {
                gotStruct = true;
                int p = body;
                int fmt = i32(d, p);
                g.numTris = i32(d, p + 4);
                g.numVerts = i32(d, p + 8);
                p += 16;
                if (g.numVerts <= 0 || g.numTris <= 0 || g.numVerts > 1000000 || g.numTris > 1000000)
                    return null;
                int flags = fmt & 0xFFFF;
                if ((fmt & 0x01000000) != 0)
                    throw new IOException("Модель в «нативном» формате консоли не поддерживается. "
                            + "Нужен обычный DFF от ПК или Android.");
                int nUV = (fmt >>> 16) & 0xFF;
                if (nUV == 0) nUV = (flags & 0x80) != 0 ? 2 : ((flags & 0x04) != 0 ? 1 : 0);
                if (version < 0x34000) p += 12;
                if ((flags & 0x08) != 0) p += 4 * g.numVerts;
                for (int k = 0; k < nUV; k++) {
                    if (k == 0) {
                        g.uv = new float[2 * g.numVerts];
                        for (int i = 0; i < 2 * g.numVerts; i++) g.uv[i] = f32(d, p + 4 * i);
                    }
                    p += 8 * g.numVerts;
                }
                g.tri = new int[3 * g.numTris];
                g.triMat = new int[g.numTris];
                for (int i = 0; i < g.numTris; i++) {
                    int v2 = u16(d, p), v1 = u16(d, p + 2), mat = u16(d, p + 4), v3 = u16(d, p + 6);
                    if (v1 >= g.numVerts || v2 >= g.numVerts || v3 >= g.numVerts)
                        throw new IOException("Файл DFF повреждён (неверный индекс вершины)");
                    g.tri[3 * i] = v1;
                    g.tri[3 * i + 1] = v2;
                    g.tri[3 * i + 2] = v3;
                    g.triMat[i] = mat;
                    p += 8;
                }
                p += 16; // bounding sphere
                int hasVerts = i32(d, p), hasNormals = i32(d, p + 4);
                p += 8;
                if (hasVerts == 0) return null;
                g.pos = new float[3 * g.numVerts];
                for (int i = 0; i < 3 * g.numVerts; i++) g.pos[i] = f32(d, p + 4 * i);
            } else if (type == T_MATLIST) {
                g.matTex = parseMaterialList(d, body, body + size);
            }
            off += 12 + size;
        }
        if (g.pos == null) return null;
        if (g.matTex == null) g.matTex = new String[0];
        // triangles referring to a missing material are treated as untextured
        for (int i = 0; i < g.numTris; i++)
            if (g.triMat[i] >= g.matTex.length) g.triMat[i] = -1;
        return g;
    }

    private String[] parseMaterialList(byte[] d, int s, int e) {
        int off = s;
        int[] index = null;
        List<String> mats = new ArrayList<>();
        while (off + 12 <= e) {
            int type = i32(d, off), size = chunkSize(d, off, e);
            int body = off + 12;
            if (type == T_STRUCT && index == null) {
                int n = i32(d, body);
                if (n < 0 || n > 4096) n = 0;
                index = new int[n];
                for (int i = 0; i < n; i++) index[i] = i32(d, body + 4 + 4 * i);
            } else if (type == T_MATERIAL) {
                mats.add(parseMaterialTexture(d, body, body + size));
            }
            off += 12 + size;
        }
        if (index == null) return new String[0];
        String[] out = new String[index.length];
        int next = 0;
        for (int i = 0; i < index.length; i++) {
            if (index[i] < 0) {
                out[i] = next < mats.size() ? mats.get(next) : null;
                next++;
            } else if (index[i] < i) {
                out[i] = out[index[i]];
            }
        }
        return out;
    }

    private String parseMaterialTexture(byte[] d, int s, int e) {
        int off = s;
        while (off + 12 <= e) {
            int type = i32(d, off), size = chunkSize(d, off, e);
            if (type == T_TEXTURE) {
                int o2 = off + 12, e2 = off + 12 + size;
                while (o2 + 12 <= e2) {
                    int t2 = i32(d, o2), s2 = chunkSize(d, o2, e2);
                    if (t2 == T_STRING) {
                        int len = 0;
                        while (len < s2 && d[o2 + 12 + len] != 0) len++;
                        String name = new String(d, o2 + 12, len).trim();
                        return name.isEmpty() ? null : name;
                    }
                    o2 += 12 + s2;
                }
            }
            off += 12 + size;
        }
        return null;
    }

    /** Smooth normals, welded across UV seams and oriented away from the body. */
    private static void computeNormals(Geom g) {
        int n = g.numVerts;
        // weld vertices that share a position so seams shade continuously
        int[] rep = new int[n];
        HashMap<Long, Integer> seen = new HashMap<>();
        for (int i = 0; i < n; i++) {
            long kx = Math.round(g.pos[3 * i] * 20000.0), ky = Math.round(g.pos[3 * i + 1] * 20000.0),
                    kz = Math.round(g.pos[3 * i + 2] * 20000.0);
            long key = ((kx & 0x1FFFFFL) << 42) | ((ky & 0x1FFFFFL) << 21) | (kz & 0x1FFFFFL);
            Integer r = seen.get(key);
            if (r == null) {
                seen.put(key, i);
                rep[i] = i;
            } else rep[i] = r;
        }
        float[] acc = new float[3 * n];
        double outward = 0;
        double cx = 0, cy = 0, cz = 0;
        for (int i = 0; i < n; i++) {
            cx += g.pos[3 * i];
            cy += g.pos[3 * i + 1];
            cz += g.pos[3 * i + 2];
        }
        cx /= n;
        cy /= n;
        cz /= n;
        for (int t = 0; t < g.numTris; t++) {
            int a = g.tri[3 * t], b = g.tri[3 * t + 1], c = g.tri[3 * t + 2];
            float ax = g.pos[3 * a], ay = g.pos[3 * a + 1], az = g.pos[3 * a + 2];
            float e1x = g.pos[3 * b] - ax, e1y = g.pos[3 * b + 1] - ay, e1z = g.pos[3 * b + 2] - az;
            float e2x = g.pos[3 * c] - ax, e2y = g.pos[3 * c + 1] - ay, e2z = g.pos[3 * c + 2] - az;
            float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
            outward += nx * (ax - cx) + ny * (ay - cy) + nz * (az - cz);
            int[] vs = {rep[a], rep[b], rep[c]};
            for (int v : vs) {
                acc[3 * v] += nx;
                acc[3 * v + 1] += ny;
                acc[3 * v + 2] += nz;
            }
        }
        float sign = outward >= 0 ? 1f : -1f;
        g.nrm = new float[3 * n];
        for (int i = 0; i < n; i++) {
            int r = rep[i];
            float nx = acc[3 * r], ny = acc[3 * r + 1], nz = acc[3 * r + 2];
            float len = (float) Math.sqrt(nx * nx + ny * ny + nz * nz);
            if (len < 1e-12f) {
                nx = 0;
                ny = 0;
                nz = 1;
                len = 1;
            }
            g.nrm[3 * i] = sign * nx / len;
            g.nrm[3 * i + 1] = sign * ny / len;
            g.nrm[3 * i + 2] = sign * nz / len;
        }
    }

    private static int chunkSize(byte[] d, int off, int end) {
        int size = i32(d, off + 4);
        if (size < 0 || off + 12 + size > end) {
            int unlocked = size & ~0x01000000;
            if (unlocked >= 0 && off + 12 + unlocked <= end) return unlocked;
            return Math.max(0, end - off - 12);
        }
        return size;
    }

    static int i32(byte[] d, int o) {
        return (d[o] & 0xFF) | (d[o + 1] & 0xFF) << 8 | (d[o + 2] & 0xFF) << 16 | (d[o + 3] & 0xFF) << 24;
    }

    static int u16(byte[] d, int o) {
        return (d[o] & 0xFF) | (d[o + 1] & 0xFF) << 8;
    }

    static float f32(byte[] d, int o) {
        return Float.intBitsToFloat(i32(d, o));
    }
}
