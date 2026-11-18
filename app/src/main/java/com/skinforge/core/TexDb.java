package com.skinforge.core;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * Texture cache of the mobile GTA San Andreas ("texdb"): NAME.txt lists the textures,
 * NAME.FMT.dat holds their data, NAME.FMT.toc the offsets and NAME.FMT.tmb tiny previews.
 * This class replaces chosen textures with uncompressed RGBA ones, which every GPU accepts.
 */
public final class TexDb {

    private static final int GL_RGBA8 = 0x1401;

    public static final class Entry {
        public String name;
        public String affiliate; // name of the entry this one borrows its picture from, or null
        public int width, height;
        String line;
    }

    public final List<Entry> entries = new ArrayList<>();
    private String header = "";
    private String eol = "\n";

    public static TexDb parseTxt(byte[] txt) throws IOException {
        String s = new String(txt, "ISO-8859-1");
        TexDb db = new TexDb();
        if (s.contains("\r\n")) db.eol = "\r\n";
        String[] lines = s.split("\r?\n");
        if (lines.length < 2) throw new IOException("Файл списка текстур пуст");
        db.header = lines[0];
        for (int i = 1; i < lines.length; i++) {
            String l = lines[i];
            if (l.trim().isEmpty()) continue;
            int q1 = l.indexOf('"'), q2 = l.indexOf('"', q1 + 1);
            if (q1 < 0 || q2 < 0) throw new IOException("Непонятная строка в списке текстур: " + l);
            Entry e = new Entry();
            e.line = l;
            e.name = l.substring(q1 + 1, q2);
            int a = l.indexOf("affiliate=");
            if (a >= 0) {
                int end = l.indexOf('"', a);
                e.affiliate = l.substring(a + 10, end < 0 ? l.length() : end).trim();
            }
            e.width = attr(l, "width=");
            e.height = attr(l, "height=");
            db.entries.add(e);
        }
        return db;
    }

    private static int attr(String l, String key) {
        int i = l.indexOf(key);
        if (i < 0) return 0;
        int j = i + key.length(), v = 0;
        while (j < l.length() && Character.isDigit(l.charAt(j))) v = v * 10 + (l.charAt(j++) - '0');
        return v;
    }

    /** Entry that actually holds the picture for {@code name}, following "affiliate" links. */
    public Entry resolve(String name) {
        for (int hop = 0; hop < 4; hop++) {
            Entry found = null;
            for (Entry e : entries) if (e.name.equalsIgnoreCase(name)) found = e;
            if (found == null) return null;
            if (found.affiliate == null) return found;
            name = found.affiliate;
        }
        return null;
    }

    /** The list file with sizes updated for the replaced textures. */
    public byte[] writeTxt(Map<String, Projector.Tex> replace) throws IOException {
        StringBuilder sb = new StringBuilder(header).append(eol);
        for (Entry e : entries) {
            Projector.Tex t = e.affiliate == null ? find(replace, e.name) : null;
            String l = e.line;
            if (t != null) {
                l = l.replaceAll("width=\\d+", "width=" + t.w).replaceAll("height=\\d+", "height=" + t.h);
                l = l.replaceAll(" png=[0-9a-fA-F]+", "").replaceAll(" img=[0-9a-fA-F]+", "");
            }
            sb.append(l).append(eol);
        }
        return sb.toString().getBytes("ISO-8859-1");
    }

    private static Projector.Tex find(Map<String, Projector.Tex> m, String name) {
        for (Map.Entry<String, Projector.Tex> e : m.entrySet()) if (e.getKey().equalsIgnoreCase(name)) return e.getValue();
        return null;
    }

