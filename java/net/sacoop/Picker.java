package net.sacoop;

import android.app.Activity;
import android.app.Fragment;
import android.app.FragmentManager;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

// Выбор файла сохранения на телефоне (системное окно «Файлы», как при загрузке в браузере).
// Невидимый фрагмент открывает окно выбора; выбранный файл копируется в папку кооп-сохранений.
// Состояние читает мод: state 0 — ничего, 1 — выбирают, 2 — готово (result — путь), -1 — отмена, -2 — ошибка (result — текст).
public class Picker extends Fragment {
    public static volatile int state = 0;
    public static volatile String result = "";
    static String destDir = "";
    static final int REQ = 7301;
    static final String TAG = "sacoop_picker";

    public static void pick(final Activity act, String dir) {
        destDir = dir;
        state = 1;
        result = "";
        act.runOnUiThread(new Opener(act));
    }

    static final class Opener implements Runnable {
        final Activity act;
        Opener(Activity a) { act = a; }
        public void run() {
            try {
                FragmentManager fm = act.getFragmentManager();
                Fragment old = fm.findFragmentByTag(TAG);
                if (old != null) fm.beginTransaction().remove(old).commitAllowingStateLoss();
                fm.beginTransaction().add(new Picker(), TAG).commitAllowingStateLoss();
            } catch (Throwable t) {
                result = "окно выбора не открылось: " + t;
                state = -2;
            }
        }
    }

    static final class Copier implements Runnable {
        final Activity act; final Uri uri;
        Copier(Activity a, Uri u) { act = a; uri = u; }
        public void run() { copy(act, uri); }
    }

    @Override
    public void onCreate(Bundle b) {
        super.onCreate(b);
        if (b != null) return; // восстановление после поворота — не открывать заново
        try {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            startActivityForResult(i, REQ);
        } catch (Throwable t) {
            result = "окно выбора не открылось: " + t;
            state = -2;
            done();
        }
    }

    @Override
    public void onActivityResult(int req, int res, Intent data) {
        if (req != REQ) return;
        if (res != Activity.RESULT_OK || data == null || data.getData() == null) {
            state = -1;
            done();
            return;
        }
        new Thread(new Copier(getActivity(), data.getData())).start();
        done();
    }

    static void copy(Activity act, Uri uri) {
        try {
            String name = "Из телефона.b";
            Cursor c = act.getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null);
            if (c != null) {
                if (c.moveToFirst()) {
                    String n = c.getString(0);
                    if (n != null && n.length() > 0) name = n;
                }
                c.close();
            }
            name = name.replace('/', '_').replace('\\', '_');
            if (!name.toLowerCase().endsWith(".b")) name = name + ".b";
            File dir = new File(destDir);
            dir.mkdirs();
            File out = new File(dir, name);
            InputStream in = act.getContentResolver().openInputStream(uri);
            OutputStream os = new FileOutputStream(out);
            byte[] buf = new byte[65536];
            long total = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                os.write(buf, 0, n);
                total += n;
                if (total > 4L * 1024 * 1024) break;
            }
            os.close();
            in.close();
            result = out.getAbsolutePath();
            state = 2;
        } catch (Throwable t) {
            result = "не удалось скопировать: " + t;
            state = -2;
        }
    }

    void done() {
        try { getFragmentManager().beginTransaction().remove(this).commitAllowingStateLoss(); } catch (Throwable t) { }
    }
}
