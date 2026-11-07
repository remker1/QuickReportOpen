// =============================================================================
// AudioTestModule.cpp - speaker tone generator, microphone meter, record/playback.
// =============================================================================
#include "AudioTestModule.h"
#include "common/Card.h"
#include "common/TesterTheme.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

namespace {
constexpr int kToneSampleRate = 48000;
constexpr double kToneLevel = 0.4;      // headroom, avoids clipping/harshness
constexpr double kRampSeconds = 0.010;  // click-free start/stop

// A simple, universally recognizable public-domain tune ("Twinkle Twinkle
// Little Star" / "Ah! vous dirai-je, maman"). Pleasant for verifying a speaker
// and its L/R channels — far easier on the ear than a sustained sine tone.
// Each entry is (frequency Hz, duration ms); 0 Hz is a short rest.
// The test melody (frequency Hz, duration ms; 0 Hz = rest).
QList<QPair<double, int>> twinkleMelody() {
    const double C = 261.63, D = 293.66, E = 329.63, F = 349.23, G = 392.00, A = 440.00;
    const int q = 380, h = 720, r = 40; // quarter, half, inter-note rest
    const QList<QPair<double, int>> notes = {
        {C,q},{0,r},{C,q},{0,r},{G,q},{0,r},{G,q},{0,r},{A,q},{0,r},{A,q},{0,r},{G,h},{0,r},
        {F,q},{0,r},{F,q},{0,r},{E,q},{0,r},{E,q},{0,r},{D,q},{0,r},{D,q},{0,r},{C,h},{0,r},
        {G,q},{0,r},{G,q},{0,r},{F,q},{0,r},{F,q},{0,r},{E,q},{0,r},{E,q},{0,r},{D,h},{0,r},
        {G,q},{0,r},{G,q},{0,r},{F,q},{0,r},{F,q},{0,r},{E,q},{0,r},{E,q},{0,r},{D,h},{0,h},
    };
    return notes;
}
} // namespace

// ── SineGenerator ──────────────────────────────────────────────

SineGenerator::SineGenerator(int sampleRate, QObject *parent)
    : QIODevice(parent), m_rate(sampleRate) {
    open(QIODevice::ReadOnly);
}

// Selects which channels carry the tone.
void SineGenerator::setChannels(bool left, bool right) {
    m_left = left;
    m_right = right;
}

// (see header)
void SineGenerator::setMelody(const QList<QPair<double, int>> &notesMs) {
    m_melody.clear();
    for (const auto &n : notesMs)
        m_melody.append({n.first, qint64(qMax(1, n.second) * m_rate / 1000)});
    m_noteIdx = 0;
    m_noteFrame = 0;
    m_phase = 0.0;
}

// Pull-mode source: always reports data as available.
qint64 SineGenerator::bytesAvailable() const {
    // Pull source: always claim a healthy amount so the sink keeps reading.
    return 16384 + QIODevice::bytesAvailable();
}