    /**
     * Rebuilds one data set. Returns {dat, toc, tmb}; tmb is returned unchanged
     * when it is absent or does not line up with the entries.
     */
    public byte[][] rebuild(byte[] dat, byte[] toc, byte[] tmb, Map<String, Projector.Tex> replace) throws IOException {
        int n = entries.size();
        if (toc.length < 4 * (n + 1)) throw new IOException("Оглавление (.toc) не совпадает со списком текстур");
        ByteArrayOutputStream nd = new ByteArrayOutputStream(dat.length);
        int[] newOff = new int[n];
        // previews are stored in entry order, one per texture that has its own data
        List<int[]> thumbs = new ArrayList<>();
        if (tmb != null) {
            int p = 0;
            while (p + 12 <= tmb.length) {
                int size = RwModel.i32(tmb, p + 8);
                if (size < 0 || p + 12 + size > tmb.length) break;
                thumbs.add(new int[]{p, 12 + size});
                p += 12 + size;
            }
            if (p != tmb.length) thumbs.clear();
        }
        int own = 0;
        for (int i = 0; i < n; i++) if (RwModel.i32(toc, 4 + 4 * i) != -1) own++;
        boolean thumbsOk = tmb != null && thumbs.size() == own;
        ByteArrayOutputStream nt = new ByteArrayOutputStream();
        int k = 0;
        for (int i = 0; i < n; i++) {
            int off = RwModel.i32(toc, 4 + 4 * i);
            if (off == -1) {
                newOff[i] = -1;
                continue;
            }
            if (off < 0 || off + 12 > dat.length) throw new IOException("Оглавление (.toc) указывает за пределы файла данных");
            int size = RwModel.i32(dat, off + 8);
            if (size < 0 || off + 12 + size > dat.length) throw new IOException("Файл данных текстур повреждён");
            Projector.Tex t = find(replace, entries.get(i).name);
            newOff[i] = nd.size();
            if (t == null) {
                nd.write(dat, off, 12 + size);
                if (thumbsOk) nt.write(tmb, thumbs.get(k)[0], thumbs.get(k)[1]);
            } else {
                writeEntry(nd, t);
                if (thumbsOk) writeThumb(nt, t);
            }
            k++;
        }
        ByteArrayOutputStream nc = new ByteArrayOutputStream();
        u32(nc, nd.size());
        for (int i = 0; i < n; i++) u32(nc, newOff[i]);
        // keep anything that followed the offsets in the original file
        nc.write(toc, 4 * (n + 1), toc.length - 4 * (n + 1));
        return new byte[][]{nd.toByteArray(), nc.toByteArray(), thumbsOk ? nt.toByteArray() : tmb};
    }

    private static void writeEntry(ByteArrayOutputStream o, Projector.Tex t) {
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        u32(body, 0); // not run-length packed
        int[] cur = t.px;
        int w = t.w, h = t.h;
        while (true) {
            byte[] b = new byte[4 * w * h];
            for (int i = 0; i < w * h; i++) {
                int c = cur[i];
                b[4 * i] = (byte) (c >> 16);
                b[4 * i + 1] = (byte) (c >> 8);
                b[4 * i + 2] = (byte) c;
                b[4 * i + 3] = (byte) 0xFF;
            }
            body.write(b, 0, b.length);
            if (w == 1 && h == 1) break;
            cur = TxdWriter.halve(cur, w, h);
            w = Math.max(1, w / 2);
            h = Math.max(1, h / 2);
        }
        u16(o, 0xADDE);
        u16(o, GL_RGBA8);
        u16(o, t.w);
        u16(o, t.h | 0x8000); // has mipmaps
        u32(o, body.size());
        o.write(body.toByteArray(), 0, body.size());
    }

    private static void writeThumb(ByteArrayOutputStream o, Projector.Tex t) {
        long r = 0, g = 0, b = 0;
        for (int c : t.px) {
            r += (c >> 16) & 0xFF;
            g += (c >> 8) & 0xFF;
            b += c & 0xFF;
        }
        int n = t.px.length;
        u16(o, 0xADDE);
        u16(o, GL_RGBA8);
        u16(o, 1);
        u16(o, 1);
        u32(o, 8);
        u32(o, 0);
        o.write((int) (r / n));
        o.write((int) (g / n));
        o.write((int) (b / n));
        o.write(0xFF);
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
