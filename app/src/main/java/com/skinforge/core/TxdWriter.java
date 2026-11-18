package com.skinforge.core;

import java.io.ByteArrayOutputStream;
import java.util.Map;

/** Writes textures as a RenderWare texture dictionary (GTA San Andreas, uncompressed 32-bit). */
public final class TxdWriter {

    private static final int STAMP = 0x1803FFFF;

    private TxdWriter() {
    }

    public static byte[] write(Map<String, Projector.Tex> textures) {
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        ByteArrayOutputStream head = new ByteArrayOutputStream();
        u16(head, textures.size());
        u16(head, 2); // Direct3D 9
        chunk(body, 0x01, head.toByteArray());
        for (Map.Entry<String, Projector.Tex> e : textures.entrySet()) {
            ByteArrayOutputStream tex = new ByteArrayOutputStream();
            chunk(tex, 0x01, nativeTexture(e.getKey(), e.getValue()));
            chunk(tex, 0x03, new byte[0]);
            chunk(body, 0x15, tex.toByteArray());
        }
        chunk(body, 0x03, new byte[0]);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        chunk(out, 0x16, body.toByteArray());
        return out.toByteArray();
    }

    private static byte[] nativeTexture(String name, Projector.Tex t) {
        int levels = 1;
        for (int a = t.w, b = t.h; a > 1 || b > 1; a = Math.max(1, a / 2), b = Math.max(1, b / 2)) levels++;
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        u32(o, 9);          // platform: Direct3D 9
        u32(o, 0x1106);     // trilinear filtering, wrap in U and V
        fixed(o, name, 32);
        fixed(o, "", 32);
        u32(o, 0x8600);     // 888 raster with mipmaps
        u32(o, 22);         // D3DFMT_X8R8G8B8
        u16(o, t.w);
        u16(o, t.h);
        o.write(32);
        o.write(levels);
        o.write(4);
        o.write(0);
        int[] cur = t.px;
        int w = t.w, h = t.h;
        for (int l = 0; l < levels; l++) {
            u32(o, w * h * 4);
            byte[] row = new byte[w * h * 4];
            for (int i = 0; i < w * h; i++) {
                int c = cur[i];
                row[4 * i] = (byte) c;
                row[4 * i + 1] = (byte) (c >> 8);
                row[4 * i + 2] = (byte) (c >> 16);
                row[4 * i + 3] = (byte) 0xFF;
            }
            o.write(row, 0, row.length);
            if (w > 1 || h > 1) {
                cur = halve(cur, w, h);
                w = Math.max(1, w / 2);
                h = Math.max(1, h / 2);
            }
        }
        return o.toByteArray();
    }

    /** Next mip level: each side halved (never below one pixel). */
    public static int[] halve(int[] px, int w, int h) {
        int nw = Math.max(1, w / 2), nh = Math.max(1, h / 2);
        int[] out = new int[nw * nh];
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                int x0 = Math.min(w - 1, 2 * x), x1 = Math.min(w - 1, 2 * x + 1);
                int y0 = Math.min(h - 1, 2 * y), y1 = Math.min(h - 1, 2 * y + 1);
                int a = px[y0 * w + x0], b = px[y0 * w + x1], c = px[y1 * w + x0], d = px[y1 * w + x1];
                int r = (((a >> 16) & 0xFF) + ((b >> 16) & 0xFF) + ((c >> 16) & 0xFF) + ((d >> 16) & 0xFF) + 2) / 4;
                int g = (((a >> 8) & 0xFF) + ((b >> 8) & 0xFF) + ((c >> 8) & 0xFF) + ((d >> 8) & 0xFF) + 2) / 4;
                int bl = ((a & 0xFF) + (b & 0xFF) + (c & 0xFF) + (d & 0xFF) + 2) / 4;
                out[y * nw + x] = 0xFF000000 | r << 16 | g << 8 | bl;
            }
        return out;
    }

    private static void chunk(ByteArrayOutputStream o, int type, byte[] data) {
        u32(o, type);
        u32(o, data.length);
        u32(o, STAMP);
        o.write(data, 0, data.length);
    }

    private static void fixed(ByteArrayOutputStream o, String s, int len) {
        byte[] b = new byte[len];
        byte[] src = s.getBytes(java.nio.charset.StandardCharsets.US_ASCII);
        System.arraycopy(src, 0, b, 0, Math.min(src.length, len - 1));
        o.write(b, 0, len);
    }

    private static void u32(ByteArrayOutputStream o, int v) {
        o.write(v);
        o.write(v >> 8);
        o.write(v >> 16);
        o.write(v >> 24);
    }

    private static void u16(ByteArrayOutputStream o, int v) {
        o.write(v);
        o.write(v >> 8);
    }
}