// Generates float32 stereo samples: sine phase accumulation, melody advance,
// and a smoothed gain envelope to avoid clicks.
qint64 SineGenerator::readData(char *data, qint64 maxlen) {
    // Frames are float32 stereo.
    const qint64 frames = maxlen / qint64(2 * sizeof(float));
    auto *out = reinterpret_cast<float *>(data);
    const double rampStep = 1.0 / (kRampSeconds * m_rate);
    // Per-note attack/release so a melody's notes sound distinct and click-free.
    const qint64 atkFrames = qint64(0.006 * m_rate);
    const qint64 relFrames = qint64(0.050 * m_rate);

    for (qint64 i = 0; i < frames; ++i) {
        if (m_gain < m_targetGain)
            m_gain = qMin(m_targetGain, m_gain + rampStep);
        else if (m_gain > m_targetGain)
            m_gain = qMax(m_targetGain, m_gain - rampStep);

        double freq = m_freq;
        double noteEnv = 1.0;
        if (!m_melody.isEmpty()) {
            const Note &note = m_melody[m_noteIdx];
            freq = note.freq;
            if (m_noteFrame < atkFrames)
                noteEnv = double(m_noteFrame) / double(atkFrames);
            else if (m_noteFrame > note.frames - relFrames)
                noteEnv = qMax(0.0, double(note.frames - m_noteFrame) / double(relFrames));
            if (++m_noteFrame >= note.frames) {
                m_noteFrame = 0;
                m_phase = 0.0; // reset per note to avoid pitch-transition clicks
                if (++m_noteIdx >= m_melody.size())
                    m_noteIdx = 0; // loop the tune
            }
        }

        float v = 0.0f;
        if (freq > 0.0) {
            v = float(qSin(m_phase) * kToneLevel * m_gain * noteEnv);
            m_phase += 2.0 * M_PI * freq / m_rate;
            if (m_phase > 2.0 * M_PI)
                m_phase -= 2.0 * M_PI;
        }
        *out++ = m_left ? v : 0.0f;
        *out++ = m_right ? v : 0.0f;
    }
    return frames * 2 * sizeof(float);
}

// ── WaveformWidget ─────────────────────────────────────────────

// Creates the waveform view (minimum height 70 px).
WaveformWidget::WaveformWidget(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(70);
}

// Appends samples, keeping only the newest 4000.
void WaveformWidget::addSamples(const QList<float> &samples) {
    m_samples += samples;
    const int cap = 4000;
    if (m_samples.size() > cap)
        m_samples.remove(0, m_samples.size() - cap);
    update();
}

// Empties the waveform.
void WaveformWidget::clear() {
    m_samples.clear();
    update();
}

// Draws the samples as a waveform over the canvas background.
void WaveformWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const auto &pal = TesterTheme::current();
    p.fillRect(rect(), pal.canvasBg);
    p.setPen(QPen(pal.cardBorder, 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    if (m_samples.size() < 2)
        return;
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(pal.accent, 1));
    const double midY = height() / 2.0;
    const double xStep = double(width()) / m_samples.size();
    QPolygonF poly;
    poly.reserve(m_samples.size());
    for (int i = 0; i < m_samples.size(); ++i)
        poly << QPointF(i * xStep, midY - m_samples[i] * (midY - 4));
    p.drawPolyline(poly);
}

// ── AudioTestModule ────────────────────────────────────────────

