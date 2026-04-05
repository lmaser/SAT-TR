// ==================================================================================
// Copyright (c) 2017 HiFi-LoFi
// MIT License — see Utilities.h for full text
// ==================================================================================

#ifndef _FFTCONVOLVER_TWOSTAGEFFTCONVOLVER_H
#define _FFTCONVOLVER_TWOSTAGEFFTCONVOLVER_H

#include "FFTConvolver.h"
#include "Utilities.h"

namespace fftconvolver
{

class TwoStageFFTConvolver
{
public:
  TwoStageFFTConvolver();
  virtual ~TwoStageFFTConvolver();

  bool init(size_t headBlockSize, size_t tailBlockSize, const Sample* ir, size_t irLen);
  void process(const Sample* input, Sample* output, size_t len);
  void reset();

protected:
  virtual void startBackgroundProcessing();
  virtual void waitForBackgroundProcessing();
  void doBackgroundProcessing();

private:
  size_t _headBlockSize;
  size_t _tailBlockSize;
  FFTConvolver _headConvolver;
  FFTConvolver _tailConvolver0;
  SampleBuffer _tailOutput0;
  SampleBuffer _tailPrecalculated0;
  FFTConvolver _tailConvolver;
  SampleBuffer _tailOutput;
  SampleBuffer _tailPrecalculated;
  SampleBuffer _tailInput;
  size_t _tailInputFill;
  size_t _precalculatedPos;
  SampleBuffer _backgroundProcessingInput;

  TwoStageFFTConvolver(const TwoStageFFTConvolver&);
  TwoStageFFTConvolver& operator=(const TwoStageFFTConvolver&);
};

} // End of namespace fftconvolver

#endif // _FFTCONVOLVER_TWOSTAGEFFTCONVOLVER_H
