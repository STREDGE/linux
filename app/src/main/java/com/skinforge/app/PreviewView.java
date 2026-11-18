package com.skinforge.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

import com.skinforge.core.Projector;
import com.skinforge.core.RwModel;

import java.util.Map;

/** Shows the textured model; dragging sideways turns it around. */
public class PreviewView extends View {

    private RwModel model;
    private Map<String, Projector.Tex> textures;
    private Projector.Frame frame;
    private float yaw = 25f;
    private Bitmap bmp;
    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final RectF dst = new RectF();
    private final float dp;
    private float lastX;

    public PreviewView(Context c) {
        super(c);
        dp = c.getResources().getDisplayMetrics().density;
    }

    public void show(RwModel m, Map<String, Projector.Tex> tex, Projector.Frame f) {
        model = m;
        textures = tex;
        frame = f;
        yaw = 25f;
        redraw();
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        redraw();
    }

    private void redraw() {
        int w = getWidth(), h = getHeight();
        if (model == null || w <= 0 || h <= 0) return;
        // render at reduced resolution so turning stays smooth on any phone
        int rh = Math.min(h, 640), rw = Math.max(1, Math.round(rh * (float) w / h));
        if (bmp == null || bmp.getWidth() != rw || bmp.getHeight() != rh)
            bmp = Bitmap.createBitmap(rw, rh, Bitmap.Config.ARGB_8888);
        bmp.setPixels(Projector.render(model, textures, frame, yaw, rw, rh), 0, rw, 0, 0, rw, rh);
        invalidate();
    }

    @Override
    protected void onDraw(Canvas c) {
        if (bmp == null) return;
        dst.set(0, 0, getWidth(), getHeight());
        c.drawBitmap(bmp, null, dst, paint);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (model == null) return false;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                lastX = e.getX();
                getParent().requestDisallowInterceptTouchEvent(true);
                return true;
            case MotionEvent.ACTION_MOVE:
                yaw -= (e.getX() - lastX) / dp * 0.6f;
                lastX = e.getX();
                redraw();
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                getParent().requestDisallowInterceptTouchEvent(false);
                return true;
        }
        return false;
    }
}
