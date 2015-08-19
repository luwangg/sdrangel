///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2012 maintech GmbH, Otto-Hahn-Str. 15, 97204 Hoechberg, Germany //
// written by Christian Daniel                                                   //
// (c) 2014 Modified by John Greb
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <QTime>
#include <QDebug>
#include <stdio.h>
#include "ssbdemod.h"
#include "audio/audiooutput.h"
#include "dsp/channelizer.h"

MESSAGE_CLASS_DEFINITION(SSBDemod::MsgConfigureSSBDemod, Message)

SSBDemod::SSBDemod(AudioFifo* audioFifo, SampleSink* sampleSink) :
	m_sampleSink(sampleSink),
	m_audioFifo(audioFifo)
{
	setObjectName("SSBDemod");

	m_Bandwidth = 5000;
	m_LowCutoff = 300;
	m_volume = 2.0;
	m_spanLog2 = 3;
	m_sampleRate = 96000;
	m_frequency = 0;
	m_nco.setFreq(m_frequency, m_sampleRate);
	m_interpolator.create(16, m_sampleRate, 5000);
	m_sampleDistanceRemain = (Real)m_sampleRate / 48000.0;

	m_audioBuffer.resize(512);
	m_audioBufferFill = 0;
	m_undersampleCount = 0;

	m_usb = true;
	SSBFilter = new fftfilt(m_LowCutoff / 48000.0, m_Bandwidth / 48000.0, ssbFftLen);
	// if (!USBFilter) segfault;
}

SSBDemod::~SSBDemod()
{
	if (SSBFilter) delete SSBFilter;
}

void SSBDemod::configure(MessageQueue* messageQueue, Real Bandwidth, Real LowCutoff, Real volume, int spanLog2)
{
	Message* cmd = MsgConfigureSSBDemod::create(Bandwidth, LowCutoff, volume, spanLog2);
	messageQueue->push(cmd);
}

void SSBDemod::feed(SampleVector::const_iterator begin, SampleVector::const_iterator end, bool positiveOnly)
{
	Complex ci;
	fftfilt::cmplx *sideband, sum;
	Real avg;
	int n_out;
	int decim = 1<<(m_spanLog2 - 1);
	unsigned char decim_mask = decim - 1; // counter LSB bit mask for decimation by 2^(m_scaleLog2 - 1)

	for(SampleVector::const_iterator it = begin; it < end; ++it)
	{
		Complex c(it->real() / 32768.0, it->imag() / 32768.0);
		c *= m_nco.nextIQ();

		if(m_interpolator.interpolate(&m_sampleDistanceRemain, c, &ci))
		{
			n_out = SSBFilter->runSSB(ci, &sideband, m_usb);
			m_sampleDistanceRemain += (Real)m_sampleRate / 48000.0;
		}
		else
		{
			n_out = 0;
		}

		for (int i = 0; i < n_out; i++)
		{
			Real demod = (sideband[i].real() + sideband[i].imag()) * 0.7 * 32768.0;

			// Downsample by 2^(m_scaleLog2 - 1) for SSB band spectrum display
			// smart decimation with bit gain using float arithmetic (23 bits significand)

			sum += sideband[i];

			if (!(m_undersampleCount++ & decim_mask))
			{
				avg = (sum.real() + sum.imag()) * 0.7 * 32768.0 / decim;
				m_sampleBuffer.push_back(Sample(avg, 0.0));
				sum.real() = 0.0;
				sum.imag() = 0.0;
			}

			qint16 sample = (qint16)(demod * m_volume * 10);
			m_audioBuffer[m_audioBufferFill].l = sample;
			m_audioBuffer[m_audioBufferFill].r = sample;
			++m_audioBufferFill;

			if (m_audioBufferFill >= m_audioBuffer.size())
			{
				uint res = m_audioFifo->write((const quint8*)&m_audioBuffer[0], m_audioBufferFill, 1);

				if (res != m_audioBufferFill)
				{
					qDebug("lost %u samples", m_audioBufferFill - res);
				}

				m_audioBufferFill = 0;
			}
		}
	}

	if (m_audioFifo->write((const quint8*)&m_audioBuffer[0], m_audioBufferFill, 0) != m_audioBufferFill)
	{
		qDebug("SSBDemod::feed: lost samples");
	}
	m_audioBufferFill = 0;

	if(m_sampleSink != 0)
	{
		m_sampleSink->feed(m_sampleBuffer.begin(), m_sampleBuffer.end(), true);
	}

	m_sampleBuffer.clear();
}

void SSBDemod::start()
{
}

void SSBDemod::stop()
{
}

bool SSBDemod::handleMessage(const Message& cmd)
{
	float band, lowCutoff;

	qDebug() << "SSBDemod::handleMessage";

	if (Channelizer::MsgChannelizerNotification::match(cmd))
	{
		Channelizer::MsgChannelizerNotification& notif = (Channelizer::MsgChannelizerNotification&) cmd;

		m_sampleRate = notif.getSampleRate();
		m_nco.setFreq(-notif.getFrequencyOffset(), m_sampleRate);
		m_interpolator.create(16, m_sampleRate, m_Bandwidth);
		m_sampleDistanceRemain = m_sampleRate / 48000.0;

		qDebug() << "SSBDemod::handleMessage: MsgChannelizerNotification: m_sampleRate: " << m_sampleRate
				<< " frequencyOffset" << notif.getFrequencyOffset();

		return true;
	}
	else if (MsgConfigureSSBDemod::match(cmd))
	{
		MsgConfigureSSBDemod& cfg = (MsgConfigureSSBDemod&) cmd;

		band = cfg.getBandwidth();
		lowCutoff = cfg.getLoCutoff();

		if (band < 0) {
			band = -band;
			lowCutoff = -lowCutoff;
			m_usb = false;
		} else
			m_usb = true;

		if (band < 100.0f)
		{
			band = 100.0f;
			lowCutoff = 0;
		}

		m_Bandwidth = band;
		m_LowCutoff = lowCutoff;

		m_interpolator.create(16, m_sampleRate, band * 2.0f);
		SSBFilter->create_filter(m_LowCutoff / 48000.0f, m_Bandwidth / 48000.0f);

		m_volume = cfg.getVolume();
		m_volume *= m_volume * 0.1;

		m_spanLog2 = cfg.getSpanLog2();

		qDebug() << "  - MsgConfigureSSBDemod: m_Bandwidth: " << m_Bandwidth
				<< " m_LowCutoff: " << m_LowCutoff
				<< " m_volume: " << m_volume
				<< " m_spanLog2: " << m_spanLog2;

		return true;
	}
	else
	{
		if(m_sampleSink != 0)
		{
		   return m_sampleSink->handleMessage(cmd);
		}
		else
		{
			return false;
		}
	}
}
