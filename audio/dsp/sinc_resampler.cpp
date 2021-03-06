/* Copyright (c) 2017-2019 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

// Rewrote the RetroArch Sinc resampler for my purpose here.

/* Copyright  (C) 2010-2019 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (sinc_resampler.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Copyright  (C) 2010-2019 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (filters.h).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Copyright  (C) 2010-2019 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (memalign.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#if defined(_WIN32) && !defined(__SSE__)
#define __SSE__
#endif

#include "sinc_resampler.hpp"
#include <cmath>
#include <cstdlib>

#ifndef PI
#define PI 3.14159265359
#endif

#if defined(__SSE__)
#include <xmmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace Granite
{
namespace Audio
{
namespace DSP
{
static void *memalign_alloc(size_t boundary, size_t size)
{
	void **place = NULL;
	uintptr_t addr = 0;
	void *ptr = malloc(boundary + size + sizeof(uintptr_t));
	if (!ptr)
		return NULL;

	addr = ((uintptr_t)ptr + sizeof(uintptr_t) + boundary) & ~(boundary - 1);
	place = (void**)addr;
	place[-1] = ptr;

	return (void*)addr;
}

static void memalign_free(void *ptr)
{
	void **p = NULL;
	if (!ptr)
		return;

	p = (void**)ptr;
	free(p[-1]);
}

/* Modified Bessel function of first order.
 * Check Wiki for mathematical definition ... */
static inline double besseli0(double x)
{
	double sum = 0.0;
	double factorial = 1.0;
	double factorial_mult = 0.0;
	double x_pow = 1.0;
	double two_div_pow = 1.0;
	double x_sqr = x * x;

	/* Approximate. This is an infinite sum.
	 * Luckily, it converges rather fast. */
	for (unsigned i = 0; i < 18; i++)
	{
		sum += x_pow * two_div_pow / (factorial * factorial);

		factorial_mult += 1.0;
		x_pow *= x_sqr;
		two_div_pow *= 0.25;
		factorial *= factorial_mult;
	}

	return sum;
}

static inline double sinc(double val)
{
	if (std::fabs(val) < 0.00001)
		return 1.0;
	return sin(val) / val;
}

static inline double kaiser_window_function(double index, double beta)
{
	return besseli0(beta * std::sqrt(1.0 - index * index));
}

void SincResampler::init_table_kaiser(double cutoff, unsigned phases, unsigned taps, double beta)
{
	double window_mod = kaiser_window_function(0.0, beta);
	const unsigned stride = 2;
	double sidelobes = taps / 2.0;

	for (unsigned i = 0; i < phases; i++)
	{
		for (unsigned j = 0; j < taps; j++)
		{
			unsigned n = j * phases + i;
			double window_phase = double(n) / double(phases * taps); /* [0, 1). */
			window_phase = 2.0 * window_phase - 1.0; /* [-1, 1) */
			double sinc_phase = sidelobes * window_phase;
			float val = float(cutoff * sinc(PI * sinc_phase * cutoff) * kaiser_window_function(window_phase, beta) / window_mod);
			phase_table[i * stride * taps + j] = val;
		}
	}

	for (unsigned p = 0; p < phases - 1; p++)
	{
		for (unsigned j = 0; j < taps; j++)
		{
			float delta = phase_table[(p + 1) * stride * taps + j] - phase_table[p * stride * taps + j];
			phase_table[(p * stride + 1) * taps + j] = delta;
		}
	}

	unsigned phase = phases - 1;
	for (unsigned j = 0; j < taps; j++)
	{
		unsigned n = j * phases + (phase + 1);
		double window_phase = double(n) / double(phases * taps); /* (0, 1]. */
		window_phase = 2.0 * window_phase - 1.0; /* (-1, 1] */
		double sinc_phase = sidelobes * window_phase;

		float val = float(cutoff * sinc(PI * sinc_phase * cutoff) * kaiser_window_function(window_phase, beta) / window_mod);
		float delta = (val - phase_table[phase * stride * taps + j]);
		phase_table[(phase * stride + 1) * taps + j] = delta;
	}
}

