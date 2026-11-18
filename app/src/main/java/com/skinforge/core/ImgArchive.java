package com.skinforge.core;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/** Directory of a GTA San Andreas IMG archive (version 2), e.g. gta3.img or player.img. */
public final class ImgArchive {

    public static final int SECTOR = 2048;

    public static final class Entry {
        public String name;
        public long offset;
        public int size;
    }

    public final List<Entry> entries = new ArrayList<>();

    public static boolean isImg(byte[] head) {
        return head.length >= 8 && head[0] == 'V' && head[1] == 'E' && head[2] == 'R' && head[3] == '2';
    }

    /** Number of bytes the directory occupies, given the first 8 bytes of the file. */
    public static int directorySize(byte[] head) throws IOException {
        int count = RwModel.i32(head, 4);
        if (count < 0 || count > 200000) throw new IOException("Архив IMG повреждён");
        return 8 + 32 * count;
    }

    /** Parses the directory from the first {@link #directorySize} bytes of the file. */
    public static ImgArchive parse(byte[] dir) throws IOException {
        if (!isImg(dir)) throw new IOException("Это не архив IMG версии 2");
        int count = RwModel.i32(dir, 4);
        if (dir.length < 8 + 32 * count) throw new IOException("Архив IMG обрезан");
        ImgArchive a = new ImgArchive();
        for (int i = 0; i < count; i++) {
            int o = 8 + 32 * i;
            Entry e = new Entry();
            e.offset = (RwModel.i32(dir, o) & 0xFFFFFFFFL) * SECTOR;
            int sectors = RwModel.u16(dir, o + 4);
            if (sectors == 0) sectors = RwModel.u16(dir, o + 6);
            e.size = sectors * SECTOR;
            int len = 0;
            while (len < 24 && dir[o + 8 + len] != 0) len++;
            e.name = new String(dir, o + 8, len);
            a.entries.add(e);
        }
        return a;
    }
}
