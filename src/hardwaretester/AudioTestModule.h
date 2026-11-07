// =============================================================================
// AudioTestModule.h - speaker (left/right/both tone), microphone level and loop-back test.
// =============================================================================
#ifndef AUDIOTESTMODULE_H
#define AUDIOTESTMODULE_H

#include "TesterModule.h"

#include <QAudioFormat>
#include <QBuffer>
#include <QIODevice>
#include <QMediaDevices>

#include <memory>

class QAudioSink;
class QAudioSource;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QTimer;
class Card;

// Pull-mode sine source for QAudioSink: float32 stereo with per-channel gates
// (left / right / both) and a short amplitude ramp on start/stop/frequency
// changes to avoid clicks.
class SineGenerator : public QIODevice {
    Q_OBJECT
public:
    explicit SineGenerator(int sampleRate, QObject *parent = nullptr);

    void setFrequency(double hz) { m_freq = hz; }
    void setChannels(bool left, bool right);
    void rampTo(double gain) { m_targetGain = gain; } // 0..1

    // Plays a looping melody instead of a fixed tone: each entry is (frequency
    // Hz, duration ms); frequency 0 is a rest. Pass an empty list for a plain
    // fixed-frequency tone.
    void setMelody(const QList<QPair<double, int>> &notesMs);

    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *, qint64) override { return -1; }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;

private:
    int m_rate;
    double m_freq = 440.0;
    double m_phase = 0.0;
    double m_gain = 0.0;        // current envelope value
    double m_targetGain = 0.0;  // ramp target
    bool m_left = true, m_right = true;

    struct Note { double freq; qint64 frames; };
    QList<Note> m_melody;       // empty → fixed-frequency tone
    int m_noteIdx = 0;
    qint64 m_noteFrame = 0;     // frames elapsed within the current note
};

// Scrolling microphone waveform (normalized floats, ring buffer).
class WaveformWidget : public QWidget {
    Q_OBJECT
public:
    explicit WaveformWidget(QWidget *parent = nullptr);
    void addSamples(const QList<float> &samples);
    void clear();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QList<float> m_samples; // most recent last
};

class AudioTestModule : public TesterModule {
    Q_OBJECT
public:
    explicit AudioTestModule(Translator t, QWidget *parent = nullptr);

    void retranslate() override;
    void moduleEntered() override;
    void moduleLeft() override;

private:
    // --- speaker ---
    // Starts a test tone/melody on the left, right or both speakers.
    void startTone(bool left, bool right);
    // Fades out and stops the tone.
    void stopTone();

    // --- microphone ---
    // Opens the selected microphone for the level meter and waveform.
    void startMic();
    // Closes the microphone.
    void stopMic();
    // Timer slot: reads samples, updates level bar and waveform.
    void readMicData();
    // Records a few seconds from the microphone into memory.
    void startRecording();
    // Plays the recording back on the selected output.
    void playRecording();
    // Stops playback.
    void stopPlayback();

    // UI — speaker
    QComboBox *m_outputCombo;
    QPushButton *m_leftBtn, *m_rightBtn, *m_bothBtn, *m_stopBtn;
    QLabel *m_melodyLabel;
    Card *m_speakerCard;

    // UI — mic
    QComboBox *m_inputCombo;
    QProgressBar *m_levelBar;
    WaveformWidget *m_waveform;
    QLabel *m_micInfoLabel;
    QPushButton *m_recordBtn, *m_playBtn;
    QLabel *m_recordStatus;
    Card *m_micCard;

    // audio plumbing
    QMediaDevices m_mediaDevices;
    std::unique_ptr<QAudioSink> m_toneSink;
    std::unique_ptr<SineGenerator> m_generator;
    QTimer *m_toneStopTimer;

    std::unique_ptr<QAudioSource> m_micSource;
    QIODevice *m_micIo = nullptr; // owned by m_micSource
    QAudioFormat m_micFormat;
    QTimer *m_micTimer;

    bool m_recording = false;
    QByteArray m_recorded;
    QAudioFormat m_recordedFormat;
    QTimer *m_recordTimer;
    std::unique_ptr<QAudioSink> m_playbackSink;
    QBuffer m_playbackBuffer;
};

#endif // AUDIOTESTMODULE_H
