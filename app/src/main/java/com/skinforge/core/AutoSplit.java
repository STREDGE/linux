package com.skinforge.core;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Finds the figures in a character sheet (front / side / back on a plain backdrop)
 * and proposes a box around each. The result is only a starting point for the user.
 */
public final class AutoSplit {

    private AutoSplit() {
    }

    /** Returns up to three boxes {left, top, right, bottom}, ordered left to right. */
    public static List<float[]> detect(int[] px, int w, int h) {
        // backdrop colour = median of the picture's border
        int n = 0;
        int[] rs = new int[2 * (w + h)], gs = new int[2 * (w + h)], bs = new int[2 * (w + h)];
        for (int x = 0; x < w; x++)
            for (int y : new int[]{0, h - 1}) {
                int c = px[y * w + x];
                rs[n] = (c >> 16) & 0xFF;
                gs[n] = (c >> 8) & 0xFF;
                bs[n++] = c & 0xFF;
            }
        for (int y = 0; y < h; y++)
            for (int x : new int[]{0, w - 1}) {
                int c = px[y * w + x];
                rs[n] = (c >> 16) & 0xFF;
                gs[n] = (c >> 8) & 0xFF;
                bs[n++] = c & 0xFF;
            }
        Arrays.sort(rs, 0, n);
        Arrays.sort(gs, 0, n);
        Arrays.sort(bs, 0, n);
        int br = rs[n / 2], bg = gs[n / 2], bb = bs[n / 2];

        boolean[] fg = new boolean[w * h];
        float[] colScore = new float[w];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int c = px[y * w + x];
                int d = Math.max(Math.abs(((c >> 16) & 0xFF) - br),
                        Math.max(Math.abs(((c >> 8) & 0xFF) - bg), Math.abs((c & 0xFF) - bb)));
                if (d > 40) {
                    fg[y * w + x] = true;
                    colScore[x]++;
                }
            }
        // smooth the column profile and cut it into runs
        int rad = Math.max(2, w / 80);
        float[] sm = new float[w];
        for (int x = 0; x < w; x++) {
            float s = 0;
            int c = 0;
            for (int k = -rad; k <= rad; k++)
                if (x + k >= 0 && x + k < w) {
                    s += colScore[x + k];
                    c++;
                }
            sm[x] = s / c;
        }
        float thr = Math.max(1f, h * 0.012f);
        List<int[]> runs = new ArrayList<>();
        int start = -1;
        for (int x = 0; x <= w; x++) {
            boolean on = x < w && sm[x] > thr;
            if (on && start < 0) start = x;
            if (!on && start >= 0) {
                runs.add(new int[]{start, x - 1});
                start = -1;
            }
        }
        // merge runs separated by small gaps, drop tiny ones
        List<int[]> merged = new ArrayList<>();
        for (int[] r : runs) {
            if (!merged.isEmpty() && r[0] - merged.get(merged.size() - 1)[1] < w * 0.04f)
                merged.get(merged.size() - 1)[1] = r[1];
            else merged.add(r);
        }
        // The backdrop often changes from top to bottom (floor, vignette), so the vertical
        // extent is measured against each row's own backdrop, taken from between the figures.
        boolean[] inRun = new boolean[w];
        for (int[] r : merged) {
            int pad = (r[1] - r[0] + 1) / 2 + w / 50;
            for (int x = Math.max(0, r[0] - pad); x <= Math.min(w - 1, r[1] + pad); x++) inRun[x] = true;
        }
        int free = 0;
        for (int x = 0; x < w; x++) if (!inRun[x]) free++;
        int[] rowBg = new int[h];
        int[] tr = new int[w], tg = new int[w], tb = new int[w];
        for (int y = 0; y < h; y++) {
            if (free < 8) {
                rowBg[y] = br << 16 | bg << 8 | bb;
                continue;
            }
            int k = 0;
            for (int x = 0; x < w; x++)
                if (!inRun[x]) {
                    int c = px[y * w + x];
                    tr[k] = (c >> 16) & 0xFF;
                    tg[k] = (c >> 8) & 0xFF;
                    tb[k++] = c & 0xFF;
                }
            Arrays.sort(tr, 0, k);
            Arrays.sort(tg, 0, k);
            Arrays.sort(tb, 0, k);
            rowBg[y] = tr[k / 2] << 16 | tg[k / 2] << 8 | tb[k / 2];
        }
        List<float[]> boxes = new ArrayList<>();
        for (int[] r : merged) {
            float mass = 0;
            for (int x = r[0]; x <= r[1]; x++) mass += colScore[x];
            int top = -1, bottom = -1;
            int x0 = Math.max(0, r[0] - (r[1] - r[0]) / 2), x1 = Math.min(w - 1, r[1] + (r[1] - r[0]) / 2);
            int minRow = Math.max(2, (r[1] - r[0] + 1) / 12);
            for (int y = 0; y < h; y++) {
                int c = 0, q = rowBg[y];
                for (int x = x0; x <= x1; x++) {
                    int p = px[y * w + x];
                    int d = Math.max(Math.abs(((p >> 16) & 0xFF) - ((q >> 16) & 0xFF)),
                            Math.max(Math.abs(((p >> 8) & 0xFF) - ((q >> 8) & 0xFF)), Math.abs((p & 0xFF) - (q & 0xFF))));
                    if (d > 28) c++;
                }
                if (c >= minRow) {
                    if (top < 0) top = y;
                    bottom = y;
                }
            }
            if (top < 0 || bottom - top < h * 0.15f) continue;
            boxes.add(new float[]{r[0], top, r[1], bottom, mass});
        }
        // keep the three heaviest, left to right
        while (boxes.size() > 3) {
            int worst = 0;
            for (int i = 1; i < boxes.size(); i++) if (boxes.get(i)[4] < boxes.get(worst)[4]) worst = i;
            boxes.remove(worst);
        }
        List<float[]> out = new ArrayList<>();
        for (float[] b : boxes) out.add(new float[]{b[0], b[1], b[2], b[3]});
        return out;
    }
}
