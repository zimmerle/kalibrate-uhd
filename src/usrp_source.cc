/*
 * Copyright (c) 2010, Joshua Lackey
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *     *  Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *     *  Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <complex>

#include "usrp_source.h"

extern int g_verbosity;


usrp_source::usrp_source(float sample_rate,
			long int fpga_master_clock_freq,
			bool external_ref) {

	m_desired_sample_rate = sample_rate;
	m_fpga_master_clock_freq = fpga_master_clock_freq;
	m_external_ref = external_ref;
	m_sample_rate = 0.0;
	m_dev.reset();
	m_cb = new circular_buffer(CB_LEN, sizeof(complex), 0);

	pthread_mutex_init(&m_u_mutex, 0);
}


usrp_source::~usrp_source() {

	stop();
	delete m_cb;
	pthread_mutex_destroy(&m_u_mutex);
}


void usrp_source::stop() {

	pthread_mutex_lock(&m_u_mutex);
	if(m_dev) {
		uhd::stream_cmd_t cmd = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
		m_dev->issue_stream_cmd(cmd);
	}
	pthread_mutex_unlock(&m_u_mutex);
}


void usrp_source::start() {

	pthread_mutex_lock(&m_u_mutex);
	if(m_dev) {
		uhd::stream_cmd_t cmd =	uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS;
		m_dev->issue_stream_cmd(cmd);
	}
	pthread_mutex_unlock(&m_u_mutex);
}


float usrp_source::sample_rate() {

	return m_sample_rate;
}


int usrp_source::tune(double freq) {

	uhd::tune_result_t tr;

	pthread_mutex_lock(&m_u_mutex);
	tr = m_dev->set_rx_freq(freq);
	pthread_mutex_unlock(&m_u_mutex);

	return tr.actual_inter_freq + tr.actual_dsp_freq;
}


void usrp_source::set_antenna(const std::string antenna) {

	m_dev->set_rx_antenna(antenna);
}


void usrp_source::set_antenna(int antenna) {

	std::vector<std::string> antennas = get_antennas();
	if (antenna < antennas.size())
		set_antenna(antennas[antenna]);
	else 
		fprintf(stderr, "error: requested invalid antenna\n");
}


std::vector<std::string> usrp_source::get_antennas() {

	return m_dev->get_rx_antennas();
}


bool usrp_source::set_gain(float gain) {

	uhd::gain_range_t gain_range = m_dev->get_rx_gain_range();
	float min = gain_range.start(), max = gain_range.stop();

	if((gain < 0.0) || (1.0 < gain))
		return false;

	m_dev->set_rx_gain(min + gain * (max - min));

	return true;
}


/*
 * open() should be called before multiple threads access usrp_source.
 */
int usrp_source::open(unsigned int subdev) {

	if(!m_dev) {
		uhd::device_addr_t dev_addr("type=usrp2");
		if (!(m_dev = uhd::usrp::single_usrp::make(dev_addr))) {
			fprintf(stderr, "error: single_usrp::make: failed!\n");
			return -1;
		}

		m_dev->set_rx_rate(m_desired_sample_rate);
		m_sample_rate = m_dev->get_rx_rate();

		uhd::clock_config_t clock_config;
		clock_config.pps_source = uhd::clock_config_t::PPS_SMA;
		clock_config.pps_polarity = uhd::clock_config_t::PPS_NEG;

		if (m_external_ref)
			clock_config.ref_source = uhd::clock_config_t::REF_SMA;
		else
			clock_config.ref_source = uhd::clock_config_t::REF_INT;

		m_dev->set_clock_config(clock_config);

		if(g_verbosity > 1) {
			fprintf(stderr, "Sample rate: %f\n", m_sample_rate);
		}
	}

	set_gain(0.45);

	set_antenna(1);

	m_recv_samples_per_packet =
		m_dev->get_device()->get_max_recv_samps_per_packet();

	return 0;
}


int usrp_source::fill(unsigned int num_samples, unsigned int *overrun_i) {

	unsigned char ubuf[m_recv_samples_per_packet * 2 * sizeof(short)];
	short *s = (short *)ubuf;
	unsigned int i, j, space, overruns = 0;
	complex *c;

	while ((m_cb->data_available() < num_samples)
			&& m_cb->space_available() > 0) {

		uhd::rx_metadata_t metadata;

		pthread_mutex_lock(&m_u_mutex);
		size_t samples_read = m_dev->get_device()->recv((void*)ubuf,
					m_recv_samples_per_packet,
					metadata,
					uhd::io_type_t::COMPLEX_INT16,
					uhd::device::RECV_MODE_ONE_PACKET);
		pthread_mutex_unlock(&m_u_mutex);

		if (samples_read < m_recv_samples_per_packet) {
			fprintf(stderr, "error: device::recv\n");
			return -1;
		}

		// write complex<short> input to complex<float> output
		c = (complex *)m_cb->poke(&space);

		// set space to number of complex items to copy
		if(space > m_recv_samples_per_packet)
			space = m_recv_samples_per_packet;

		// write data
		for(i = 0, j = 0; i < space; i += 1, j += 2)
			c[i] = complex(s[j], s[j + 1]);

		// update cb
		m_cb->wrote(i);

	}

	// if the cb is full, we left behind data from the usb packet
	if(m_cb->space_available() == 0) {
		fprintf(stderr, "warning: local overrun\n");
		overruns++;
	}

	return 0;
}


int usrp_source::read(complex *buf, unsigned int num_samples, unsigned int *samples_read) {

	unsigned int n;

	if(fill(num_samples, 0))
		return -1;

	n = m_cb->read(buf, num_samples);

	if(samples_read)
		*samples_read = n;

	return 0;
}


/*
 * Don't hold a lock on this and use the usrp at the same time.
 */
circular_buffer *usrp_source::get_buffer() {

	return m_cb;
}


int usrp_source::flush(unsigned int flush_count) {

	m_cb->flush();
	fill(flush_count * m_recv_samples_per_packet * 2 * sizeof(short), 0);
	m_cb->flush();

	return 0;
}
