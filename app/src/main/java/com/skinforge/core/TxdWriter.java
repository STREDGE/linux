package com.skinforge.core;

import java.io.ByteArrayOutputStream;
import java.util.Map;

/** Writes textures as a RenderWare texture dictionary (GTA San Andreas, uncompressed 32-bit). */
public final class TxdWriter {

    private static final int STAMP = 0x1803FFFF;

    private TxdWriter() {
    }

    public static byte[] write(Map<String, int[]> textures, int size) {
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        ByteArrayOutputStream head = new ByteArrayOutputStream();
        u16(head, textures.size());
        u16(head, 2); // Direct3D 9
        chunk(body, 0x01, head.toByteArray());
        for (Map.Entry<String, int[]> e : textures.entrySet()) {
            ByteArrayOutputStream tex = new ByteArrayOutputStream();
            chunk(tex, 0x01, nativeTexture(e.getKey(), e.getValue(), size));
            chunk(tex, 0x03, new byte[0]);
            chunk(body, 0x15, tex.toByteArray());
        }
        chunk(body, 0x03, new byte[0]);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        chunk(out, 0x16, body.toByteArray());
        return out.toByteArray();
    }

    private static byte[] nativeTexture(String name, int[] px, int size) {
        int levels = 1;
        for (int s = size; s > 1; s >>= 1) levels++;
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        u32(o, 9);          // platform: Direct3D 9
        u32(o, 0x1106);     // trilinear filtering, wrap in U and V
        fixed(o, name, 32);
        fixed(o, "", 32);
        u32(o, 0x8600);     // 888 raster with mipmaps
        u32(o, 22);         // D3DFMT_X8R8G8B8
        u16(o, size);
        u16(o, size);
        o.write(32);
        o.write(levels);
        o.write(4);
        o.write(0);
        int[] cur = px;
        int s = size;
        for (int l = 0; l < levels; l++) {
            u32(o, s * s * 4);
            byte[] row = new byte[s * s * 4];
            for (int i = 0; i < s * s; i++) {
                int c = cur[i];
                row[4 * i] = (byte) c;
                row[4 * i + 1] = (byte) (c >> 8);
                row[4 * i + 2] = (byte) (c >> 16);
                row[4 * i + 3] = (byte) 0xFF;
            }
            o.write(row, 0, row.length);
            if (s > 1) {
                cur = halve(cur, s);
                s >>= 1;
            }
        }
        return o.toByteArray();
    }

    static int[] halve(int[] px, int s) {
        int h = s / 2;
        int[] out = new int[h * h];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < h; x++) {
                int a = px[2 * y * s + 2 * x], b = px[2 * y * s + 2 * x + 1],
                        c = px[(2 * y + 1) * s + 2 * x], d = px[(2 * y + 1) * s + 2 * x + 1];
                int r = (((a >> 16) & 0xFF) + ((b >> 16) & 0xFF) + ((c >> 16) & 0xFF) + ((d >> 16) & 0xFF) + 2) / 4;
                int g = (((a >> 8) & 0xFF) + ((b >> 8) & 0xFF) + ((c >> 8) & 0xFF) + ((d >> 8) & 0xFF) + 2) / 4;
                int bl = ((a & 0xFF) + (b & 0xFF) + (c & 0xFF) + (d & 0xFF) + 2) / 4;
                out[y * h + x] = 0xFF000000 | r << 16 | g << 8 | bl;
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