AudioTestModule::AudioTestModule(Translator t, QWidget *parent)
    : TesterModule(std::move(t), parent) {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 10, 14, 10);
    outer->setSpacing(10);

    // ── Speaker card ──
    m_speakerCard = new Card(QString(), this);
    m_outputCombo = new QComboBox(this);
    m_speakerCard->body()->addWidget(m_outputCombo);

    auto *chanRow = new QHBoxLayout();
    m_leftBtn = new QPushButton(this);
    m_rightBtn = new QPushButton(this);
    m_bothBtn = new QPushButton(this);
    m_stopBtn = new QPushButton(this);
    for (auto *b : {m_leftBtn, m_rightBtn, m_bothBtn})
        b->setCheckable(true);
    chanRow->addWidget(m_leftBtn);
    chanRow->addWidget(m_bothBtn);
    chanRow->addWidget(m_rightBtn);
    chanRow->addSpacing(16);
    chanRow->addWidget(m_stopBtn);
    chanRow->addStretch();
    m_speakerCard->body()->addLayout(chanRow);

    // The channel buttons above play a short, recognizable melody (instead of a
    // harsh sine tone) so a quick listen confirms the speaker and its channels.
    m_melodyLabel = new QLabel(this);
    m_melodyLabel->setStyleSheet(QString("color:%1;font-size:12px;")
                                     .arg(TesterTheme::current().textSecondary.name()));
    m_speakerCard->body()->addWidget(m_melodyLabel);
    outer->addWidget(m_speakerCard);

    // ── Microphone card ──
    m_micCard = new Card(QString(), this);
    m_inputCombo = new QComboBox(this);
    m_micCard->body()->addWidget(m_inputCombo);

    m_micInfoLabel = new QLabel(this);
    m_micInfoLabel->setStyleSheet(QString("color:%1;font-size:11px;")
                                      .arg(TesterTheme::current().textSecondary.name()));
    m_micCard->body()->addWidget(m_micInfoLabel);

    m_levelBar = new QProgressBar(this);
    m_levelBar->setRange(0, 100);
    m_levelBar->setTextVisible(false);
    m_levelBar->setFixedHeight(12);
    m_micCard->body()->addWidget(m_levelBar);

    m_waveform = new WaveformWidget(this);
    m_micCard->body()->addWidget(m_waveform, 1);

    auto *recRow = new QHBoxLayout();
    m_recordBtn = new QPushButton(this);
    m_playBtn = new QPushButton(this);
    m_playBtn->setEnabled(false);
    m_recordStatus = new QLabel(this);
    recRow->addWidget(m_recordBtn);
    recRow->addWidget(m_playBtn);
    recRow->addWidget(m_recordStatus, 1);
    m_micCard->body()->addLayout(recRow);
    outer->addWidget(m_micCard, 1);

    // ── wiring ──
    connect(m_leftBtn, &QPushButton::clicked, this, [this] { startTone(true, false); });
    connect(m_rightBtn, &QPushButton::clicked, this, [this] { startTone(false, true); });
    connect(m_bothBtn, &QPushButton::clicked, this, [this] { startTone(true, true); });
    connect(m_stopBtn, &QPushButton::clicked, this, [this] { stopTone(); });
    connect(m_outputCombo, &QComboBox::activated, this, [this](int) { stopTone(); });
    connect(m_inputCombo, &QComboBox::activated, this, [this](int) {
        stopMic();
        startMic();
    });
    connect(m_recordBtn, &QPushButton::clicked, this, &AudioTestModule::startRecording);
    connect(m_playBtn, &QPushButton::clicked, this, &AudioTestModule::playRecording);
    connect(&m_mediaDevices, &QMediaDevices::audioOutputsChanged, this, [this] {
        if (isVisible()) moduleEntered();
    });
    connect(&m_mediaDevices, &QMediaDevices::audioInputsChanged, this, [this] {
        if (isVisible()) moduleEntered();
    });

    m_toneStopTimer = new QTimer(this);
    m_toneStopTimer->setSingleShot(true);
    m_toneStopTimer->setInterval(60); // after the 10 ms down-ramp has played out
    connect(m_toneStopTimer, &QTimer::timeout, this, [this] {
        if (m_toneSink) {
            m_toneSink->stop();
            m_toneSink.reset();
            m_generator.reset();
        }
    });

    m_micTimer = new QTimer(this);
    m_micTimer->setInterval(20);
    connect(m_micTimer, &QTimer::timeout, this, &AudioTestModule::readMicData);

    m_recordTimer = new QTimer(this);
    m_recordTimer->setSingleShot(true);
    m_recordTimer->setInterval(5000);
    connect(m_recordTimer, &QTimer::timeout, this, [this] {
        m_recording = false;
        m_playBtn->setEnabled(!m_recorded.isEmpty());
        m_recordBtn->setEnabled(true);
        m_recordStatus->setText(tr_("ht_aud_record_done"));
        recordResult("ht_aud_result_name",
                     QString("mic OK, %1 KB recorded").arg(m_recorded.size() / 1024));
    });

    retranslate();
}

