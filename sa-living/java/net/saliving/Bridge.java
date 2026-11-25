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
}