SincResampler::SincResampler(float out_rate, float in_rate, Quality quality)
{
	double cutoff;
	unsigned sidelobes;
	double kaiser_beta;

	switch (quality)
	{
	case Quality::Low:
		cutoff = 0.80;
		sidelobes = 4;
		kaiser_beta = 4.5;
		phase_bits = 12;
		subphase_bits = 10;
		break;

	case Quality::Medium:
		cutoff = 0.825;
		sidelobes = 8;
		kaiser_beta = 5.5;
		phase_bits = 8;
		subphase_bits = 16;
		break;

	case Quality::High:
		cutoff = 0.90;
		sidelobes = 32;
		kaiser_beta   = 10.5;
		phase_bits = 10;
		subphase_bits = 14;
		break;

	default:
		std::abort();
	}

	subphase_mask = (1u << subphase_bits) - 1u;
	subphase_mod  = 1.0f / (1u << subphase_bits);
	taps = sidelobes * 2;
	float bandwidth_mod = out_rate / in_rate;

	/* Downsampling, must lower cutoff, and extend number of
	 * taps accordingly to keep same stopband attenuation. */
	if (bandwidth_mod < 1.0f)
	{
		cutoff *= bandwidth_mod;
		taps = unsigned(ceil(taps / bandwidth_mod));
	}

	/* Be SIMD-friendly. */
	taps = (taps + 3) & ~3;

	unsigned phase_elems = ((1u << phase_bits) * taps);
	phase_elems = phase_elems * 2;
	unsigned elems = phase_elems + 2 * taps;

	main_buffer = (float*)memalign_alloc(128, sizeof(float) * elems);
	if (!main_buffer)
		throw std::bad_alloc();

	phase_table = main_buffer;
	window_buffer = main_buffer + phase_elems;

	init_table_kaiser(cutoff, 1u << phase_bits, taps, kaiser_beta);

	float ratio = out_rate / in_rate;
	phases = 1u << (phase_bits + subphase_bits);
	fixed_ratio = uint32_t(round(phases / ratio));
}

SincResampler::~SincResampler()
{
	memalign_free(main_buffer);
}

size_t SincResampler::get_maximum_input_for_output_frames(size_t out_frames) const noexcept
{
	uint64_t max_start_time = phases - 1;
	max_start_time += uint64_t(fixed_ratio) * out_frames;
	max_start_time >>= phase_bits + subphase_bits;
	return size_t(max_start_time);
}

size_t SincResampler::get_current_input_for_output_frames(size_t out_frames) const noexcept
{
	uint64_t start_time = time;
	start_time += uint64_t(fixed_ratio) * out_frames;
	start_time >>= phase_bits + subphase_bits;
	return size_t(start_time);
}

size_t SincResampler::process_and_accumulate(float *output, const float *input, size_t out_frames) noexcept
{
	uint32_t ratio = fixed_ratio;

	size_t consumed_frames = 0;
	while (out_frames)
	{
		// Pump out samples.
		while (out_frames && time < phases)
		{
			const float *buffer = window_buffer + ptr;
			unsigned taps = this->taps;
			unsigned phase = time >> subphase_bits;

			const float *phase_table = this->phase_table + phase * taps * 2;
			const float *delta_table = phase_table + taps;

#ifdef __SSE__
			__m128 sum = _mm_setzero_ps();
			__m128 delta = _mm_set1_ps(float(time & subphase_mask) * subphase_mod);
			for (unsigned i = 0; i < taps; i += 4)
			{
				__m128 buf = _mm_loadu_ps(buffer + i);
				__m128 deltas = _mm_load_ps(delta_table + i);
				__m128 _sinc  = _mm_add_ps(_mm_load_ps(phase_table + i), _mm_mul_ps(deltas, delta));
				sum = _mm_add_ps(sum, _mm_mul_ps(buf, _sinc));
			}

			// Horizontal add.
			sum = _mm_add_ps(_mm_shuffle_ps(sum, sum, _MM_SHUFFLE(2, 3, 2, 3)), sum);
			sum = _mm_add_ss(_mm_shuffle_ps(sum, sum, _MM_SHUFFLE(1, 1, 1, 1)), sum);
			_mm_store_ss(output, _mm_add_ss(_mm_load_ss(output), sum));
#elif defined(__ARM_NEON)
			float delta = float(time & subphase_mask) * subphase_mod;
			float32x4_t sum = vdupq_n_f32(0.0f);
			for (unsigned i = 0; i < taps; i += 4)
			{
				float32x4_t phases = vld1q_f32(phase_table + i);
				float32x4_t deltas = vld1q_f32(delta_table + i);
				float32x4_t buf = vld1q_f32(buffer + i);
				float32x4_t _sinc = vfmaq_n_f32(phases, deltas, delta);
				sum = vfmaq_f32(sum, buf, _sinc);
			}

			float32x2_t half = vadd_f32(vget_low_f32(sum), vget_high_f32(sum));
			float32x2_t res = vpadd_f32(half, half);
			float32x2_t o = vld1_dup_f32(output);
			res = vadd_f32(o, res);
			vst1_lane_f32(output, res, 0);
#else
			float delta = float(time & subphase_mask) * subphase_mod;
			float sum = 0.0f;
			for (unsigned i = 0; i < taps; i++)
			{
				float sinc_val = phase_table[i] + delta_table[i] * delta;
				sum += buffer[i] * sinc_val;
			}
			*output += sum;
#endif

			output++;
			out_frames--;
			time += ratio;
		}

		// Drain inputs.
		while (time >= phases)
		{
			/* Push in reverse to make filter more obvious. */
			if (!ptr)
				ptr = taps;
			ptr--;

			window_buffer[ptr + taps] = input[consumed_frames];
			window_buffer[ptr] = input[consumed_frames];
			consumed_frames++;
			time -= phases;
		}
	}

	return consumed_frames;
}

}
}
}