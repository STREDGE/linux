package net.saliving;

// Мост между модом (C++) и Android: HTTPS с потоковым ответом, распознавание речи, голос педов.
// Собирается в classes.dex (tools/build_dex.sh) и вшивается в libSALiving.so; загружается
// через InMemoryDexClassLoader. Колбэки уходят в C++ через native-методы (RegisterNatives).

import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.speech.RecognitionListener;
import android.speech.RecognizerIntent;
import android.speech.SpeechRecognizer;
import android.speech.tts.TextToSpeech;
import android.speech.tts.UtteranceProgressListener;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.Locale;

public final class Bridge
{
    // ---------- колбэки в C++ ----------
    static native void nativeHttpLine(int id, String line);
    static native void nativeHttpDone(int id, int status, String error);
    static native void nativeStt(int kind, String text);      // 0 частично, 1 итог, 2 ошибка (text = код), 3 готов слушать, 4 конец речи
    static native void nativeTts(String utteranceId, int state); // 0 начал, 1 закончил, 2 ошибка
    static native void nativeTtsReady(int ok);

    static Context sCtx;
    static Handler sMain;
    static SpeechRecognizer sRec;
    static TextToSpeech sTts;
    static volatile boolean sTtsOk;

    public static void init(Context ctx)
    {
        sCtx = ctx.getApplicationContext() != null ? ctx.getApplicationContext() : ctx;
        sMain = new Handler(Looper.getMainLooper());
    }

    // ================= HTTPS (потоковый ответ построчно) =================
    public static void httpPost(final int id, final String url, final String[] headers, final String body, final int timeoutMs)
    {
        new Thread(new Runnable() { public void run() { doPost(id, url, headers, body, timeoutMs); } }, "SALiving-http").start();
    }

    static void doPost(int id, String url, String[] headers, String body, int timeoutMs)
    {
        HttpURLConnection c = null;
        int status = -1;
        try
        {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setRequestMethod("POST");
            c.setDoOutput(true);
            c.setConnectTimeout(15000);
            c.setReadTimeout(timeoutMs > 0 ? timeoutMs : 60000);
            for(int i = 0; i + 1 < headers.length; i += 2) c.setRequestProperty(headers[i], headers[i + 1]);
            byte[] data = body.getBytes("UTF-8");
            c.setFixedLengthStreamingMode(data.length);
            OutputStream os = c.getOutputStream();
            os.write(data);
            os.close();
            status = c.getResponseCode();
            InputStream is = status >= 400 ? c.getErrorStream() : c.getInputStream();
            if(is != null)
            {
                BufferedReader r = new BufferedReader(new InputStreamReader(is, "UTF-8"));
                String line;
                StringBuilder err = status >= 400 ? new StringBuilder() : null;
                while((line = r.readLine()) != null)
                {
                    if(err != null) { if(err.length() < 4000) err.append(line).append('\n'); }
                    else nativeHttpLine(id, line);
                }
                r.close();
                nativeHttpDone(id, status, err != null ? err.toString() : "");
            }
            else nativeHttpDone(id, status, "");
        }
        catch(Throwable t)
        {
            nativeHttpDone(id, status, t.getClass().getSimpleName() + ": " + t.getMessage());
        }
        finally { if(c != null) c.disconnect(); }
    }

    // ================= Распознавание речи =================
    public static boolean sttAvailable()
    {
        try { return SpeechRecognizer.isRecognitionAvailable(sCtx); } catch(Throwable t) { return false; }
    }

