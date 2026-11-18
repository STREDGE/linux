import com.skinforge.core.*;

import javax.imageio.ImageIO;
import java.awt.image.BufferedImage;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.util.*;

/**
 * Desktop check of the core pipeline:
 *   java DesktopTest picture.png outDir [model.dff]
 * Without a model, a synthetic humanoid DFF is generated and used.
 */
public class DesktopTest {
    public static void main(String[] a) throws Exception {
        BufferedImage img = ImageIO.read(new File(a[0]));
        File out = new File(a[1]);
        out.mkdirs();
        int w = img.getWidth(), h = img.getHeight();
        int[] px = img.getRGB(0, 0, w, h, null, 0, w);

        byte[] dff = a.length > 2 ? Files.readAllBytes(new File(a[2]).toPath()) : dummy();
        Files.write(new File(out, "base.dff").toPath(), dff);
        RwModel m = RwModel.parse(dff);
        System.out.println("geoms=" + m.geoms.size() + " verts=" + m.geoms.get(0).numVerts
                + " tris=" + m.geoms.get(0).numTris + " textures=" + m.textureNames());

        List<float[]> boxes = AutoSplit.detect(px, w, h);
        for (float[] b : boxes) System.out.println("box " + Arrays.toString(b));
        if (a.length > 3) { // manual boxes: l,t,r,b;l,t,r,b;...
            boxes = new ArrayList<>();
            for (String s : a[3].split(";")) {
                String[] p = s.split(",");
                boxes.add(new float[]{Float.parseFloat(p[0]), Float.parseFloat(p[1]), Float.parseFloat(p[2]), Float.parseFloat(p[3])});
            }
        }
        Projector.View front = view(px, w, h, boxes.get(0));
        Projector.View side = boxes.size() > 2 ? view(px, w, h, boxes.get(1)) : null;
        Projector.View back = boxes.size() > 1 ? view(px, w, h, boxes.get(boxes.size() - 1)) : null;
        Projector.Options o = new Projector.Options();
        o.size = 1024;
        long t0 = System.currentTimeMillis();
        LinkedHashMap<String, Projector.Tex> tex = Projector.project(m, front, side, back, o);
        System.out.println("project ms=" + (System.currentTimeMillis() - t0));
        for (Map.Entry<String, Projector.Tex> e : tex.entrySet()) save(e.getValue().px, e.getValue().w, e.getValue().h, new File(out, e.getKey() + ".png"));
        Files.write(new File(out, "skin.txd").toPath(), TxdWriter.write(tex));

        Projector.Frame f = Projector.analyze(m, false);
        System.out.println("up=" + Arrays.toString(f.up) + " front=" + Arrays.toString(f.front) + " left=" + Arrays.toString(f.left));
        int pw = 360, ph = 720;
        float[] yaws = {0, 40, 90, 180, 270};
        BufferedImage sheet = new BufferedImage(pw * yaws.length, ph, BufferedImage.TYPE_INT_RGB);
        for (int i = 0; i < yaws.length; i++)
            sheet.setRGB(i * pw, 0, pw, ph, Projector.render(m, tex, f, yaws[i], pw, ph), 0, pw);
        ImageIO.write(sheet, "png", new File(out, "preview.png"));
    }

    static Projector.View view(int[] px, int w, int h, float[] b) {
        return new Projector.View(px, w, h, b[0], b[1], b[2], b[3]);
    }

    static void save(int[] px, int w, int h, File f) throws IOException {
        BufferedImage im = new BufferedImage(w, h, BufferedImage.TYPE_INT_RGB);
        im.setRGB(0, 0, w, h, px, 0, w);
        ImageIO.write(im, "png", f);
    }

    // ---- synthetic humanoid, written as a real RenderWare clump ----

    static List<float[]> V = new ArrayList<>();
    static List<int[]> T = new ArrayList<>();

