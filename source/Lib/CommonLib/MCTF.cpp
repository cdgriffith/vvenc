/* -----------------------------------------------------------------------------
The copyright in this software is being made available under the BSD
License, included below. No patent rights, trademark rights and/or 
other Intellectual Property Rights other than the copyrights concerning 
the Software are granted under this license.

For any license concerning other Intellectual Property rights than the software,
especially patent licenses, a separate Agreement needs to be closed. 
For more information please contact:

Fraunhofer Heinrich Hertz Institute
Einsteinufer 37
10587 Berlin, Germany
www.hhi.fraunhofer.de/vvc
vvc@hhi.fraunhofer.de

Copyright (c) 2019-2021, Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
 * Neither the name of Fraunhofer nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
THE POSSIBILITY OF SUCH DAMAGE.


------------------------------------------------------------------------------------------- */


/** \file     MCTF.cpp
\brief    MCTF class
*/

#include "MCTF.h"
#include <math.h>
#include "CommonLib/Picture.h"
#include "CommonLib/dtrace_buffer.h"
#include "Utilities/NoMallocThreadPool.h"

namespace vvenc {

#ifdef TRACE_ENABLE_ITT
static __itt_string_handle* itt_handle_est = __itt_string_handle_create( "MCTF_est" );
static __itt_domain* itt_domain_MCTF_est   = __itt_domain_create( "MCTFEst" );
static __itt_string_handle* itt_handle_flt = __itt_string_handle_create( "MCTF_flt" );
static __itt_domain* itt_domain_MCTF_flt   = __itt_domain_create( "MCTFFlt" );
#endif

// ====================================================================================================================
// Constructor / destructor / initialization / destroy
// ====================================================================================================================

const double MCTF::m_chromaFactor     =  0.55;
const double MCTF::m_sigmaMultiplier  =  9.0;
const double MCTF::m_sigmaZeroPoint   = 10.0;
const int MCTF::m_range               = VVENC_MCTF_RANGE;
const int MCTF::m_motionVectorFactor  = 16;
const int MCTF::m_padding             = MCTF_PADDING;
const int16_t MCTF::m_interpolationFilter[16][8] =
{
  {   0,   0,   0,  64,   0,   0,   0,   0 },   //0
  {   0,   1,  -3,  64,   4,  -2,   0,   0 },   //1 -->-->
  {   0,   1,  -6,  62,   9,  -3,   1,   0 },   //2 -->
  {   0,   2,  -8,  60,  14,  -5,   1,   0 },   //3 -->-->
  {   0,   2,  -9,  57,  19,  -7,   2,   0 },   //4
  {   0,   3, -10,  53,  24,  -8,   2,   0 },   //5 -->-->
  {   0,   3, -11,  50,  29,  -9,   2,   0 },   //6 -->
  {   0,   3, -11,  44,  35, -10,   3,   0 },   //7 -->-->
  {   0,   1,  -7,  38,  38,  -7,   1,   0 },   //8
  {   0,   3, -10,  35,  44, -11,   3,   0 },   //9 -->-->
  {   0,   2,  -9,  29,  50, -11,   3,   0 },   //10-->
  {   0,   2,  -8,  24,  53, -10,   3,   0 },   //11-->-->
  {   0,   2,  -7,  19,  57,  -9,   2,   0 },   //12
  {   0,   1,  -5,  14,  60,  -8,   2,   0 },   //13-->-->
  {   0,   1,  -3,   9,  62,  -6,   1,   0 },   //14-->
  {   0,   0,  -2,   4,  64,  -3,   1,   0 }    //15-->-->
};

const double MCTF::m_refStrengths[3][4] =
{ // abs(POC offset)
  //  1,    2     3     4
  {0.85, 0.57, 0.41, 0.33},  // m_range * 2
  {1.13, 0.97, 0.81, 0.57},  // m_range
  {0.30, 0.30, 0.30, 0.30}   // otherwise
};

int motionErrorLumaInt( const Pel* origOrigin, const ptrdiff_t origStride, const Pel* buffOrigin, const ptrdiff_t buffStride, const int bs, const int x, const int y, const int dx, const int dy, const int besterror )
{
  int error = 0;

  for( int y1 = 0; y1 < bs; y1++ )
  {
    const Pel* origRowStart = origOrigin + ( y + y1 )*origStride + x;
    const Pel* bufferRowStart = buffOrigin + ( y + y1 + dy )*buffStride + ( x + dx );
    for( int x1 = 0; x1 < bs; x1 += 2 )
    {
      int diff = origRowStart[x1] - bufferRowStart[x1];
      error += diff * diff;
      diff = origRowStart[x1 + 1] - bufferRowStart[x1 + 1];
      error += diff * diff;
    }
    if( error > besterror )
    {
      return error;
    }
  }

  return error;
}

int motionErrorLumaFrac( const Pel* origOrigin, const ptrdiff_t origStride, const Pel* buffOrigin, const ptrdiff_t buffStride, const int bs, const int x, const int y, const int dx, const int dy, const int16_t* xFilter, const int16_t* yFilter, const int bitDepth, const int besterror )
{
  int error = 0;
  Pel tempArray[64 + 8][64];
  int sum, base;
  const Pel maxSampleValue = ( 1 << bitDepth ) - 1;

  for( int y1 = 1; y1 < bs + 7; y1++ )
  {
    const int yOffset = y + y1 + ( dy >> 4 ) - 3;
    const Pel* sourceRow = buffOrigin + ( yOffset ) *buffStride + 0;
    for( int x1 = 0; x1 < bs; x1++ )
    {
      sum = 0;
      base = x + x1 + ( dx >> 4 ) - 3;
      const Pel* rowStart = sourceRow + base;

      sum += xFilter[1] * rowStart[1];
      sum += xFilter[2] * rowStart[2];
      sum += xFilter[3] * rowStart[3];
      sum += xFilter[4] * rowStart[4];
      sum += xFilter[5] * rowStart[5];
      sum += xFilter[6] * rowStart[6];

      sum = ( sum + ( 1 << 5 ) ) >> 6;
      sum = sum < 0 ? 0 : ( sum > maxSampleValue ? maxSampleValue : sum );

      tempArray[y1][x1] = sum;
    }
  }

  for( int y1 = 0; y1 < bs; y1++ )
  {
    const Pel* origRow = origOrigin + ( y + y1 )*origStride + 0;
    for( int x1 = 0; x1 < bs; x1++ )
    {
      sum = 0;
      sum += yFilter[1] * tempArray[y1 + 1][x1];
      sum += yFilter[2] * tempArray[y1 + 2][x1];
      sum += yFilter[3] * tempArray[y1 + 3][x1];
      sum += yFilter[4] * tempArray[y1 + 4][x1];
      sum += yFilter[5] * tempArray[y1 + 5][x1];
      sum += yFilter[6] * tempArray[y1 + 6][x1];

      sum = ( sum + ( 1 << 5 ) ) >> 6;
      sum = sum < 0 ? 0 : ( sum > maxSampleValue ? maxSampleValue : sum );

      error += ( sum - origRow[x + x1] ) * ( sum - origRow[x + x1] );
    }
    if( error > besterror )
    {
      return error;
    }
  }

  return error;
}

MCTF::MCTF()
  : m_encCfg    ( nullptr )
  , m_threadPool( nullptr )
  , m_filterPoc ( 0 )
{
  m_motionErrorLumaIntX  = motionErrorLumaInt;
  m_motionErrorLumaInt8  = motionErrorLumaInt;
  m_motionErrorLumaFracX = motionErrorLumaFrac;
  m_motionErrorLumaFrac8 = motionErrorLumaFrac;

#if defined( TARGET_SIMD_X86 ) && ENABLE_SIMD_OPT_MCTF
  initMCTF_X86();
#endif
}

MCTF::~MCTF()
{
}

void MCTF::init( const VVEncCfg& encCfg, NoMallocThreadPool* threadPool )
{
  CHECK( encCfg.m_vvencMCTF.numFrames != encCfg.m_vvencMCTF.numStrength, "should have been checked before" );

  m_encCfg     = &encCfg;
  m_threadPool = threadPool;
  m_area       = Area( 0, 0, m_encCfg->m_PadSourceWidth, m_encCfg->m_PadSourceHeight );
  m_filterPoc  = 0;

  const uint8_t acMCTFSpeedVal[] = {0, 5, 6, 22, 26 };
  m_MCTFSpeedVal = acMCTFSpeedVal[ m_encCfg->m_vvencMCTF.MCTFSpeed ];
}

// ====================================================================================================================
// Public member functions
// ====================================================================================================================


void MCTF::initPicture( Picture* pic )
{
  pic->getOrigBuf().extendBorderPel( MCTF_PADDING, MCTF_PADDING );
  pic->setSccFlags( m_encCfg );
}

void MCTF::processPictures( const PicList& picList, bool flush, AccessUnitList& auList, PicList& doneList, PicList& freeList )
{
  // filter one picture (either all or up to frames to be encoded)
  if( picList.size()
      && m_filterPoc <= picList.back()->poc
      && ( m_encCfg->m_framesToBeEncoded <= 0 || m_filterPoc < m_encCfg->m_framesToBeEncoded ) )
  {
    // setup fifo of pictures to be filtered
    std::deque<Picture*> picFifo;
    int filterIdx = 0;
    for( auto pic : picList )
    {
      const int minPoc = m_filterPoc - VVENC_MCTF_RANGE;
      const int maxPoc = m_encCfg->m_vvencMCTF.MCTFFutureReference ? m_filterPoc + VVENC_MCTF_RANGE : m_filterPoc;
      if( pic->poc >= minPoc && pic->poc <= maxPoc )
      {
        picFifo.push_back( pic );
        if( pic->poc < m_filterPoc )
        {
          filterIdx += 1;
        }
      }
    }
    CHECK( picFifo.empty(), "MCTF: no pictures to be filtered found" );
    CHECK( filterIdx >= (int)picFifo.size(), "MCTF: picture filter error" );
    CHECK( picFifo[ filterIdx ]->poc != m_filterPoc, "MCTF: picture filter error" );
    // filter picture (when more than 1 picture is available for processing)
    if( picFifo.size() > 1 )
    {
      filter( picFifo, filterIdx );
    }
    // set picture done
    doneList.push_back( picFifo[ filterIdx ] );
  }

  // mark pictures not needed anymore
  for( auto pic : picList )
  {
    if( pic->poc > m_filterPoc - VVENC_MCTF_RANGE )
      break;
    freeList.push_back( pic );
  }
  m_filterPoc += 1;
}


void MCTF::filter( const std::deque<Picture*>& picFifo, int filterIdx )
{
  double overallStrength = -1.0;
  bool isFilterThisFrame = false;
  int idx = (int)m_encCfg->m_vvencMCTF.numFrames - 1;
  for( ; idx >= 0; idx-- )
  {
    if ( m_filterPoc % m_encCfg->m_vvencMCTF.MCTFFrames[ idx ] == 0 )
    {
      overallStrength   = m_encCfg->m_vvencMCTF.MCTFStrengths[ idx ];
      isFilterThisFrame = true;
      break;
    }
  }

  int dropFrames = 0;
  if( idx >= 0 )
  {
    // m_MCTFSpeedVal is specified for m_FiterFrames.size() == 3, with keyframe being idx == 2
    // for m_FiterFrames.size() > 3, this is not a problem, since less important frames will be sped-up for
    // low values for idx, and keyframe in idx == 3 or higher will get threshold == 0, i.e. full filtering
    // for m_FiterFrames.size() < 3 (e.g. GOP16), the value of idx has to shifted so that keyframe is at idx == 2
    if( m_encCfg->m_vvencMCTF.numFrames < 3 ) idx += ( 3 - ( int ) m_encCfg->m_vvencMCTF.numFrames );

    int threshold     = ( m_MCTFSpeedVal >> ( idx * 2 ) ) & 3;
    isFilterThisFrame =   threshold < 2;
    dropFrames        = ( threshold & 1 ) << 1;
  }

  const int filterFrames = VVENC_MCTF_RANGE - dropFrames;

  int dropFramesFront = std::min( std::max(                                          filterIdx - filterFrames, 0 ), dropFrames );
  int dropFramesBack  = std::min( std::max( static_cast<int>( picFifo.size() ) - 1 - filterIdx - filterFrames, 0 ), dropFrames );

  Picture* pic = picFifo[ filterIdx ];
  if( ! pic->useScMCTF )
  {
    isFilterThisFrame = false;
  }

  if ( isFilterThisFrame )
  {
    const PelStorage& origBuf = pic->getOrigBuffer();
          PelStorage& fltrBuf = pic->getFilteredOrigBuffer();

    // subsample original picture so it only needs to be done once
    PelStorage origSubsampled2;
    PelStorage origSubsampled4;
    subsampleLuma( origBuf,         origSubsampled2 );
    subsampleLuma( origSubsampled2, origSubsampled4 );

    // determine motion vectors
    std::deque<TemporalFilterSourcePicInfo> srcFrameInfo;
    for ( int i = dropFramesFront; i < picFifo.size() - dropFramesBack; i++ )
    {
      Picture* curPic = picFifo[ i ];
      if ( curPic->poc == m_filterPoc )
      {
        continue;
      }
      srcFrameInfo.push_back( TemporalFilterSourcePicInfo() );
      TemporalFilterSourcePicInfo &srcPic = srcFrameInfo.back();

      srcPic.picBuffer.createFromBuf( curPic->getOrigBuf() );
      srcPic.mvs.allocate( m_area.width / 4, m_area.height / 4 );

      {
        const int width = m_area.width;
        const int height = m_area.height;
        Array2D<MotionVector> mv_0(width / 64, height / 64);
        Array2D<MotionVector> mv_1(width / 32, height / 32);
        Array2D<MotionVector> mv_2(width / 16, height / 16);

        PelStorage bufferSub2;
        PelStorage bufferSub4;

        subsampleLuma(srcPic.picBuffer, bufferSub2);
        subsampleLuma(bufferSub2, bufferSub4);

        motionEstimationLuma(mv_0, origSubsampled4, bufferSub4, 16);
        motionEstimationLuma(mv_1, origSubsampled2, bufferSub2, 16, &mv_0, 2);
        motionEstimationLuma(mv_2, origBuf, srcPic.picBuffer, 16, &mv_1, 2);

        motionEstimationLuma(srcPic.mvs, origBuf, srcPic.picBuffer, 8, &mv_2, 1, true);
      }

      srcPic.index = std::min(3, std::abs(curPic->poc - m_filterPoc) - 1);
    }

    // filter
    fltrBuf.create( m_encCfg->m_internChromaFormat, m_area, 0, m_padding );
    bilateralFilter( origBuf, srcFrameInfo, fltrBuf, overallStrength );
  }
}

// ====================================================================================================================
// Private member functions
// ====================================================================================================================

void MCTF::subsampleLuma(const PelStorage &input, PelStorage &output, const int factor) const
{
  const int newWidth = input.Y().width / factor;
  const int newHeight = input.Y().height / factor;
  output.create(CHROMA_400, Area(0, 0, newWidth, newHeight), 0, m_padding);

  const Pel* srcRow = input.Y().buf;
  const int srcStride = input.Y().stride;
  Pel* dstRow = output.Y().buf;
  const int dstStride = output.Y().stride;

  for (int y = 0; y < newHeight; y++, srcRow+=factor*srcStride, dstRow+=dstStride)
  {
    const Pel* inRow      = srcRow;
    const Pel* inRowBelow = srcRow+srcStride;
    Pel* target     = dstRow;

    for (int x = 0; x < newWidth; x++)
    {
      target[x] = (inRow[0] + inRowBelow[0] + inRow[1] + inRowBelow[1] + 2) >> 2;
      inRow += 2;
      inRowBelow += 2;
    }
  }
  output.extendBorderPel(m_padding, m_padding);
}

int MCTF::motionErrorLuma(const PelStorage &orig,
  const PelStorage &buffer,
  const int x,
  const int y,
  int dx,
  int dy,
  const int bs,
  const int besterror = 8 * 8 * 1024 * 1024) const
{
  const Pel* origOrigin = orig.Y().buf;
  const int origStride  = orig.Y().stride;
  const Pel* buffOrigin = buffer.Y().buf;
  const int buffStride  = buffer.Y().stride;

  int error = 0;// dx * 10 + dy * 10;
  if (((dx | dy) & 0xF) == 0)
  {
    dx /= m_motionVectorFactor;
    dy /= m_motionVectorFactor;

    if( bs & 7 )
    {
      return m_motionErrorLumaIntX( origOrigin, origStride, buffOrigin, buffStride, bs, x, y, dx, dy, besterror );
    }
    else
    {
      return m_motionErrorLumaInt8( origOrigin, origStride, buffOrigin, buffStride, bs, x, y, dx, dy, besterror );
    }
  }
  else
  {
    const int16_t *xFilter = m_interpolationFilter[dx & 0xF];
    const int16_t *yFilter = m_interpolationFilter[dy & 0xF];

    if( bs & 7 )
    {
      return m_motionErrorLumaFracX( origOrigin, origStride, buffOrigin, buffStride, bs, x, y, dx, dy, xFilter, yFilter, m_encCfg->m_internalBitDepth[CH_L], besterror );
    }
    else
    {
      return m_motionErrorLumaFrac8( origOrigin, origStride, buffOrigin, buffStride, bs, x, y, dx, dy, xFilter, yFilter, m_encCfg->m_internalBitDepth[CH_L], besterror );
    }
  }
  return error;
}

bool MCTF::estimateLumaLn( std::atomic_int& blockX_, std::atomic_int* prevLineX, Array2D<MotionVector> &mvs, const PelStorage &orig, const PelStorage &buffer, const int blockSize,
  const Array2D<MotionVector> *previous, const int factor, const bool doubleRes, int blockY ) const
{
  const int stepSize = blockSize;
  const int origWidth  = orig.Y().width;

  for( int blockX = blockX_.load(); blockX + blockSize <= origWidth; blockX += stepSize, blockX_.store( blockX) )
  {
    if( prevLineX && blockX >= prevLineX->load() ) return false;

    int range = doubleRes ? 0 : 5;
    const int stepSize = blockSize;

    MotionVector best;

    if (previous == NULL)
    {
      range = 8;
    }
    else
    {
      for( int py = -1; py <= 1; py++ )
      {
        int testy = blockY / (2 * blockSize) + py;
        if( (testy >= 0) && (testy < previous->h()) )
        {
          for (int px = -1; px <= 1; px++)
          {
            int testx = blockX / (2 * blockSize) + px;
            if ((testx >= 0) && (testx < previous->w()) )
            {
              const MotionVector& old = previous->get(testx, testy);
              int error = motionErrorLuma(orig, buffer, blockX, blockY, old.x * factor, old.y * factor, blockSize, best.error);
              if (error < best.error)
              {
                best.set(old.x * factor, old.y * factor, error);
              }
            }
          }
        }
      }

      int error = motionErrorLuma( orig, buffer, blockX, blockY, 0, 0, blockSize, best.error );
      if( error < best.error )
      {
        best.set( 0, 0, error );
      }
    }
    MotionVector prevBest = best;
    for (int y2 = prevBest.y / m_motionVectorFactor - range; y2 <= prevBest.y / m_motionVectorFactor + range; y2++)
    {
      for (int x2 = prevBest.x / m_motionVectorFactor - range; x2 <= prevBest.x / m_motionVectorFactor + range; x2++)
      {
        int error = motionErrorLuma(orig, buffer, blockX, blockY, x2 * m_motionVectorFactor, y2 * m_motionVectorFactor, blockSize, best.error);
        if (error < best.error)
        {
          best.set(x2 * m_motionVectorFactor, y2 * m_motionVectorFactor, error);
        }
      }
    }
    if (doubleRes)
    { // merge into one loop, probably with precision array (here [12, 3] or maybe [4, 1]) with setable number of iterations
      prevBest = best;
      int doubleRange = 3 * 4;
      for (int y2 = prevBest.y - doubleRange; y2 <= prevBest.y + doubleRange; y2 += 4)
      {
        for (int x2 = prevBest.x - doubleRange; x2 <= prevBest.x + doubleRange; x2 += 4)
        {
          int error = motionErrorLuma(orig, buffer, blockX, blockY, x2, y2, blockSize, best.error);
          if (error < best.error)
          {
            best.set(x2, y2, error);
          }
        }
      }

      prevBest = best;
      doubleRange = 3;
      for (int y2 = prevBest.y - doubleRange; y2 <= prevBest.y + doubleRange; y2++)
      {
        for (int x2 = prevBest.x - doubleRange; x2 <= prevBest.x + doubleRange; x2++)
        {
          int error = motionErrorLuma(orig, buffer, blockX, blockY, x2, y2, blockSize, best.error);
          if (error < best.error)
          {
            best.set(x2, y2, error);
          }
        }
      }
    } 
    if( blockY > 0 )
    {
      MotionVector aboveMV = mvs.get( blockX / stepSize, ( blockY - stepSize ) / stepSize );
      int error = motionErrorLuma( orig, buffer, blockX, blockY, aboveMV.x, aboveMV.y, blockSize, best.error );
      if( error < best.error )
      {
        best.set( aboveMV.x, aboveMV.y, error );
      }
    }
    if( blockX > 0 )
    {
      MotionVector leftMV = mvs.get( ( blockX - stepSize ) / stepSize, blockY / stepSize );
      int error = motionErrorLuma( orig, buffer, blockX, blockY, leftMV.x, leftMV.y, blockSize, best.error );
      if( error < best.error )
      {
        best.set( leftMV.x, leftMV.y, error );
      }
    }

    // calculate average
    double avg = 0.0;
    for( int y1 = 0; y1 < blockSize; y1++ )
    {
      for( int x1 = 0; x1 < blockSize; x1++ )
      {
        avg = avg + orig.Y().at( blockX + x1, blockY + y1 );
      }
    }
    avg = avg / ( blockSize * blockSize );

    // calculate variance
    double variance = 0;
    for( int y1 = 0; y1 < blockSize; y1++ )
    {
      for( int x1 = 0; x1 < blockSize; x1++ )
      {
        int pix = orig.Y().at( blockX + x1, blockY + y1 );
        variance = variance + ( pix - avg ) * ( pix - avg );
      }
    }
    best.error = ( int ) ( 20 * ( ( best.error + 5.0 ) / ( variance + 5.0 ) ) + ( best.error / ( blockSize * blockSize ) ) / 50 );

    mvs.get(blockX / stepSize, blockY / stepSize) = best;
  }

  return true;
}

void MCTF::motionEstimationLuma(Array2D<MotionVector> &mvs, const PelStorage &orig, const PelStorage &buffer, const int blockSize, const Array2D<MotionVector> *previous, const int factor, const bool doubleRes) const
{
  const int stepSize = blockSize;
  const int origHeight = orig.Y().height;

  if( m_threadPool )
  {
    struct EstParams
    {
      std::atomic_int blockX;
      std::atomic_int* prevLineX;
      Array2D<MotionVector> *mvs;
      const PelStorage* orig; 
      const PelStorage* buffer; 
      const Array2D<MotionVector> *previous; 
      int   blockSize; 
      int   factor; 
      bool  doubleRes;
      int   blockY;
      const MCTF* mctf;
    };

    std::vector<EstParams> EstParamsArray( origHeight/stepSize);

    WaitCounter taskCounter;

    for( int n = 0, blockY = 0; blockY + blockSize <= origHeight; blockY += stepSize, n++ )
    {
      static auto task = []( int tId, EstParams* params)
      {
        ITT_TASKSTART( itt_domain_MCTF_est, itt_handle_est );

        bool ret = params->mctf->estimateLumaLn( params->blockX, params->prevLineX, *params->mvs, *params->orig, *params->buffer, params->blockSize, params->previous, params->factor, params->doubleRes, params->blockY );

        ITT_TASKEND( itt_domain_MCTF_est, itt_handle_est );
        return ret;
      };

      EstParams& cEstParams = EstParamsArray[n];
      cEstParams.blockX = 0;
      cEstParams.prevLineX = n == 0 ? nullptr : &EstParamsArray[n-1].blockX;
      cEstParams.mvs = &mvs; 
      cEstParams.orig = &orig;
      cEstParams.buffer = &buffer; 
      cEstParams.previous = previous;
      cEstParams.blockSize = blockSize; 
      cEstParams.factor = factor;
      cEstParams.doubleRes = doubleRes;
      cEstParams.mctf = this;
      cEstParams.blockY = blockY;

      m_threadPool->addBarrierTask<EstParams>( task, &cEstParams, &taskCounter);
    }
    taskCounter.wait();
  }
  else
  {
    for( int blockY = 0; blockY + blockSize <= origHeight; blockY += stepSize )
    {
      std::atomic_int blockX( 0 ), prevBlockX( orig.Y().width + stepSize );
      estimateLumaLn( blockX, blockY ? &prevBlockX : nullptr, mvs, orig, buffer, blockSize, previous, factor, doubleRes, blockY );
    }

  }
}

void MCTF::applyMotionLn(const Array2D<MotionVector> &mvs, const PelStorage &input, PelStorage &output, int blockNumY, int comp ) const
{
  static const int lumaBlockSize=8;

  const ComponentID compID=(ComponentID)comp;
  const int csx=getComponentScaleX(compID, m_encCfg->m_internChromaFormat);
  const int csy=getComponentScaleY(compID, m_encCfg->m_internChromaFormat);
  const int blockSizeX = lumaBlockSize>>csx;
  const int blockSizeY = lumaBlockSize>>csy;
  const int width  = input.bufs[compID].width;
  int y = blockNumY*blockSizeY;
  const Pel maxValue = (1<<m_encCfg->m_internalBitDepth[toChannelType(compID)])-1;

  const Pel* srcImage = input.bufs[compID].buf;
  const int srcStride  = input.bufs[compID].stride;

  Pel* dstImage = output.bufs[compID].buf;
  const int dstStride  = output.bufs[compID].stride;

  for (int x = 0, blockNumX = 0; x + blockSizeX <= width; x += blockSizeX, blockNumX++)
  {
    const MotionVector &mv = mvs.get(blockNumX,blockNumY);
    const int dx = mv.x >> csx ;
    const int dy = mv.y >> csy ;
    const int xInt = mv.x >> (4+csx) ;
    const int yInt = mv.y >> (4+csy) ;

    const int16_t *xFilter = m_interpolationFilter[dx & 0xf];
    const int16_t *yFilter = m_interpolationFilter[dy & 0xf]; // will add 6 bit.
    const int numFilterTaps=7;
    const int centreTapOffset=3;

    Pel tempArray[lumaBlockSize + numFilterTaps][lumaBlockSize];

    for (int by = 1; by < blockSizeY + numFilterTaps; by++)
    {
      const int yOffset = y + by + yInt - centreTapOffset;
      const Pel* sourceRow = srcImage+yOffset*srcStride;
      for (int bx = 0; bx < blockSizeX; bx++)
      {
        int base = x + bx + xInt - centreTapOffset;
        const Pel* rowStart = sourceRow + base;

        int sum = 0;
        sum += xFilter[1] * rowStart[1];
        sum += xFilter[2] * rowStart[2];
        sum += xFilter[3] * rowStart[3];
        sum += xFilter[4] * rowStart[4];
        sum += xFilter[5] * rowStart[5];
        sum += xFilter[6] * rowStart[6];

        sum = ( sum + ( 1 << 5 ) ) >> 6;
        tempArray[by][bx] = sum;
      }
    }

    Pel* dstRow = dstImage+y*dstStride;
    for (int by = 0; by < blockSizeY; by++, dstRow+=dstStride)
    {
      Pel* dstPel=dstRow+x;
      for (int bx = 0; bx < blockSizeX; bx++, dstPel++)
      {
        int sum = 0;

        sum += yFilter[1] * tempArray[by + 1][bx];
        sum += yFilter[2] * tempArray[by + 2][bx];
        sum += yFilter[3] * tempArray[by + 3][bx];
        sum += yFilter[4] * tempArray[by + 4][bx];
        sum += yFilter[5] * tempArray[by + 5][bx];
        sum += yFilter[6] * tempArray[by + 6][bx];

        sum = ( sum + ( 1 << 5 ) ) >> 6;
        sum = sum < 0 ? 0 : (sum > maxValue ? maxValue : sum);
        *dstPel = sum;
      }
    }
  }
}


inline static double fastExp( double x )
{
  // using the e^x ~= ( 1 + x/n )^n for n -> inf
  x = 1.0 + x / 1024;
  x *= x; x *= x; x *= x; x *= x;
  x *= x; x *= x; x *= x; x *= x;
  x *= x; x *= x;
  return x;
}

void MCTF::xFinalizeBlkLine( const PelStorage &orgPic, std::deque<TemporalFilterSourcePicInfo> &srcFrameInfo, PelStorage &newOrgPic,
  std::vector<PelStorage>& correctedPics, int yStart, const double sigmaSqCh[MAX_NUM_CH], double overallStrength ) const
{
  const int numRefs = int(srcFrameInfo.size());

  int refStrengthRow = 2;
  if( numRefs == m_range * 2 )
  {
    refStrengthRow = 0;
  }
  else if( numRefs == m_range )
  {
    refStrengthRow = 1;
  }

  for(int c=0; c< getNumberValidComponents(m_encCfg->m_internChromaFormat); c++)
  {
    for (int i = 0; i < numRefs; i++)
    {
      applyMotionLn(srcFrameInfo[i].mvs, srcFrameInfo[i].picBuffer, correctedPics[i], yStart / 8, c);
    }

    const ComponentID compID=(ComponentID)c;
    const int height = orgPic.bufs[c].height;
    const int width  = orgPic.bufs[c].width;
    const int srcStride = orgPic.bufs[c].stride;
    const int dstStride = newOrgPic.bufs[c].stride;

    const double sigmaSq = sigmaSqCh[ toChannelType( compID) ];
    const double weightScaling = overallStrength * ( isChroma( compID ) ? m_chromaFactor : 0.4 );
    const Pel maxSampleValue = (1<<m_encCfg->m_internalBitDepth[ toChannelType( compID) ])-1;

    const int blkSizeY = 8 >> getComponentScaleY(compID, m_encCfg->m_internChromaFormat);
    const int blkSizeX = 8 >> getComponentScaleX(compID, m_encCfg->m_internChromaFormat);
    int yOut = yStart >> getComponentScaleY(compID, m_encCfg->m_internChromaFormat);
    const Pel* srcPelRow = orgPic.bufs[c].buf + yOut * srcStride;
    Pel* dstPelRow = newOrgPic.bufs[c].buf + yOut * dstStride;
    for (int y = yOut; y < std::min(yOut+blkSizeY,height); y++, srcPelRow+=srcStride, dstPelRow+=dstStride)
    {
      const int yBlkAddr = y / blkSizeY;

      const Pel* srcPel=srcPelRow;
      Pel* dstPel=dstPelRow;

      double minError = 9999999;

      for (int x = 0; x < width; x++, srcPel++, dstPel++)
      {
        const int xBlkAddr = x / blkSizeX;
        const int orgVal = (int) *srcPel;
        double temporalWeightSum = 1.0;
        double newVal = (double) orgVal;
        if( ( y % blkSizeY == 0 ) && ( x % blkSizeX == 0 ) )
        {
          for( int i = 0; i < numRefs; i++ )
          {
            const PelBuf& corrBuf = correctedPics[i].bufs[c];
            int64_t variance = 0, diffsum = 0;
            for( int y1 = 0; y1 < blkSizeY - 1; y1++ )
            {
              for( int x1 = 0; x1 < blkSizeX - 1; x1++ )
              {
                int pix =  srcPel[x1];
                int pixR = srcPel[x1 + 1];
                int pixD = srcPel[x1 + srcStride];
                int ref =  corrBuf.buf[( ( y + y1     ) * corrBuf.stride + x + x1 )];
                int refR = corrBuf.buf[( ( y + y1     ) * corrBuf.stride + x + x1 + 1 )];
                int refD = corrBuf.buf[( ( y + y1 + 1 ) * corrBuf.stride + x + x1 )];

                int diff = pix - ref;
                int diffR = pixR - refR;
                int diffD = pixD - refD;

                variance += diff * diff;
                diffsum += ( diffR - diff ) * ( diffR - diff );
                diffsum += ( diffD - diff ) * ( diffD - diff );
              }
            }
            srcFrameInfo[i].mvs.get( xBlkAddr, yBlkAddr ).noise = ( int ) round( ( 300 * variance + 50 ) / ( 10 * diffsum + 50 ) );
          }
        }
        if( x % blkSizeX == 0 )
        {
          minError = 9999999;
          for( int i = 0; i < numRefs; i++ )
          {
            minError = std::min( minError, ( double ) srcFrameInfo[i].mvs.get( xBlkAddr, yBlkAddr ).error );
          }
        }

        for (int i = 0; i < numRefs; i++)
        {
          const MotionVector& mv = srcFrameInfo[i].mvs.get( xBlkAddr, yBlkAddr );
          const int error = mv.error;
          const int noise = mv.noise;
          const Pel*   pCorrectedPelPtr=correctedPics[i].bufs[c].buf+(y*correctedPics[i].bufs[c].stride+x);
          const int    refVal = (int) *pCorrectedPelPtr;
          const double diff   = (double)(refVal - orgVal);
          const double diffSq = diff * diff;

          double ww = 1, sw = 1;
          ww *= ( noise < 25 ) ? 1.0 : 0.6;
          sw *= ( noise < 25 ) ? 1.0 : 0.8;
          ww *= ( error < 50 ) ? 1.2 : ( ( error > 100 ) ? 0.6 : 1.0 );
          sw *= ( error < 50 ) ? 1.0 : 0.8;
          ww *= ( ( minError + 1 ) / ( error + 1 ) );
          const int index = srcFrameInfo[i].index;
          double weight = weightScaling * m_refStrengths[refStrengthRow][index] * ww * fastExp( -diffSq / ( 2 * sw * sigmaSq ) );
          newVal += weight * refVal;
          temporalWeightSum  += weight;
        }
        newVal /= temporalWeightSum;
        Pel sampleVal = (Pel)round(newVal);
        sampleVal=(sampleVal<0?0 : (sampleVal>maxSampleValue ? maxSampleValue : sampleVal));
        *dstPel = sampleVal;
      }
    }
  }
}

void MCTF::bilateralFilter(const PelStorage& orgPic, std::deque<TemporalFilterSourcePicInfo>& srcFrameInfo, PelStorage& newOrgPic, double overallStrength) const
{
  const int numRefs = int(srcFrameInfo.size());

  const double lumaSigmaSq = (m_encCfg->m_QP - m_sigmaZeroPoint) * (m_encCfg->m_QP - m_sigmaZeroPoint) * m_sigmaMultiplier;
  const double chromaSigmaSq = 30 * 30;

  double sigmaSqCh[MAX_NUM_CH];
  for(int c=0; c< getNumberValidChannels(m_encCfg->m_internChromaFormat); c++)
  {
    const ChannelType ch=(ChannelType)c;
    const Pel maxSampleValue = (1<<m_encCfg->m_internalBitDepth[ch])-1;
    const double bitDepthDiffWeighting=1024.0 / (maxSampleValue+1);
    sigmaSqCh[ch] = (isChroma(ch)? chromaSigmaSq : lumaSigmaSq)/(bitDepthDiffWeighting*bitDepthDiffWeighting);
  }

#
  std::vector<PelStorage> correctedPics(numRefs);
  for (int i = 0; i < numRefs; i++)
  {
    correctedPics[i].create(m_encCfg->m_internChromaFormat, m_area, 0, m_padding);
  }

  if( m_threadPool )
  {
    struct FltParams
    {
      const PelStorage *orgPic; 
      std::deque<TemporalFilterSourcePicInfo> *srcFrameInfo; 
      PelStorage *newOrgPic;
      std::vector<PelStorage>* correctedPics;
      const double *sigmaSqCh;
      double overallStrength;
      const MCTF* mctf;
      int yStart; 
    };

    std::vector<FltParams> FltParamsArray( orgPic.Y().height/8 );

    WaitCounter taskCounter;

    for (int n = 0, yStart = 0; yStart < orgPic.Y().height; yStart += 8, n++)
    {
      static auto task = []( int tId, FltParams* params)
      {
        ITT_TASKSTART( itt_domain_MCTF_flt, itt_handle_flt );

        params->mctf->xFinalizeBlkLine( *params->orgPic, *params->srcFrameInfo, *params->newOrgPic, *params->correctedPics, params->yStart, params->sigmaSqCh, params->overallStrength );

        ITT_TASKEND( itt_domain_MCTF_flt, itt_handle_flt );
        return true;
      };

      FltParams& cFltParams = FltParamsArray[n];
      cFltParams.orgPic = &orgPic; 
      cFltParams.srcFrameInfo = &srcFrameInfo; 
      cFltParams.newOrgPic = &newOrgPic;
      cFltParams.correctedPics = &correctedPics;
      cFltParams.sigmaSqCh = sigmaSqCh;
      cFltParams.overallStrength = overallStrength;
      cFltParams.mctf = this;
      cFltParams.yStart = yStart;

      m_threadPool->addBarrierTask<FltParams>( task, &cFltParams, &taskCounter);
    }
    taskCounter.wait();
  }
  else
  {
    for (int yStart = 0; yStart < orgPic.Y().height; yStart += 8)
    {
      xFinalizeBlkLine( orgPic, srcFrameInfo, newOrgPic, correctedPics, yStart, sigmaSqCh, overallStrength );
    }
  }
}

} // namespace vvenc

//! \}