    public static void sttStart(final String lang)
    {
        sMain.post(new Runnable() { public void run() {
            try
            {
                if(sRec == null)
                {
                    sRec = SpeechRecognizer.createSpeechRecognizer(sCtx);
                    sRec.setRecognitionListener(new RecListener());
                }
                Intent it = new Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH);
                it.putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM);
                it.putExtra(RecognizerIntent.EXTRA_LANGUAGE, lang);
                it.putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true);
                it.putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 1);
                it.putExtra("android.speech.extra.SPEECH_INPUT_COMPLETE_SILENCE_LENGTH_MILLIS", 1500);
                sRec.startListening(it);
            }
            catch(Throwable t) { nativeStt(2, "start: " + t.getMessage()); }
        }});
    }

    static String first(Bundle b)
    {
        ArrayList<String> l = b == null ? null : b.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION);
        return (l != null && !l.isEmpty()) ? l.get(0) : "";
    }

    public static void sttStop()
    {
        sMain.post(new Runnable() { public void run() { try { if(sRec != null) sRec.stopListening(); } catch(Throwable t) {} } });
    }

    public static void sttCancel()
    {
        sMain.post(new Runnable() { public void run() { try { if(sRec != null) sRec.cancel(); } catch(Throwable t) {} } });
    }

    // ================= Голос (TextToSpeech) =================
    public static void ttsInit(final String lang)
    {
        sMain.post(new Runnable() { public void run() {
            try
            {
                if(sTts != null) return;
                sTts = new TextToSpeech(sCtx, new TtsInit(lang));
            }
            catch(Throwable t) { nativeTtsReady(0); }
        }});
    }

    static final class RecListener implements RecognitionListener
    {
        public void onReadyForSpeech(Bundle b) { nativeStt(3, ""); }
        public void onBeginningOfSpeech() {}
        public void onRmsChanged(float v) {}
        public void onBufferReceived(byte[] b) {}
        public void onEndOfSpeech() { nativeStt(4, ""); }
        public void onError(int e) { nativeStt(2, Integer.toString(e)); }
        public void onResults(Bundle b) { nativeStt(1, first(b)); }
        public void onPartialResults(Bundle b) { nativeStt(0, first(b)); }
        public void onEvent(int t, Bundle b) {}
    }

    static final class TtsProgress extends UtteranceProgressListener
    {
        public void onStart(String id) { nativeTts(id, 0); }
        public void onDone(String id) { nativeTts(id, 1); }
        public void onError(String id) { nativeTts(id, 2); }
    }

    static final class TtsInit implements TextToSpeech.OnInitListener
    {
        final String lang;
        TtsInit(String l) { lang = l; }
        public void onInit(int status)
        {
            sTtsOk = status == TextToSpeech.SUCCESS;
            if(sTtsOk)
            {
                try { sTts.setLanguage(Locale.forLanguageTag(lang)); } catch(Throwable t) {}
                sTts.setOnUtteranceProgressListener(new TtsProgress());
            }
            nativeTtsReady(sTtsOk ? 1 : 0);
        }
    }

    public static void ttsSpeak(final String text, final float pitch, final float rate, final String id, final boolean queue)
    {
        sMain.post(new Runnable() { public void run() {
            try
            {
                if(sTts == null || !sTtsOk) { nativeTts(id, 2); return; }
                sTts.setPitch(pitch);
                sTts.setSpeechRate(rate);
                Bundle p = new Bundle();
                sTts.speak(text, queue ? TextToSpeech.QUEUE_ADD : TextToSpeech.QUEUE_FLUSH, p, id);
            }
            catch(Throwable t) { nativeTts(id, 2); }
        }});
    }

    public static void ttsStop()
    {
        sMain.post(new Runnable() { public void run() { try { if(sTts != null) sTts.stop(); } catch(Throwable t) {} } });
    }

    // ================= Голосовой пакет: скачивание файлов =================
    static native void nativeDl(int state, long done, long total, String msg);   // 0 идёт, 1 готово, 2 ошибка
    static volatile boolean sDlCancel;

    // plan: строки "url\tпуть" и "tree\tрепозиторий\tпрефикс\tпапка" (список файлов HF по префиксу),
    // "unzip\tzip\tимя_внутри\tпуть". Уже скачанные файлы (с отметкой .ok) пропускаются.
    public static void downloadPack(final String plan)
    {
        sDlCancel = false;
        new Thread(new Runnable() { public void run() { runPlan(plan); } }, "SALiving-dl").start();
    }

    public static void cancelDownload() { sDlCancel = true; }

    static void runPlan(String plan)
    {
        try
        {
            java.util.ArrayList<String[]> jobs = new java.util.ArrayList<String[]>();
            java.util.ArrayList<String[]> unz = new java.util.ArrayList<String[]>();
            java.util.ArrayList<String[]> unzAll = new java.util.ArrayList<String[]>();
            for(String line : plan.split("\n"))
            {
                String[] p = line.split("\t");
                if(p.length >= 2 && p[0].startsWith("http")) jobs.add(new String[] { p[0], p[1] });
                else if(p.length >= 4 && p[0].equals("tree"))
                {
                    String json = getText("https://huggingface.co/api/models/" + p[1] + "/tree/main/" + p[2] + "?recursive=true");
                    org.json.JSONArray a = new org.json.JSONArray(json);
                    for(int i = 0; i < a.length(); ++i)
                    {
                        org.json.JSONObject o = a.getJSONObject(i);
                        if(!"file".equals(o.optString("type"))) continue;
                        String path = o.getString("path");
                        jobs.add(new String[] { "https://huggingface.co/" + p[1] + "/resolve/main/" + path, p[3] + "/" + path });
                    }
                }
                else if(p.length >= 4 && p[0].equals("unzip")) unz.add(new String[] { p[1], p[2], p[3] });
                else if(p.length >= 3 && p[0].equals("unzipall")) unzAll.add(new String[] { p[1], p[2] });
            }
            long total = 0, done = 0;
            for(String[] j : jobs) { java.io.File f = new java.io.File(j[1]); if(!new java.io.File(j[1] + ".ok").exists()) total += 1; }
            int n = 0;
            for(String[] j : jobs)
            {
                if(sDlCancel) { nativeDl(2, n, total, "отменено"); return; }
                java.io.File f = new java.io.File(j[1]);
                if(new java.io.File(j[1] + ".ok").exists()) continue;
                f.getParentFile().mkdirs();
                nativeDl(0, 0, 0, f.getName());
                fetch(j[0], f);
                new java.io.File(j[1] + ".ok").createNewFile();
                n++;
            }
            for(String[] u : unz)
            {
                if(new java.io.File(u[2] + ".ok").exists()) continue;
                nativeDl(0, n, total, "распаковка " + new java.io.File(u[2]).getName());
                unzipOne(u[0], u[1], u[2]);
                new java.io.File(u[2] + ".ok").createNewFile();
            }
            for(String[] u : unzAll) unzipAll(u[0], u[1]);
            nativeDl(1, total, total, "готово");
        }
        catch(Throwable t) { nativeDl(2, 0, 0, t.getClass().getSimpleName() + ": " + t.getMessage()); }
    }

    // Весь архив в папку (голосовой пакет). Прогресс — в байтах архива.
    static void unzipAll(String zip, String dir) throws Exception
    {
        java.io.File zf = new java.io.File(zip);
        long total = zf.length(), done = 0;
        java.io.FileInputStream fis = new java.io.FileInputStream(zf);
        java.util.zip.ZipInputStream z = new java.util.zip.ZipInputStream(new java.io.BufferedInputStream(fis, 1 << 16));
        java.util.zip.ZipEntry e;
        byte[] b = new byte[1 << 16];
        long lastReport = 0;
        while((e = z.getNextEntry()) != null)
        {
            String name = e.getName();
            if(e.isDirectory() || name.contains("..")) continue;
            java.io.File out = new java.io.File(dir, name);
            out.getParentFile().mkdirs();
            java.io.File tmp = new java.io.File(out.getPath() + ".part");
            java.io.FileOutputStream os = new java.io.FileOutputStream(tmp);
            int r;
            while((r = z.read(b)) > 0)
            {
                if(sDlCancel) { os.close(); z.close(); throw new java.io.IOException("отменено"); }
                os.write(b, 0, r);
                done = fis.getChannel().position();
                if(done - lastReport > (1 << 20)) { lastReport = done; nativeDl(0, done >> 10, total >> 10, "распаковка " + out.getName()); }
            }
            os.close();
            if(out.exists()) out.delete();
            if(!tmp.renameTo(out)) { z.close(); throw new java.io.IOException("не переименовать " + out); }
        }
        z.close();
    }

    // ================= Выбор файла на телефоне (системное окно «Файлы») =================
    // 0 — ничего, 1 — выбирают/копируют, 2 — готово (pickResult — путь к копии), -1 — отмена, -2 — ошибка (pickResult — текст)
    public static volatile int pickState = 0;
    public static volatile String pickResult = "";
    static String sPickDest = "";

    public static void pickFile(final android.app.Activity act, String destPath)
    {
        sPickDest = destPath; pickState = 1; pickResult = "";
        act.runOnUiThread(new PickOpener(act));
    }

    static final class PickOpener implements Runnable
    {
        final android.app.Activity act;
        PickOpener(android.app.Activity a) { act = a; }
        public void run()
        {
            try
            {
                android.app.FragmentManager fm = act.getFragmentManager();
                android.app.Fragment old = fm.findFragmentByTag("saliving_picker");
                if(old != null) fm.beginTransaction().remove(old).commitAllowingStateLoss();
                fm.beginTransaction().add(new Picker(), "saliving_picker").commitAllowingStateLoss();
            }
            catch(Throwable t) { pickResult = "окно выбора не открылось: " + t; pickState = -2; }
        }
    }

    static void pickCopy(android.app.Activity act, android.net.Uri uri)
    {
        try
        {
            java.io.File out = new java.io.File(sPickDest);
            out.getParentFile().mkdirs();
            InputStream in = act.getContentResolver().openInputStream(uri);
            java.io.FileOutputStream os = new java.io.FileOutputStream(out);
            byte[] buf = new byte[1 << 16]; int n;
            while((n = in.read(buf)) > 0) os.write(buf, 0, n);
            os.close(); in.close();
            pickResult = out.getAbsolutePath(); pickState = 2;
        }
        catch(Throwable t) { pickResult = "не удалось скопировать: " + t; pickState = -2; }
    }

    // Открыть ссылку в браузере
    public static void openUrl(final String url)
    {
        try
        {
            android.content.Intent i = new android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(url));
            i.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
            sCtx.startActivity(i);
        }
        catch(Throwable t) {}
    }

    static String getText(String url) throws Exception
    {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(15000); c.setReadTimeout(30000);
        InputStream is = c.getInputStream();
        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
        byte[] b = new byte[16384]; int r;
        while((r = is.read(b)) > 0) bo.write(b, 0, r);
        is.close(); c.disconnect();
        return bo.toString("UTF-8");
    }

    static void fetch(String url, java.io.File out) throws Exception
    {
        java.io.File tmp = new java.io.File(out.getPath() + ".part");
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(15000); c.setReadTimeout(60000); c.setInstanceFollowRedirects(true);
        int code = c.getResponseCode();
        if(code >= 400) throw new java.io.IOException("HTTP " + code + " " + url);
        InputStream is = c.getInputStream();
        java.io.FileOutputStream os = new java.io.FileOutputStream(tmp);
        byte[] b = new byte[65536]; int r;
        long len = 0; try { len = c.getContentLengthLong(); } catch(Throwable t) {}
        long got = 0, last = 0;
        while((r = is.read(b)) > 0)
        {
            if(sDlCancel) { os.close(); is.close(); throw new java.io.IOException("отменено"); }
            os.write(b, 0, r);
            got += r;
            if(got - last > (1 << 20)) { last = got; nativeDl(0, got >> 10, len >> 10, out.getName()); }
        }
        os.close(); is.close(); c.disconnect();
        if(out.exists()) out.delete();
        if(!tmp.renameTo(out)) throw new java.io.IOException("не переименовать " + out);
    }

    static void unzipOne(String zip, String entry, String outPath) throws Exception
    {
        java.util.zip.ZipInputStream z = new java.util.zip.ZipInputStream(new java.io.FileInputStream(zip));
        java.util.zip.ZipEntry e;
        while((e = z.getNextEntry()) != null)
        {
            if(!e.getName().equals(entry)) continue;
            java.io.File out = new java.io.File(outPath);
            out.getParentFile().mkdirs();
            java.io.FileOutputStream os = new java.io.FileOutputStream(out);
            byte[] b = new byte[65536]; int r;
            while((r = z.read(b)) > 0) os.write(b, 0, r);
            os.close(); z.close();
            return;
        }
        z.close();
        throw new java.io.IOException("в архиве нет " + entry);
    }

    // ================= Микрофон (16 кГц, моно) =================
    static native void nativeMic(short[] samples, int count);   // запись закончена
    static volatile boolean sRecording;

    public static boolean micStart()
    {
        if(sRecording) return true;
        try
        {
            final int sr = 16000;
            int min = android.media.AudioRecord.getMinBufferSize(sr, android.media.AudioFormat.CHANNEL_IN_MONO, android.media.AudioFormat.ENCODING_PCM_16BIT);
            final android.media.AudioRecord rec = new android.media.AudioRecord(android.media.MediaRecorder.AudioSource.VOICE_RECOGNITION,
                sr, android.media.AudioFormat.CHANNEL_IN_MONO, android.media.AudioFormat.ENCODING_PCM_16BIT, Math.max(min, sr));
            if(rec.getState() != android.media.AudioRecord.STATE_INITIALIZED) { rec.release(); return false; }
            sRecording = true;
            rec.startRecording();
            new Thread(new Runnable() { public void run() {
                short[] all = new short[sr * 30]; int n = 0;   // не больше 30 секунд
                short[] buf = new short[1600];
                while(sRecording && n < all.length)
                {
                    int r = rec.read(buf, 0, buf.length);
                    if(r <= 0) break;
                    int k = Math.min(r, all.length - n);
                    System.arraycopy(buf, 0, all, n, k); n += k;
                }
                try { rec.stop(); } catch(Throwable t) {}
                rec.release();
                sRecording = false;
                nativeMic(all, n);
            }}, "SALiving-mic").start();
            return true;
        }
        catch(Throwable t) { sRecording = false; return false; }
    }

    public static void micStop() { sRecording = false; }

    // ================= Воспроизведение синтезированной речи =================
    static final java.util.concurrent.LinkedBlockingQueue<Object[]> sPlayQ = new java.util.concurrent.LinkedBlockingQueue<Object[]>();
    static Thread sPlayer;
    static volatile int sPlayGen;

    public static void playPcm(float[] samples, int sampleRate, float pitch, float speed, String id)
    {
        sPlayQ.add(new Object[] { samples, sampleRate, pitch, speed, id, sPlayGen });
        if(sPlayer == null)
        {
            sPlayer = new Thread(new Runnable() { public void run() { playerLoop(); } }, "SALiving-voice");
            sPlayer.start();
        }
    }

    public static void playStop() { sPlayGen++; sPlayQ.clear(); }

    static void playerLoop()
    {
        while(true)
        {
            Object[] j;
            try { j = sPlayQ.take(); } catch(InterruptedException e) { return; }
            if((Integer) j[5] != sPlayGen) continue;
            float[] s = (float[]) j[0]; int sr = (Integer) j[1]; float pitch = (Float) j[2], speed = (Float) j[3]; String id = (String) j[4];
            try
            {
                int bytes = s.length * 4;
                android.media.AudioTrack t = new android.media.AudioTrack.Builder()
                    .setAudioAttributes(new android.media.AudioAttributes.Builder()
                        .setUsage(android.media.AudioAttributes.USAGE_GAME)
                        .setContentType(android.media.AudioAttributes.CONTENT_TYPE_SPEECH).build())
                    .setAudioFormat(new android.media.AudioFormat.Builder()
                        .setEncoding(android.media.AudioFormat.ENCODING_PCM_FLOAT).setSampleRate(sr)
                        .setChannelMask(android.media.AudioFormat.CHANNEL_OUT_MONO).build())
                    .setTransferMode(android.media.AudioTrack.MODE_STATIC).setBufferSizeInBytes(bytes).build();
                t.write(s, 0, s.length, android.media.AudioTrack.WRITE_BLOCKING);
                try { t.setPlaybackParams(new android.media.PlaybackParams().setPitch(pitch).setSpeed(speed)); } catch(Throwable e) {}
                nativeTts(id, 0);
                t.play();
                long ms = (long) (s.length * 1000.0 / sr / Math.max(0.5f, speed)) + 60;
                long end = System.currentTimeMillis() + ms;
                while(System.currentTimeMillis() < end && (Integer) j[5] == sPlayGen) Thread.sleep(20);
                t.stop(); t.release();
                nativeTts(id, 1);
            }
            catch(Throwable e) { nativeTts(id, 2); }
        }
    }
}