    static void part(double cx, double cy, double cz, double rx, double ry, double rz, int cell) {
        int rings = 14, segs = 20, base = V.size();
        double u0 = (cell % 4) / 4.0, v0 = (cell / 4) / 2.0;
        for (int r = 0; r <= rings; r++) {
            double th = Math.PI * r / rings;
            for (int s = 0; s <= segs; s++) {
                double ph = 2 * Math.PI * s / segs;
                // superellipsoid-ish: flatter than a sphere so limbs look like capsules
                double st = Math.pow(Math.sin(th), 0.6);
                V.add(new float[]{(float) (cx + rx * st * Math.cos(ph)), (float) (cy + ry * st * Math.sin(ph)),
                        (float) (cz + rz * Math.cos(th)),
                        (float) (u0 + 0.01 + 0.23 * s / segs), (float) (v0 + 0.01 + 0.48 * r / rings)});
            }
        }
        for (int r = 0; r < rings; r++)
            for (int s = 0; s < segs; s++) {
                int i = base + r * (segs + 1) + s;
                T.add(new int[]{i, i + segs + 1, i + 1});
                T.add(new int[]{i + 1, i + segs + 1, i + segs + 2});
            }
    }

    static byte[] dummy() throws IOException {
        part(0, 0, 1.67, 0.095, 0.11, 0.13, 0);          // head
        part(0, 0, 1.18, 0.20, 0.12, 0.38, 1);           // torso
        part(-0.31, 0, 1.13, 0.055, 0.06, 0.38, 2);      // arms
        part(0.31, 0, 1.13, 0.055, 0.06, 0.38, 3);
        part(-0.10, 0, 0.47, 0.085, 0.09, 0.45, 4);      // legs
        part(0.10, 0, 0.47, 0.085, 0.09, 0.45, 5);
        part(-0.10, 0.07, 0.04, 0.05, 0.13, 0.04, 6);    // feet, toes towards +Y
        part(0.10, 0.07, 0.04, 0.05, 0.13, 0.04, 7);
        int nv = V.size(), nt = T.size();
        ByteBuffer g = ByteBuffer.allocate(16 + 8 * nv + 8 * nt + 24 + 12 * nv).order(ByteOrder.LITTLE_ENDIAN);
        g.putInt(0x00010064).putInt(nt).putInt(nv).putInt(1);
        for (float[] v : V) g.putFloat(v[3]).putFloat(v[4]);
        for (int[] t : T) g.putShort((short) t[1]).putShort((short) t[0]).putShort((short) 0).putShort((short) t[2]);
        g.putFloat(0).putFloat(0).putFloat(0.9f).putFloat(1f).putInt(1).putInt(0);
        for (float[] v : V) g.putFloat(v[0]).putFloat(v[1]).putFloat(v[2]);

        byte[] name = chunk(2, "dummy\0\0\0".getBytes()), mask = chunk(2, "\0\0\0\0".getBytes());
        byte[] texture = chunk(6, cat(chunk(1, new byte[4]), name, mask, chunk(3, new byte[0])));
        ByteBuffer ms = ByteBuffer.allocate(28).order(ByteOrder.LITTLE_ENDIAN);
        ms.putInt(0).putInt(-1).putInt(0).putInt(1).putFloat(1).putFloat(1).putFloat(1);
        byte[] material = chunk(7, cat(chunk(1, ms.array()), texture, chunk(3, new byte[0])));
        ByteBuffer ml = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
        ml.putInt(1).putInt(-1);
        byte[] matList = chunk(8, cat(chunk(1, ml.array()), material));
        byte[] geometry = chunk(0x0F, cat(chunk(1, g.array()), matList, chunk(3, new byte[0])));
        ByteBuffer gl = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN);
        gl.putInt(1);
        byte[] geomList = chunk(0x1A, cat(chunk(1, gl.array()), geometry));
        return chunk(0x10, cat(chunk(1, new byte[12]), geomList, chunk(3, new byte[0])));
    }

    static byte[] chunk(int type, byte[] d) {
        ByteBuffer b = ByteBuffer.allocate(12 + d.length).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(type).putInt(d.length).putInt(0x1803FFFF).put(d);
        return b.array();
    }

    static byte[] cat(byte[]... p) {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        for (byte[] x : p) o.write(x, 0, x.length);
        return o.toByteArray();
    }
}
