// ==================================================================================
// Copyright (c) 2017 HiFi-LoFi
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// ==================================================================================

#ifndef _AUDIOFFT_H
#define _AUDIOFFT_H

#include <cstddef>
#include <memory>

namespace audiofft
{

  namespace detail
  {
    class AudioFFTImpl;
  }

  class AudioFFT
  {
  public:
    AudioFFT();
    AudioFFT(const AudioFFT&) = delete;
    AudioFFT& operator=(const AudioFFT&) = delete;
    ~AudioFFT();

    void init(size_t size);
    void fft(const float* data, float* re, float* im);
    void ifft(float* data, const float* re, const float* im);
    static size_t ComplexSize(size_t size);

  private:
    std::unique_ptr<detail::AudioFFTImpl> _impl;
  };

  typedef AudioFFT AudioFFTBase;

} // End of namespace audiofft

#endif // _AUDIOFFT_H
