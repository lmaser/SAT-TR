// ==================================================================================
// AudioFFT â€” FFT wrapper for FFTConvolver
// Backend: JUCE dsp::FFT (uses FFTW dynamically when JUCE_DSP_USE_SHARED_FFTW=1)
// ==================================================================================

#include "AudioFFT.h"

#include <JuceHeader.h>
#include <cassert>
#include <cmath>
#include <cstring>

namespace audiofft
{

  namespace detail
  {

    class AudioFFTImpl
    {
    public:
      AudioFFTImpl() = default;
      AudioFFTImpl(const AudioFFTImpl&) = delete;
      AudioFFTImpl& operator=(const AudioFFTImpl&) = delete;
      virtual ~AudioFFTImpl() = default;
      virtual void init(size_t size) = 0;
      virtual void fft(const float* data, float* re, float* im) = 0;
      virtual void ifft(float* data, const float* re, const float* im) = 0;
    };

    constexpr bool IsPowerOf2(size_t val)
    {
      return (val == 1 || (val & (val-1)) == 0);
    }

  } // End of namespace detail


  // ================================================================
  // JUCE dsp::FFT backend â€” uses FFTW via JUCE_DSP_USE_SHARED_FFTW=1
  // when available, otherwise falls back to JUCE's built-in FFT.
  // ================================================================
  class JuceFFTImpl : public detail::AudioFFTImpl
  {
  public:
    JuceFFTImpl() = default;

    void init(size_t size) override
    {
      if (_size == size)
        return;

      _size = size;
      _complexSize = AudioFFT::ComplexSize(size);

      if (size > 0)
      {
        int order = 0;
        size_t s = size;
        while (s > 1) { s >>= 1; ++order; }

        _fft = std::make_unique<juce::dsp::FFT>(order);
        _buffer.resize(size * 2, 0.0f); // interleaved complex: [re0, im0, re1, im1, ...]
      }
      else
      {
        _fft.reset();
        _buffer.clear();
      }
    }

    void fft(const float* data, float* re, float* im) override
    {
      // JUCE expects sequential real input in first N positions
      for (size_t i = 0; i < _size; ++i)
        _buffer[i] = data[i];
      for (size_t i = _size; i < _size * 2; ++i)
        _buffer[i] = 0.0f;

      _fft->performRealOnlyForwardTransform(_buffer.data(), true);

      // Output is interleaved complex [Re0, Im0, Re1, Im1, ...] -> split to re[], im[]
      for (size_t i = 0; i < _complexSize; ++i)
      {
        re[i] = _buffer[2 * i];
        im[i] = _buffer[2 * i + 1];
      }
    }

    void ifft(float* data, const float* re, const float* im) override
    {
      // Pack into interleaved buffer
      for (size_t i = 0; i < _complexSize; ++i)
      {
        _buffer[2 * i]     = re[i];
        _buffer[2 * i + 1] = im[i];
      }
      // Zero out higher bins (if any)
      for (size_t i = _complexSize * 2; i < _size * 2; ++i)
        _buffer[i] = 0.0f;

      _fft->performRealOnlyInverseTransform(_buffer.data());

      // JUCE already normalises by 1/N — output is sequential real in first N positions
      for (size_t i = 0; i < _size; ++i)
        data[i] = _buffer[i];
    }

  private:
    size_t _size = 0;
    size_t _complexSize = 0;
    std::unique_ptr<juce::dsp::FFT> _fft;
    std::vector<float> _buffer;
  };

  // ================================================================

  AudioFFT::AudioFFT() :
    _impl(new JuceFFTImpl())
  {
  }

  AudioFFT::~AudioFFT()
  {
  }

  void AudioFFT::init(size_t size)
  {
    assert(detail::IsPowerOf2(size));
    _impl->init(size);
  }

  void AudioFFT::fft(const float* data, float* re, float* im)
  {
    _impl->fft(data, re, im);
  }

  void AudioFFT::ifft(float* data, const float* re, const float* im)
  {
    _impl->ifft(data, re, im);
  }

  size_t AudioFFT::ComplexSize(size_t size)
  {
    return (size / 2) + 1;
  }

} // End of namespace audiofft
