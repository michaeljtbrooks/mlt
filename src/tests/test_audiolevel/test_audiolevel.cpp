/*
 * Copyright (C) 2026 Meltytech, LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with consumer library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <framework/mlt.h>
#include <math.h>
#include <mlt++/Mlt.h>
#include <string.h>
#include <QtTest>
using namespace Mlt;

/** Sits below the filter under test and records the format it was asked for.
 *
 * The format a filter imposes is only visible from underneath: the caller's own
 * mlt_frame_get_audio() converts back to what the caller wanted, which hides it.
 */
static int probe_get_audio(mlt_frame frame,
                           void **buffer,
                           mlt_audio_format *format,
                           int *frequency,
                           int *channels,
                           int *samples)
{
    mlt_properties p = MLT_FRAME_PROPERTIES(frame);
    mlt_properties_set_int(p, "_probe_format", *format);
    *buffer = mlt_properties_get_data(p, "audio", NULL);
    *format = (mlt_audio_format) mlt_properties_get_int(p, "audio_format");
    *frequency = mlt_properties_get_int(p, "audio_frequency");
    *channels = mlt_properties_get_int(p, "audio_channels");
    *samples = mlt_properties_get_int(p, "audio_samples");
    return 0;
}

class TestAudioLevel : public QObject
{
    Q_OBJECT

    static const int CHANNELS = 2;
    static const int SAMPLES = 1000;

public:
    TestAudioLevel() { Factory::init(); }

    ~TestAudioLevel() { Factory::close(); }

private:
    /** Run the audiolevel filter over one buffer and report what it measured.
     *
     * Returns the peak level in dBFS for channel 0, and writes back the format
     * the frame ended up in so a caller can check the filter left it alone.
     */
    double measure(const void *audio, mlt_audio_format format, mlt_audio_format *asked_for)
    {
        Profile profile;
        int size = mlt_audio_format_size(format, SAMPLES, CHANNELS);
        mlt_frame frame = mlt_frame_init(NULL);
        mlt_properties properties = MLT_FRAME_PROPERTIES(frame);
        mlt_properties_set_int(properties, "audio_channels", CHANNELS);
        mlt_properties_set_int(properties, "audio_samples", SAMPLES);
        mlt_properties_set_int(properties, "audio_frequency", 48000);

        void *buffer = mlt_pool_alloc(size);
        memcpy(buffer, audio, size);
        mlt_frame_set_audio(frame, buffer, format, size, mlt_pool_release);

        mlt_frame_push_audio(frame, (void *) probe_get_audio);

        mlt_filter filter = mlt_factory_filter(profile.get_profile(), "audiolevel", NULL);
        // Report plain dBFS rather than the IEC meter curve.
        mlt_properties_set_int(MLT_FILTER_PROPERTIES(filter), "dbpeak", 1);
        mlt_properties_set_int(MLT_FILTER_PROPERTIES(filter), "iec_scale", 0);
        mlt_filter_process(filter, frame);

        void *out = NULL;
        mlt_audio_format fmt = format;
        int frequency = 48000, channels = CHANNELS, samples = SAMPLES;
        mlt_frame_get_audio(frame, &out, &fmt, &frequency, &channels, &samples);
        if (asked_for)
            *asked_for = (mlt_audio_format) mlt_properties_get_int(properties, "_probe_format");

        double level = mlt_properties_get_double(properties, "meta.media.audio_level.0");
        mlt_filter_close(filter);
        mlt_frame_close(frame);
        return level;
    }

    // Full-scale-referenced sine at the given peak, in each format.
    void fill_s16(int16_t *p, double peak)
    {
        for (int s = 0; s < SAMPLES; s++)
            for (int c = 0; c < CHANNELS; c++)
                p[c + s * CHANNELS] = (int16_t) lrint(peak * 32768.0
                                                      * sin(2.0 * M_PI * 1000.0 * s / 48000.0));
    }
    void fill_f32(float *p, double peak)
    {
        for (int s = 0; s < SAMPLES; s++)
            for (int c = 0; c < CHANNELS; c++)
                p[c + s * CHANNELS] = (float) (peak * sin(2.0 * M_PI * 1000.0 * s / 48000.0));
    }

private Q_SLOTS:

    /** The filter must not impose a format on the pipeline.
     *
     * It used to set *format = mlt_audio_s16 unconditionally. Because audio
     * format propagates backwards through the filter stack, that re-imposed a
     * 16-bit bus on every filter below it, clipping anything above 0 dBFS.
     */
    void DoesNotForcePipelineFormat()
    {
        QVector<float> in(SAMPLES * CHANNELS);
        fill_f32(in.data(), 0.5);
        mlt_audio_format asked_for = mlt_audio_none;
        measure(in.data(), mlt_audio_f32le, &asked_for);
        QVERIFY2(asked_for == mlt_audio_f32le,
                 qPrintable(QString("filter asked the chain below it for %1, not f32le")
                                .arg(mlt_audio_format_name(asked_for))));
    }

    /** A -6 dBFS sine must read as -6 dBFS. */
    void ReportsCorrectPeak()
    {
        QVector<float> in(SAMPLES * CHANNELS);
        fill_f32(in.data(), 0.5);
        QVERIFY(qAbs(measure(in.data(), mlt_audio_f32le, NULL) - (-6.0)) < 0.1);
    }

    /** The same signal must measure the same in any format. */
    void AgreesAcrossFormats()
    {
        QVector<float> f32(SAMPLES * CHANNELS);
        QVector<int16_t> s16(SAMPLES * CHANNELS);
        fill_f32(f32.data(), 0.5);
        fill_s16(s16.data(), 0.5);
        double a = measure(f32.data(), mlt_audio_f32le, NULL);
        double b = measure(s16.data(), mlt_audio_s16, NULL);
        QVERIFY2(qAbs(a - b) < 0.01,
                 qPrintable(QString("f32le read %1 dBFS, s16 read %2 dBFS").arg(a).arg(b)));
    }

    /** Levels above 0 dBFS must be reported, not clipped away. */
    void ReportsAboveFullScale()
    {
        QVector<float> in(SAMPLES * CHANNELS);
        fill_f32(in.data(), 2.0);
        double level = measure(in.data(), mlt_audio_f32le, NULL);
        QVERIFY2(level > 5.0, qPrintable(QString("expected about +6 dBFS, got %1").arg(level)));
    }
};

QTEST_APPLESS_MAIN(TestAudioLevel)

#include "test_audiolevel.moc"
