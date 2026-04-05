// ==================================================================================
// Copyright (c) 2017 HiFi-LoFi
// MIT License — see Utilities.h for full text
// ==================================================================================

#ifndef _FFTCONVOLVER_FFTCONVOLVER_H
#define _FFTCONVOLVER_FFTCONVOLVER_H

#include "AudioFFT.h"
#include "Utilities.h"

#include <vector>

namespace fftconvolver
{

class FFTConvolver
{
public:
  FFTConvolver();
  virtual ~FFTConvolver();

  bool init(size_t blockSize, const Sample* ir, size_t irLen);
  void process(const Sample* input, Sample* output, size_t len);
  void reset();

private:
  size_t _blockSize;
  size_t _segSize;
  size_t _segCount;
  size_t _fftComplexSize;
  std::vector<SplitComplex*> _segments;
  std::vector<SplitComplex*> _segmentsIR;
  SampleBuffer _fftBuffer;
  audiofft::AudioFFT _fft;
  SplitComplex _preMultiplied;
  SplitComplex _conv;
  SampleBuffer _overlap;
  size_t _current;
  SampleBuffer _inputBuffer;
  size_t _inputBufferFill;

  FFTConvolver(const FFTConvolver&);
  FFTConvolver& operator=(const FFTConvolver&);
};

} // End of namespace fftconvolver

#endif // _FFTCONVOLVER_FFTCONVOLVER_H