// (see header)
void AudioTestModule::startTone(bool left, bool right) {
    m_leftBtn->setChecked(left && !right);
    m_rightBtn->setChecked(right && !left);
    m_bothBtn->setChecked(left && right);
    m_toneStopTimer->stop();

    if (!m_toneSink) {
        const auto device = m_outputCombo->currentData().value<QAudioDevice>();
        if (device.isNull())
            return;
        QAudioFormat fmt;
        fmt.setSampleRate(kToneSampleRate);
        fmt.setChannelCount(2);
        fmt.setSampleFormat(QAudioFormat::Float);
        m_generator = std::make_unique<SineGenerator>(kToneSampleRate);
        m_generator->setMelody(twinkleMelody());
        m_toneSink = std::make_unique<QAudioSink>(device, fmt);
        m_toneSink->start(m_generator.get());
    }
    m_generator->setChannels(left, right);
    m_generator->rampTo(1.0);
    recordResult("ht_aud_result_name", "speaker melody played");
}

// (see header)
void AudioTestModule::stopTone() {
    m_leftBtn->setChecked(false);
    m_rightBtn->setChecked(false);
    m_bothBtn->setChecked(false);
    if (m_generator) {
        m_generator->rampTo(0.0); // fade out, then tear down
        m_toneStopTimer->start();
    }
}

// Populates the device lists and starts the microphone meter.
void AudioTestModule::moduleEntered() {
    // Repopulate both device combos with live system lists.
    m_outputCombo->clear();
    for (const QAudioDevice &d : QMediaDevices::audioOutputs())
        m_outputCombo->addItem(d.description(), QVariant::fromValue(d));
    m_inputCombo->clear();
    for (const QAudioDevice &d : QMediaDevices::audioInputs())
        m_inputCombo->addItem(d.description(), QVariant::fromValue(d));

    if (m_outputCombo->count() == 0)
        m_outputCombo->addItem(tr_("ht_common_no_device"));
    if (m_inputCombo->count() == 0) {
        m_inputCombo->addItem(tr_("ht_common_no_device"));
        m_micInfoLabel->setText(tr_("ht_common_no_device"));
        return;
    }
    startMic();
}

// Stops tone, microphone, recording and playback.
void AudioTestModule::moduleLeft() {
    // Immediate teardown; the page is gone so a click is acceptable here.
    m_toneStopTimer->stop();
    if (m_toneSink) {
        m_toneSink->stop();
        m_toneSink.reset();
        m_generator.reset();
    }
    stopMic();
    stopPlayback();
    m_recordTimer->stop();
    m_recording = false;
}

// (see header)
void AudioTestModule::startMic() {
    stopMic();
    const auto device = m_inputCombo->currentData().value<QAudioDevice>();
    if (device.isNull())
        return;

    m_micFormat = device.preferredFormat();
    // Normalize to formats we can decode for the meter; WASAPI resamples.
    if (m_micFormat.sampleFormat() != QAudioFormat::Int16 &&
        m_micFormat.sampleFormat() != QAudioFormat::Float)
        m_micFormat.setSampleFormat(QAudioFormat::Int16);

    m_micSource = std::make_unique<QAudioSource>(device, m_micFormat);
    m_micIo = m_micSource->start();
    if (!m_micIo) {
        m_micInfoLabel->setText(tr_("ht_common_no_permission"));
        m_micSource.reset();
        return;
    }
    m_micInfoLabel->setText(QString("%1 — %2 Hz, %3 ch")
                                .arg(device.description())
                                .arg(m_micFormat.sampleRate())
                                .arg(m_micFormat.channelCount()));
    m_micTimer->start();
}

// (see header)
void AudioTestModule::stopMic() {
    m_micTimer->stop();
    if (m_micSource) {
        m_micSource->stop();
        m_micSource.reset();
        m_micIo = nullptr;
    }
    m_levelBar->setValue(0);
}

