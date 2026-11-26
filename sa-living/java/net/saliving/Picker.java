package net.saliving;

import android.app.Activity;
import android.app.Fragment;
import android.content.Intent;
import android.os.Bundle;

// Невидимый фрагмент: открывает системное окно выбора файла и отдаёт результат в Bridge.
public class Picker extends Fragment {
    static final int REQ = 7311;

    @Override
    public void onCreate(Bundle b) {
        super.onCreate(b);
        if (b != null) return;
        try {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            startActivityForResult(i, REQ);
        } catch (Throwable t) {
            Bridge.pickResult = "окно выбора не открылось: " + t;
            Bridge.pickState = -2;
            done();
        }
    }

    @Override
    public void onActivityResult(int req, int res, Intent data) {
        if (req != REQ) return;
        if (res != Activity.RESULT_OK || data == null || data.getData() == null) {
            Bridge.pickState = -1;
            done();
            return;
        }
        new Thread(new Copier(getActivity(), data.getData())).start();
        done();
    }

    static final class Copier implements Runnable {
        final Activity act; final android.net.Uri uri;
        Copier(Activity a, android.net.Uri u) { act = a; uri = u; }
        public void run() { Bridge.pickCopy(act, uri); }
    }

    void done() {
        try { getFragmentManager().beginTransaction().remove(this).commitAllowingStateLoss(); } catch (Throwable t) { }
    }
}
