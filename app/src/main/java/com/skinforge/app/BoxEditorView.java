package com.skinforge.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/** Shows the reference picture with up to three draggable boxes framing the character views. */
public class BoxEditorView extends View {

    public static final int FRONT = 0, SIDE = 1, BACK = 2;
    private static final int[] COLORS = {0xFF58C46B, 0xFFF2B63C, 0xFF4FA3F7};
    private static final String[] LABELS = {"Спереди", "Сбоку", "Сзади"};

    private Bitmap bmp;
    private final float[][] boxes = new float[3][4];
    private final boolean[] on = {true, false, false};
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF dst = new RectF();
    private final float dp;
    private int dragBox = -1, dragMode; // mode: 0..3 corner (tl,tr,bl,br), 4 move
    private float lastX, lastY;

    public BoxEditorView(Context c) {
        super(c);
        dp = c.getResources().getDisplayMetrics().density;
    }

    public void setPicture(Bitmap b) {
        bmp = b;
        requestLayout();
        invalidate();
    }

    public void setBox(int i, float l, float t, float r, float b) {
        boxes[i][0] = l;
        boxes[i][1] = t;
        boxes[i][2] = r;
        boxes[i][3] = b;
        invalidate();
    }

    public float[] getBox(int i) {
        return boxes[i].clone();
    }

    public void setEnabled(int i, boolean e) {
        on[i] = e;
        invalidate();
    }

    public boolean isOn(int i) {
        return on[i];
    }

    @Override
    protected void onMeasure(int wSpec, int hSpec) {
        int w = MeasureSpec.getSize(wSpec);
        int h = (int) (120 * dp);
        if (bmp != null) {
            h = Math.round(w * (float) bmp.getHeight() / bmp.getWidth());
            int maxH = (int) (w * 1.5f);
            if (h > maxH) h = maxH;
        }
        setMeasuredDimension(w, h);
    }

    private float scale() {
        if (bmp == null) return 1;
        return Math.min(getWidth() / (float) bmp.getWidth(), getHeight() / (float) bmp.getHeight());
    }

    private float offX() {
        return bmp == null ? 0 : (getWidth() - bmp.getWidth() * scale()) / 2f;
    }

    @Override
    protected void onDraw(Canvas c) {
        if (bmp == null) {
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0xFF8B9099);
            paint.setTextSize(14 * dp);
            paint.setTextAlign(Paint.Align.CENTER);
            c.drawText("Картинка не выбрана", getWidth() / 2f, getHeight() / 2f, paint);
            return;
        }
        float s = scale(), ox = offX();
        dst.set(ox, 0, ox + bmp.getWidth() * s, bmp.getHeight() * s);
        paint.setFilterBitmap(true);
        c.drawBitmap(bmp, null, dst, paint);
        for (int i = 0; i < 3; i++) {
            if (!on[i]) continue;
            float l = ox + boxes[i][0] * s, t = boxes[i][1] * s, r = ox + boxes[i][2] * s, b = boxes[i][3] * s;
            paint.setColor(COLORS[i]);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(2 * dp);
            paint.setPathEffect(null);
            c.drawRect(l, t, r, b, paint);
            paint.setStrokeWidth(1 * dp);
            paint.setPathEffect(new DashPathEffect(new float[]{5 * dp, 5 * dp}, 0));
            c.drawLine((l + r) / 2, t, (l + r) / 2, b, paint);
            paint.setPathEffect(null);
            paint.setStyle(Paint.Style.FILL);
            float hr = 6 * dp;
            c.drawCircle(l, t, hr, paint);
            c.drawCircle(r, t, hr, paint);
            c.drawCircle(l, b, hr, paint);
            c.drawCircle(r, b, hr, paint);
            paint.setTextSize(12 * dp);
            paint.setTextAlign(Paint.Align.CENTER);
            float ty = t > 18 * dp ? t - 6 * dp : t + 16 * dp;
            paint.setColor(0xCC000000);
            float tw = paint.measureText(LABELS[i]) / 2 + 4 * dp;
            c.drawRect((l + r) / 2 - tw, ty - 12 * dp, (l + r) / 2 + tw, ty + 4 * dp, paint);
            paint.setColor(COLORS[i]);
            c.drawText(LABELS[i], (l + r) / 2, ty, paint);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (bmp == null) return false;
        float s = scale(), ox = offX();
        float x = (e.getX() - ox) / s, y = e.getY() / s;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: {
                dragBox = -1;
                float best = 30 * dp / s;
                for (int i = 0; i < 3; i++) {
                    if (!on[i]) continue;
                    float[] b = boxes[i];
                    float[][] corners = {{b[0], b[1]}, {b[2], b[1]}, {b[0], b[3]}, {b[2], b[3]}};
                    for (int k = 0; k < 4; k++) {
                        float d = (float) Math.hypot(x - corners[k][0], y - corners[k][1]);
                        if (d < best) {
                            best = d;
                            dragBox = i;
                            dragMode = k;
                        }
                    }
                }
                if (dragBox < 0) {
                    for (int i = 0; i < 3; i++) {
                        float[] b = boxes[i];
                        if (on[i] && x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3]) {
                            dragBox = i;
                            dragMode = 4;
                        }
                    }
                }
                if (dragBox < 0) return false;
                lastX = x;
                lastY = y;
                getParent().requestDisallowInterceptTouchEvent(true);
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                if (dragBox < 0) return false;
                float[] b = boxes[dragBox];
                float dx = x - lastX, dy = y - lastY;
                float min = 24 * dp / s;
                if (dragMode == 4) {
                    b[0] += dx;
                    b[2] += dx;
                    b[1] += dy;
                    b[3] += dy;
                } else {
                    if (dragMode == 0 || dragMode == 2) b[0] = Math.min(b[0] + dx, b[2] - min);
                    else b[2] = Math.max(b[2] + dx, b[0] + min);
                    if (dragMode == 0 || dragMode == 1) b[1] = Math.min(b[1] + dy, b[3] - min);
                    else b[3] = Math.max(b[3] + dy, b[1] + min);
                }
                lastX = x;
                lastY = y;
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                dragBox = -1;
                getParent().requestDisallowInterceptTouchEvent(false);
                return true;
        }
        return false;
    }
}