// (see header) Level = peak of the chunk; samples are decimated for the waveform;
// also collects data while recording.
void AudioTestModule::readMicData() {
    if (!m_micIo)
        return;
    const QByteArray chunk = m_micIo->readAll();
    if (chunk.isEmpty())
        return;

    if (m_recording) {
        m_recorded += chunk;
        m_recordStatus->setText(tr_("ht_aud_recording")
                                    .replace("{s}", QString::number(
                                        m_recordTimer->remainingTime() / 1000 + 1)));
    }

    // Decode to normalized floats for RMS + waveform (first channel only).
    const int ch = m_micFormat.channelCount();
    QList<float> samples;
    if (m_micFormat.sampleFormat() == QAudioFormat::Int16) {
        const auto *s = reinterpret_cast<const qint16 *>(chunk.constData());
        const int n = chunk.size() / 2;
        samples.reserve(n / ch);
        for (int i = 0; i < n; i += ch)
            samples << s[i] / 32768.0f;
    } else { // Float
        const auto *s = reinterpret_cast<const float *>(chunk.constData());
        const int n = chunk.size() / 4;
        samples.reserve(n / ch);
        for (int i = 0; i < n; i += ch)
            samples << s[i];
    }
    if (samples.isEmpty())
        return;

    double sq = 0;
    for (float v : samples)
        sq += double(v) * v;
    const double rms = qSqrt(sq / samples.size());
    m_levelBar->setValue(qMin(100, int(rms * 300))); // gain for a visible bar

    // Downsample for the waveform so 20 ms adds ~40 points.
    QList<float> down;
    const int stride = qMax(1, int(samples.size()) / 40);
    for (int i = 0; i < samples.size(); i += stride)
        down << samples[i];
    m_waveform->addSamples(down);
}

// (see header)
void AudioTestModule::startRecording() {
    if (!m_micIo)
        return;
    m_recorded.clear();
    m_recordedFormat = m_micFormat;
    m_recording = true;
    m_recordBtn->setEnabled(false);
    m_playBtn->setEnabled(false);
    m_recordTimer->start();
    m_recordStatus->setText(tr_("ht_aud_recording").replace("{s}", "5"));
}

// (see header)
void AudioTestModule::playRecording() {
    if (m_recorded.isEmpty())
        return;
    stopPlayback();
    const auto device = m_outputCombo->currentData().value<QAudioDevice>();
    if (device.isNull())
        return;
    m_playbackBuffer.setData(m_recorded);
    m_playbackBuffer.open(QIODevice::ReadOnly);
    m_playbackSink = std::make_unique<QAudioSink>(device, m_recordedFormat);
    connect(m_playbackSink.get(), &QAudioSink::stateChanged, this,
            [this](QAudio::State s) {
                if (s == QAudio::IdleState || s == QAudio::StoppedState)
                    m_recordStatus->setText(tr_("ht_aud_record_done"));
            });
    m_playbackSink->start(&m_playbackBuffer);
    m_recordStatus->setText(tr_("ht_aud_playing"));
}

// (see header)
void AudioTestModule::stopPlayback() {
    if (m_playbackSink) {
        m_playbackSink->stop();
        m_playbackSink.reset();
    }
    if (m_playbackBuffer.isOpen())
        m_playbackBuffer.close();
}

// Re-applies translated texts.
void AudioTestModule::retranslate() {
    m_speakerCard->setTitle(tr_("ht_aud_speaker_title"));
    m_micCard->setTitle(tr_("ht_aud_mic_title"));
    m_melodyLabel->setText(tr_("ht_aud_melody_hint"));
    m_leftBtn->setText(tr_("ht_aud_left"));
    m_rightBtn->setText(tr_("ht_aud_right"));
    m_bothBtn->setText(tr_("ht_aud_both"));
    m_stopBtn->setText(tr_("ht_common_stop"));
    m_recordBtn->setText(tr_("ht_aud_record"));
    m_playBtn->setText(tr_("ht_aud_play"));
    if (m_recordStatus->text().isEmpty() || m_recorded.isEmpty())
        m_recordStatus->setText(tr_("ht_aud_record_hint"));
}
