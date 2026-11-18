package com.skinforge.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.text.InputFilter;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

import com.skinforge.core.AutoSplit;
import com.skinforge.core.ImgArchive;
import com.skinforge.core.Projector;
import com.skinforge.core.RwModel;
import com.skinforge.core.TexDb;
import com.skinforge.core.TxdWriter;

import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class MainActivity extends Activity {

    private static final int REQ_PICTURE = 1, REQ_DFF = 2, REQ_FOLDER = 3, REQ_TEXDB = 4;

    /** CJ's default look: body parts and starting clothes, with the cache textures each one uses. */
    private static final String[][] CJ_PARTS = {
            {"head", "player_face_face", "face", "wedge_face", "head_head8bit"},
            {"vest", "vest_vest"},
            {"jeans", "jeansdenim_legs"},
            {"sneaker", "sneakerbincblk_sneakerbincblk"},
            {"torso", "player_torso_torso", "player_torso_torso_fat", "player_torso_torso_ripped", "torso_torso", "torso8bit"},
            {"legs", "player_legs_legs", "legs_legs"},
            {"feet", "player_feet_foot", "feet_feet8bit"},
    };
    private static final int[] SIZES = {512, 1024};

    private float dp;
    private final Handler ui = new Handler(Looper.getMainLooper());

    private BoxEditorView editor;
    private CheckBox sideOn, backOn, flipFront, sideRight;
    private TextView dffStatus, widthLabel, resultStatus, saveStatus;
    private Spinner sizeSpinner;
    private SeekBar widthBar;
    private EditText nameEdit;
    private Button makeButton, saveButton;
    private ProgressBar progress;
    private ImageView previewImage, textureImage;
    private TextView textureCaption;

    private Bitmap picture;
    private int[] picturePx;
    private final List<String> partNames = new ArrayList<>();
    private final List<byte[]> partBytes = new ArrayList<>();
    private RwModel model;
    private LinkedHashMap<String, Projector.Tex> result;

    // "replace CJ" mode: the game's texdb folder picked by the user
    private boolean cjMode;
    private Uri cjTree;
    private String cjPlayerDirId;
    private TexDb cjDb;
    private final LinkedHashMap<String, List<String>> cjTargets = new LinkedHashMap<>();
    private final HashMap<String, int[]> cjDims = new HashMap<>();
    private boolean busy;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        dp = getResources().getDisplayMetrics().density;
        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = px(16);
        root.setPadding(pad, pad, pad, px(32));
        scroll.addView(root);
        setContentView(scroll);

        // ---- 1. picture
        root.addView(heading("1. Картинка персонажа"));
        root.addView(note("Лучше всего подходит развёртка: персонаж в полный рост спереди, сбоку и сзади. "
                + "Хватит и одного вида спереди."));
        Button pick = button("Выбрать картинку");
        pick.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                Intent i = new Intent(Intent.ACTION_GET_CONTENT);
                i.setType("image/*");
                i.addCategory(Intent.CATEGORY_OPENABLE);
                startActivityForResult(i, REQ_PICTURE);
            }
        });
        root.addView(pick);
        editor = new BoxEditorView(this);
        LinearLayout.LayoutParams elp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        elp.topMargin = px(8);
        root.addView(editor, elp);
        root.addView(note("Подгоните рамки: верх — по макушке, низ — по подошвам, пунктир — по середине тела. "
                + "Тяните за углы или двигайте рамку целиком."));
        sideOn = check("Есть вид сбоку", false);
        backOn = check("Есть вид сзади", false);
        root.addView(sideOn);
        root.addView(backOn);
        sideOn.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            public void onCheckedChanged(CompoundButton b, boolean on) {
                enableView(BoxEditorView.SIDE, on);
            }
        });
        backOn.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            public void onCheckedChanged(CompoundButton b, boolean on) {
                enableView(BoxEditorView.BACK, on);
            }
        });

        // ---- 2. base skin
        root.addView(heading("2. Базовый скин"));
        root.addView(note("Чтобы заменить самого Си-Джея, укажите папку texdb игры (или её копию): "
                + "в ней должны лежать player.img и папка player. Приложение само возьмёт голову, "
                + "майку, джинсы и кроссовки, в которых Си-Джей начинает игру."));
        Button pickTexdb = button("Заменить Си-Джея: выбрать папку texdb");
        pickTexdb.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_TEXDB);
            }
        });
        root.addView(pickTexdb);
        root.addView(note("Или свой скин: отдельный файл .dff либо архив .img, из которого вы сами выберете модели."));
        Button pickDff = button("Свой скин: выбрать .dff или .img");
        pickDff.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.setType("*/*");
                i.addCategory(Intent.CATEGORY_OPENABLE);
                startActivityForResult(i, REQ_DFF);
            }
        });
        root.addView(pickDff);
        dffStatus = note("Файл не выбран");
        root.addView(dffStatus);

        // ---- 3. settings
        root.addView(heading("3. Настройки"));
        root.addView(label("Имя скина (так будут называться файлы)"));
        nameEdit = new EditText(this);
        nameEdit.setSingleLine(true);
        nameEdit.setText("myskin");
        nameEdit.setFilters(new InputFilter[]{new InputFilter.LengthFilter(20)});
        root.addView(nameEdit);
        root.addView(label("Качество текстур"));
        sizeSpinner = new Spinner(this);
        ArrayAdapter<String> ad = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item,
                new String[]{"Обычное качество — легче для игры", "Высокое качество — чётче"});
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sizeSpinner.setAdapter(ad);
        sizeSpinner.setSelection(1);
        root.addView(sizeSpinner);
        widthLabel = label("Ширина наложения: 100%");
        root.addView(widthLabel);
        widthBar = new SeekBar(this);
        widthBar.setMax(80);
        widthBar.setProgress(40);
        widthBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                widthLabel.setText("Ширина наложения: " + (60 + p) + "%");
            }

            public void onStartTrackingTouch(SeekBar s) {
            }

            public void onStopTrackingTouch(SeekBar s) {
            }
        });
        root.addView(widthBar);
        root.addView(note("Меньше 100% — если картинка на модели выглядит растянутой вширь, больше — если сжатой."));
        sideRight = check("На виде сбоку персонаж смотрит вправо", true);
        root.addView(sideRight);
        flipFront = check("Развернуть модель (если лицо оказалось на спине)", false);
        root.addView(flipFront);

        // ---- 4. make
        makeButton = button("Создать скин");
        LinearLayout.LayoutParams mlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        mlp.topMargin = px(20);
        root.addView(makeButton, mlp);
        makeButton.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                make();
            }
        });
        progress = new ProgressBar(this);
        progress.setVisibility(View.GONE);
        root.addView(progress);
        resultStatus = note("");
        root.addView(resultStatus);
        previewImage = new ImageView(this);
        previewImage.setAdjustViewBounds(true);
        root.addView(previewImage, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        textureCaption = label("Текстура");
        textureCaption.setVisibility(View.GONE);
        root.addView(textureCaption);
        textureImage = new ImageView(this);
        textureImage.setAdjustViewBounds(true);
        root.addView(textureImage, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        // ---- 5. save
        saveButton = button("Сохранить файлы мода");
        saveButton.setVisibility(View.GONE);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        slp.topMargin = px(16);
        root.addView(saveButton, slp);
        saveButton.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_FOLDER);
            }
        });
        saveStatus = note("");
        root.addView(saveStatus);
    }

    // ------------------------------------------------------------ UI helpers

    private int px(float v) {
        return Math.round(v * dp);
    }

    private TextView heading(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(18);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setTextColor(0xFFF2F2F2);
        t.setPadding(0, px(22), 0, px(4));
        return t;
    }

    private TextView label(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(14);
        t.setTextColor(0xFFD5D8DD);
        t.setPadding(0, px(12), 0, px(2));
        return t;
    }

    private TextView note(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(13);
        t.setTextColor(0xFF9BA1AB);
        t.setPadding(0, px(4), 0, px(6));
        return t;
    }

    private Button button(String s) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        return b;
    }

    private CheckBox check(String s, boolean on) {
        CheckBox c = new CheckBox(this);
        c.setText(s);
        c.setChecked(on);
        return c;
    }

    /** Turns a view box on; gives it a sensible place if it has never been set. */
    private void enableView(int which, boolean on) {
        if (on && picture != null) {
            float[] b = editor.getBox(which);
            if (b[2] - b[0] < 1) {
                float[] f = editor.getBox(BoxEditorView.FRONT);
                float w = f[2] - f[0];
                float cx = picture.getWidth() * (which == BoxEditorView.SIDE ? 0.5f : 0.8f);
                editor.setBox(which, cx - w / 2, f[1], cx + w / 2, f[3]);
            }
        }
        editor.setEnabled(which, on);
    }

    // ---------------------------------------------------------------- inputs

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        if (req == REQ_PICTURE) loadPicture(uri);
        else if (req == REQ_DFF) loadDff(uri);
        else if (req == REQ_FOLDER) save(uri);
        else if (req == REQ_TEXDB) loadTexdb(uri);
    }

    private void loadPicture(Uri uri) {
        try {
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inJustDecodeBounds = true;
            InputStream in = getContentResolver().openInputStream(uri);
            BitmapFactory.decodeStream(in, null, o);
            in.close();
            int sample = 1;
            while (Math.max(o.outWidth, o.outHeight) / sample > 2600) sample *= 2;
            o = new BitmapFactory.Options();
            o.inSampleSize = sample;
            o.inPreferredConfig = Bitmap.Config.ARGB_8888;
            in = getContentResolver().openInputStream(uri);
            Bitmap b = BitmapFactory.decodeStream(in, null, o);
            in.close();
            if (b == null) throw new Exception("формат не распознан");
            picture = b;
            int w = b.getWidth(), h = b.getHeight();
            picturePx = new int[w * h];
            b.getPixels(picturePx, 0, w, 0, 0, w, h);
            editor.setPicture(b);

            List<float[]> boxes = AutoSplit.detect(picturePx, w, h);
            for (int i = 0; i < 3; i++) editor.setBox(i, 0, 0, 0, 0);
            if (boxes.isEmpty()) {
                editor.setBox(BoxEditorView.FRONT, w * 0.2f, h * 0.04f, w * 0.8f, h * 0.96f);
                sideOn.setChecked(false);
                backOn.setChecked(false);
            } else {
                placeBox(BoxEditorView.FRONT, boxes.get(0), w);
                if (boxes.size() == 3) placeBox(BoxEditorView.SIDE, boxes.get(1), w);
                if (boxes.size() >= 2) placeBox(BoxEditorView.BACK, boxes.get(boxes.size() - 1), w);
                sideOn.setChecked(boxes.size() == 3);
                backOn.setChecked(boxes.size() >= 2);
            }
            editor.setEnabled(BoxEditorView.SIDE, sideOn.isChecked());
            editor.setEnabled(BoxEditorView.BACK, backOn.isChecked());
        } catch (Throwable e) {
            toastNote(resultStatus, "Не удалось открыть картинку: " + message(e));
        }
    }

    /** Detected boxes are narrow (often just the head); widen them so they are easy to grab. */
    private void placeBox(int which, float[] b, int picW) {
        float cx = (b[0] + b[2]) / 2, half = Math.max((b[2] - b[0]) / 2, (b[3] - b[1]) * 0.2f);
        editor.setBox(which, cx - half, b[1], cx + half, b[3]);
    }

    private void loadDff(final Uri uri) {
        try {
            byte[] head = readRange(uri, 0, 8);
            if (ImgArchive.isImg(head)) {
                final ImgArchive img = ImgArchive.parse(readRange(uri, 0, ImgArchive.directorySize(head)));
                final List<ImgArchive.Entry> dffs = new ArrayList<>();
                for (ImgArchive.Entry e : img.entries)
                    if (e.name.toLowerCase().endsWith(".dff") && e.size > 0) dffs.add(e);
                if (dffs.isEmpty()) throw new Exception("в архиве нет моделей .dff");
                Collections.sort(dffs, new Comparator<ImgArchive.Entry>() {
                    public int compare(ImgArchive.Entry x, ImgArchive.Entry y) {
                        return x.name.compareToIgnoreCase(y.name);
                    }
                });
                // the default body of CJ is made of these four parts
                List<String> body = Arrays.asList("head.dff", "torso.dff", "legs.dff", "feet.dff");
                String[] names = new String[dffs.size()];
                final boolean[] picked = new boolean[dffs.size()];
                for (int i = 0; i < names.length; i++) {
                    names[i] = dffs.get(i).name;
                    picked[i] = body.contains(names[i].toLowerCase());
                }
                new AlertDialog.Builder(this)
                        .setTitle("Какие модели перекрасить?")
                        .setMultiChoiceItems(names, picked, new DialogInterface.OnMultiChoiceClickListener() {
                            public void onClick(DialogInterface d, int which, boolean on) {
                                picked[which] = on;
                            }
                        })
                        .setNegativeButton("Отмена", null)
                        .setPositiveButton("Готово", new DialogInterface.OnClickListener() {
                            public void onClick(DialogInterface d, int w) {
                                try {
                                    List<String> n = new ArrayList<>();
                                    List<byte[]> b = new ArrayList<>();
                                    for (int i = 0; i < picked.length; i++)
                                        if (picked[i]) {
                                            ImgArchive.Entry e = dffs.get(i);
                                            if (e.size > 48 * 1024 * 1024) throw new Exception("модель слишком большая");
                                            n.add(e.name);
                                            b.add(readRange(uri, e.offset, e.size));
                                        }
                                    if (n.isEmpty()) throw new Exception("не выбрано ни одной модели");
                                    useModels(n, b, displayName(uri));
                                } catch (Throwable e) {
                                    dffError(e);
                                }
                            }
                        })
                        .show();
                return;
            }
            String file = displayName(uri);
            List<String> n = new ArrayList<>();
            List<byte[]> b = new ArrayList<>();
            n.add(file.toLowerCase().endsWith(".dff") ? file : file + ".dff");
            b.add(readAll(uri, 64 * 1024 * 1024));
            useModels(n, b, null);
        } catch (Throwable e) {
            dffError(e);
        }
    }

    /** Parses the chosen models and joins them into one body for projection. */
    private void useModels(List<String> names, List<byte[]> bytes, String archive) throws Exception {
        RwModel all = new RwModel();
        int verts = 0, tris = 0;
        StringBuilder skipped = new StringBuilder();
        List<String> okNames = new ArrayList<>();
        List<byte[]> okBytes = new ArrayList<>();
        for (int i = 0; i < names.size(); i++) {
            try {
                RwModel m = RwModel.parse(bytes.get(i));
                boolean hasUv = false;
                for (RwModel.Geom g : m.geoms) if (g.uv != null) hasUv = true;
                if (!hasUv) throw new Exception("нет текстурной развёртки (UV)");
                m.nameUntextured(defaultTexture(names.get(i)));
                for (RwModel.Geom g : m.geoms) {
                    verts += g.numVerts;
                    tris += g.numTris;
                }
                all.geoms.addAll(m.geoms);
                okNames.add(names.get(i));
                okBytes.add(bytes.get(i));
            } catch (Throwable e) {
                skipped.append("\n").append(names.get(i)).append(" пропущен: ").append(message(e));
            }
        }
        if (all.geoms.isEmpty()) throw new Exception("ни одна модель не подошла" + skipped);
        cjMode = false;
        makeButton.setText("Создать скин");
        partNames.clear();
        partNames.addAll(okNames);
        partBytes.clear();
        partBytes.addAll(okBytes);
        model = all;
        StringBuilder sb = new StringBuilder();
        if (archive != null) sb.append(archive).append(": ");
        for (int i = 0; i < okNames.size(); i++) sb.append(i > 0 ? ", " : "").append(okNames.get(i));
        sb.append(" — ").append(verts).append(" вершин, ").append(tris).append(" треугольников.\nТекстуры: ");
        List<String> tex = all.textureNames();
        for (int i = 0; i < tex.size(); i++) sb.append(i > 0 ? ", " : "").append(tex.get(i));
        sb.append(skipped);
        dffStatus.setText(sb.toString());
        dffStatus.setTextColor(0xFF9BD3A5);
        if (okNames.size() == 1 && "myskin".equals(nameEdit.getText().toString())) {
            String f = okNames.get(0);
            nameEdit.setText(f.substring(0, f.length() - 4));
        }
    }

    /** Texture a model uses when its file does not name one. */
    private static String defaultTexture(String file) {
        String base = file.toLowerCase();
        if (base.endsWith(".dff")) base = base.substring(0, base.length() - 4);
        if (base.equals("head")) return "player_face";
        if (base.equals("torso")) return "player_torso";
        if (base.equals("legs")) return "player_legs";
        return base;
    }

    // ------------------------------------------------------- replace CJ mode

    /** Document id of a child of a folder inside the picked tree, by name (case-insensitive). */
    private String childId(Uri tree, String dirId, String name) {
        for (String[] c : listDir(tree, dirId)) if (c[1].equalsIgnoreCase(name)) return c[0];
        return null;
    }

    /** Children of a folder inside the picked tree as {document id, name}. */
    private List<String[]> listDir(Uri tree, String dirId) {
        List<String[]> out = new ArrayList<>();
        Cursor c = getContentResolver().query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, dirId),
                new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME},
                null, null, null);
        if (c != null) {
            while (c.moveToNext()) out.add(new String[]{c.getString(0), c.getString(1)});
            c.close();
        }
        return out;
    }

    private void loadTexdb(Uri tree) {
        try {
            String dirId = DocumentsContract.getTreeDocumentId(tree);
            String inner = childId(tree, dirId, "texdb");
            if (inner != null) dirId = inner;
            String imgId = childId(tree, dirId, "player.img");
            String playerId = childId(tree, dirId, "player");
            if (imgId == null) throw new Exception("в выбранной папке нет файла player.img");
            if (playerId == null) throw new Exception("в выбранной папке нет папки player с кэшем текстур");
            String txtId = childId(tree, playerId, "player.txt");
            if (txtId == null) throw new Exception("в папке player нет файла player.txt");
            boolean anySet = false;
            for (String[] c : listDir(tree, playerId))
                if (c[1].toLowerCase().matches("player\\.[a-z0-9]+\\.dat")) anySet = true;
            if (!anySet) throw new Exception("в папке player нет файлов player.*.dat");

            TexDb db = TexDb.parseTxt(readAll(DocumentsContract.buildDocumentUriUsingTree(tree, txtId), 8 * 1024 * 1024));
            Uri img = DocumentsContract.buildDocumentUriUsingTree(tree, imgId);
            byte[] head = readRange(img, 0, 8);
            if (!ImgArchive.isImg(head)) throw new Exception("player.img не похож на архив игры");
            ImgArchive arc = ImgArchive.parse(readRange(img, 0, ImgArchive.directorySize(head)));

            RwModel all = new RwModel();
            LinkedHashMap<String, List<String>> targets = new LinkedHashMap<>();
            HashMap<String, int[]> dims = new HashMap<>();
            List<String> taken = new ArrayList<>();
            StringBuilder found = new StringBuilder(), skipped = new StringBuilder();
            for (String[] part : CJ_PARTS) {
                String key = part[0];
                ImgArchive.Entry e = null;
                for (ImgArchive.Entry x : arc.entries) if (x.name.equalsIgnoreCase(key + ".dff")) e = x;
                List<String> names = new ArrayList<>();
                int[] d = null;
                for (int i = 1; i < part.length; i++) {
                    TexDb.Entry t = db.resolve(part[i]);
                    if (t == null || t.width <= 0 || t.height <= 0 || taken.contains(t.name.toLowerCase())) continue;
                    taken.add(t.name.toLowerCase());
                    names.add(t.name);
                    if (d == null) d = new int[]{t.width, t.height};
                }
                if (e == null || names.isEmpty()) {
                    skipped.append("\n").append(key).append(e == null ? ": нет модели в player.img" : ": нет текстуры в player.txt");
                    continue;
                }
                try {
                    if (e.size > 48 * 1024 * 1024) throw new Exception("модель слишком большая");
                    RwModel m = RwModel.parse(readRange(img, e.offset, e.size));
                    boolean hasUv = false;
                    for (RwModel.Geom g : m.geoms) if (g.uv != null) hasUv = true;
                    if (!hasUv) throw new Exception("нет текстурной развёртки (UV)");
                    m.forceTexture(key);
                    all.geoms.addAll(m.geoms);
                    targets.put(key, names);
                    dims.put(key, d);
                    found.append(found.length() > 0 ? ", " : "").append(key);
                } catch (Throwable ex) {
                    skipped.append("\n").append(key).append(": ").append(message(ex));
                }
            }
            if (all.geoms.isEmpty()) throw new Exception("не удалось взять ни одной части Си-Джея" + skipped);

            cjMode = true;
            cjTree = tree;
            cjPlayerDirId = playerId;
            cjDb = db;
            cjTargets.clear();
            cjTargets.putAll(targets);
            cjDims.clear();
            cjDims.putAll(dims);
            model = all;
            partNames.clear();
            partBytes.clear();
            makeButton.setText("Заменить Си-Джея");
            saveButton.setVisibility(View.GONE);
            dffStatus.setTextColor(0xFF9BD3A5);
            dffStatus.setText("Папка игры прочитана. Части Си-Джея: " + found
                    + (skipped.length() > 0 ? "\nПропущено:" + skipped : ""));
        } catch (Throwable e) {
            dffError(e);
        }
    }

    /** Writes a patched copy of the player texture cache next to the original; returns the new folder's name. */
    private String saveCj(LinkedHashMap<String, Projector.Tex> tex) throws Exception {
        HashMap<String, Projector.Tex> replace = new HashMap<>();
        for (Map.Entry<String, List<String>> e : cjTargets.entrySet()) {
            Projector.Tex t = tex.get(e.getKey());
            if (t == null) continue;
            for (String name : e.getValue()) replace.put(name, t);
        }
        if (replace.isEmpty()) throw new Exception("нет текстур для замены");
        HashMap<String, String> files = new HashMap<>();
        for (String[] c : listDir(cjTree, cjPlayerDirId)) files.put(c[1].toLowerCase(), c[0]);

        Uri root = DocumentsContract.buildDocumentUriUsingTree(cjTree, DocumentsContract.getTreeDocumentId(cjTree));
        Uri outRoot = DocumentsContract.createDocument(getContentResolver(), root,
                DocumentsContract.Document.MIME_TYPE_DIR, "SkinForge_CJ");
        if (outRoot == null) throw new Exception("не удалось создать папку для результата");
        Uri outDir = DocumentsContract.createDocument(getContentResolver(), outRoot,
                DocumentsContract.Document.MIME_TYPE_DIR, "player");
        if (outDir == null) throw new Exception("не удалось создать папку player");
        int sets = 0;
        for (Map.Entry<String, String> f : files.entrySet()) {
            String name = f.getKey();
            if (!name.matches("player\\.[a-z0-9]+\\.dat")) continue;
            String base = name.substring(0, name.length() - 4);
            String tocId = files.get(base + ".toc"), tmbId = files.get(base + ".tmb");
            if (tocId == null) continue;
            byte[] dat = readAll(DocumentsContract.buildDocumentUriUsingTree(cjTree, f.getValue()), 400 * 1024 * 1024);
            byte[] toc = readAll(DocumentsContract.buildDocumentUriUsingTree(cjTree, tocId), 16 * 1024 * 1024);
            byte[] tmb = tmbId == null ? null
                    : readAll(DocumentsContract.buildDocumentUriUsingTree(cjTree, tmbId), 64 * 1024 * 1024);
            byte[][] r = cjDb.rebuild(dat, toc, tmb, replace);
            dat = null;
            write(outDir, "application/octet-stream", base + ".dat", r[0]);
            write(outDir, "application/octet-stream", base + ".toc", r[1]);
            if (r[2] != null) write(outDir, "application/octet-stream", base + ".tmb", r[2]);
            sets++;
        }
        if (sets == 0) throw new Exception("не найдено ни одного набора player.*.dat с оглавлением .toc");
        write(outDir, "application/octet-stream", "player.txt", cjDb.writeTxt(replace));
        return displayName(outRoot);
    }

    private void dffError(Throwable e) {
        cjMode = false;
        makeButton.setText("Создать скин");
        partNames.clear();
        partBytes.clear();
        model = null;
        dffStatus.setText("Не удалось прочитать модель: " + message(e));
        dffStatus.setTextColor(0xFFFF8A80);
    }

    /** Reads part of a file without loading the rest (archives can be about a gigabyte). */
    private byte[] readRange(Uri uri, long offset, int length) throws Exception {
        ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
        if (pfd == null) throw new Exception("файл недоступен");
        FileInputStream in = new FileInputStream(pfd.getFileDescriptor());
        try {
            in.getChannel().position(offset);
            byte[] out = new byte[length];
            int got = 0;
            while (got < length) {
                int n = in.read(out, got, length - got);
                if (n <= 0) break;
                got += n;
            }
            if (got < Math.min(length, 8)) throw new Exception("файл обрезан");
            return out;
        } finally {
            in.close();
            pfd.close();
        }
    }

    private byte[] readAll(Uri uri, int limit) throws Exception {
        InputStream in = getContentResolver().openInputStream(uri);
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                if (out.size() > limit) throw new Exception("файл слишком большой");
            }
            return out.toByteArray();
        } finally {
            in.close();
        }
    }

    private String displayName(Uri uri) {
        String name = null;
        try {
            Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null);
            if (c != null) {
                if (c.moveToFirst()) name = c.getString(0);
                c.close();
            }
        } catch (Exception ignored) {
        }
        return name == null ? "model.dff" : name;
    }

    private static String message(Throwable e) {
        if (e instanceof OutOfMemoryError) return "не хватило памяти — выберите текстуру 512";
        return e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
    }

    private void toastNote(TextView t, String s) {
        t.setText(s);
        t.setTextColor(0xFFFF8A80);
    }

    // ------------------------------------------------------------------ make

    private void make() {
        if (busy) return;
        if (picture == null) {
            toastNote(resultStatus, "Сначала выберите картинку персонажа (шаг 1).");
            return;
        }
        if (model == null) {
            toastNote(resultStatus, "Сначала выберите базовый скин .dff (шаг 2).");
            return;
        }
        final int w = picture.getWidth(), h = picture.getHeight();
        final Projector.View front = view(BoxEditorView.FRONT, w, h);
        final Projector.View side = sideOn.isChecked() ? view(BoxEditorView.SIDE, w, h) : null;
        final Projector.View back = backOn.isChecked() ? view(BoxEditorView.BACK, w, h) : null;
        final Projector.Options o = new Projector.Options();
        final boolean high = sizeSpinner.getSelectedItemPosition() == 1;
        o.size = SIZES[high ? 1 : 0];
        final boolean cj = cjMode;
        final HashMap<String, int[]> sizes = new HashMap<>();
        if (cj) {
            int mul = high ? 4 : 2;
            for (Map.Entry<String, int[]> e : cjDims.entrySet())
                sizes.put(e.getKey(), new int[]{Math.min(1024, e.getValue()[0] * mul), Math.min(1024, e.getValue()[1] * mul)});
        }
        o.flipFront = flipFront.isChecked();
        o.sideFacesRight = sideRight.isChecked();
        o.widthScale = (60 + widthBar.getProgress()) / 100f;
        final RwModel m = model;

        busy = true;
        makeButton.setEnabled(false);
        progress.setVisibility(View.VISIBLE);
        resultStatus.setTextColor(0xFF9BA1AB);
        resultStatus.setText(cj ? "Переношу картинку на Си-Джея и собираю файлы для игры…" : "Переношу картинку на модель…");
        new Thread(new Runnable() {
            public void run() {
                LinkedHashMap<String, Projector.Tex> tex = null;
                Bitmap preview = null, texBmp = null;
                String error = null, savedTo = null;
                try {
                    tex = Projector.project(m, front, side, back, o, sizes);
                    Projector.Frame f = Projector.analyze(m, o.flipFront);
                    int pw = 300, ph = 600;
                    float[] yaws = {0, 45, 180, 225};
                    preview = Bitmap.createBitmap(pw * yaws.length, ph, Bitmap.Config.ARGB_8888);
                    for (int i = 0; i < yaws.length; i++)
                        preview.setPixels(Projector.render(m, tex, f, yaws[i], pw, ph), 0, pw, i * pw, 0, pw, ph);
                    Projector.Tex first = tex.values().iterator().next();
                    texBmp = Bitmap.createBitmap(first.px, first.w, first.h, Bitmap.Config.ARGB_8888);
                    if (cj) savedTo = saveCj(tex);
                } catch (Throwable e) {
                    error = message(e);
                }
                final LinkedHashMap<String, Projector.Tex> fTex = tex;
                final Bitmap fPreview = preview, fTexBmp = texBmp;
                final String fError = error, fSavedTo = savedTo;
                ui.post(new Runnable() {
                    public void run() {
                        busy = false;
                        makeButton.setEnabled(true);
                        progress.setVisibility(View.GONE);
                        if (fError != null) {
                            toastNote(resultStatus, "Не получилось: " + fError);
                            return;
                        }
                        result = fTex;
                        previewImage.setImageBitmap(fPreview);
                        textureImage.setImageBitmap(fTexBmp);
                        textureCaption.setVisibility(View.VISIBLE);
                        saveButton.setVisibility(cj ? View.GONE : View.VISIBLE);
                        saveStatus.setText("");
                        resultStatus.setTextColor(0xFF9BD3A5);
                        if (cj) {
                            resultStatus.setText("Готово. В выбранной папке появилась папка «" + fSavedTo
                                    + "», внутри неё — папка player. Скопируйте эту папку player в игру: "
                                    + "Android/data/com.rockstargames.gtasa/files/texdb/ с заменой файлов "
                                    + "(старую папку player сначала сохраните на случай отката). "
                                    + "Ниже — как Си-Джей будет выглядеть спереди и сзади.");
                        } else {
                            resultStatus.setText("Готово. Ниже модель спереди и сзади. Если что-то съехало — "
                                    + "поправьте рамки или настройки и нажмите «Создать скин» ещё раз.");
                        }
                    }
                });
            }
        }).start();
    }

    private Projector.View view(int which, int w, int h) {
        float[] b = editor.getBox(which);
        return new Projector.View(picturePx, w, h, b[0], b[1], b[2], b[3]);
    }

    // ------------------------------------------------------------------ save

    private String skinName() {
        String n = nameEdit.getText().toString().trim().replaceAll("[^A-Za-z0-9_\\-]", "");
        return n.isEmpty() ? "myskin" : n;
    }

    private void save(Uri tree) {
        if (result == null || partBytes.isEmpty()) return;
        try {
            String name = skinName();
            Uri parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
            Uri dir = DocumentsContract.createDocument(getContentResolver(), parent,
                    DocumentsContract.Document.MIME_TYPE_DIR, name + "_skin");
            if (dir == null) throw new Exception("не удалось создать папку");
            boolean single = partNames.size() == 1;
            for (int i = 0; i < partNames.size(); i++)
                write(dir, "application/octet-stream", single ? name + ".dff" : partNames.get(i), partBytes.get(i));
            write(dir, "application/octet-stream", name + ".txd", TxdWriter.write(result));
            for (Map.Entry<String, Projector.Tex> e : result.entrySet()) {
                Projector.Tex t = e.getValue();
                Bitmap b = Bitmap.createBitmap(t.px, t.w, t.h, Bitmap.Config.ARGB_8888);
                ByteArrayOutputStream png = new ByteArrayOutputStream();
                b.compress(Bitmap.CompressFormat.PNG, 100, png);
                b.recycle();
                write(dir, "image/png", e.getKey() + ".png", png.toByteArray());
            }
            write(dir, "text/plain", "README.txt", readme(name).getBytes("UTF-8"));
            saveStatus.setTextColor(0xFF9BD3A5);
            saveStatus.setText("Сохранено в папку «" + name + "_skin»: модели .dff, " + name
                    + ".txd, текстуры PNG и README.txt с инструкцией по установке.");
        } catch (Throwable e) {
            toastNote(saveStatus, "Не удалось сохранить: " + message(e));
        }
    }

    private void write(Uri dir, String mime, String name, byte[] data) throws Exception {
        Uri doc = DocumentsContract.createDocument(getContentResolver(), dir, mime, name);
        if (doc == null) throw new Exception("не удалось создать файл " + name);
        OutputStream out = getContentResolver().openOutputStream(doc);
        try {
            out.write(data);
        } finally {
            out.close();
        }
    }

    private String readme(String name) {
        StringBuilder t = new StringBuilder();
        t.append("Скин «").append(name).append("» для GTA San Andreas\n\n");
        t.append("Файлы:\n");
        if (partNames.size() == 1) t.append("  ").append(name).append(".dff — модель (копия базового скина)\n");
        else for (String p : partNames) t.append("  ").append(p).append(" — часть тела (копия из архива)\n");
        t.append("  ").append(name).append(".txd — текстуры в формате ПК-версии\n");
        for (String k : result.keySet()) t.append("  ").append(k).append(".png — текстура «").append(k).append("»\n");
        t.append("\nУстановка в Android-версию:\n");
        t.append("1. Сделайте резервную копию папки игры.\n");
        t.append("2. Текстуры: Android-версия берёт их из кэша texdb, а не из .txd. Через TXD Tool откройте\n");
        t.append("   кэш (texdb/player/player.txt для Си-Джея, texdb/gta3/gta3.txt для педов) и замените\n");
        t.append("   текстуры PNG-файлами из этой папки под теми же именами.\n");
        t.append("3. Модели менять не нужно, если вы брали их из своей же игры: они те же самые.\n");
        t.append("   Если базовый скин был сторонним, добавьте .dff в нужный .img через IMG Tool.\n");
        t.append("4. Отдельный пед: чтобы играть им вместо Си-Джея, выберите его CLEO-скриптом\n");
        t.append("   смены скина (Skin Selector).\n");
        t.append("\nФайл .txd пригодится для ПК-версии и для загрузчиков модов, которые читают .txd.\n");
        return t.toString();
    }
}
